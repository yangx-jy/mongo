/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/transaction_oplog_application.h"

#include "mongo/db/background.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/timestamp_block.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/util/log.h"

namespace mongo {
using repl::OplogEntry;
namespace {
// If enabled, causes _applyPrepareTransaction to hang before preparing the transaction participant.
MONGO_FAIL_POINT_DEFINE(applyOpsHangBeforePreparingTransaction);

// Failpoint that will cause reconstructPreparedTransactions to return early.
MONGO_FAIL_POINT_DEFINE(skipReconstructPreparedTransactions);


// Apply the oplog entries for a prepare or a prepared commit during recovery/initial sync.
Status _applyOperationsForTransaction(OperationContext* opCtx,
                                      const repl::MultiApplier::Operations& ops,
                                      repl::OplogApplication::Mode oplogApplicationMode) {
    // Apply each the operations via repl::applyOperation.
    for (const auto& op : ops) {
        try {
            AutoGetCollection coll(opCtx, op.getNss(), MODE_IX);
            auto status = repl::applyOperation_inlock(
                opCtx, coll.getDb(), &op, false /*alwaysUpsert*/, oplogApplicationMode);
            if (!status.isOK()) {
                return status;
            }
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
            if (oplogApplicationMode != repl::OplogApplication::Mode::kInitialSync &&
                oplogApplicationMode != repl::OplogApplication::Mode::kRecovering)
                throw;
        }
    }
    return Status::OK();
}

/**
 * Helper that will read the entire sequence of oplog entries for the transaction and apply each of
 * them.
 *
 * Currently used for oplog application of a commitTransaction oplog entry during recovery and
 * rollback.
 */
Status _applyTransactionFromOplogChain(OperationContext* opCtx,
                                       const OplogEntry& entry,
                                       repl::OplogApplication::Mode mode,
                                       Timestamp commitTimestamp,
                                       Timestamp durableTimestamp) {
    invariant(mode == repl::OplogApplication::Mode::kRecovering);

    auto ops = readTransactionOperationsFromOplogChain(opCtx, entry, {});

    const auto dbName = entry.getNss().db().toString();
    Status status = Status::OK();

    writeConflictRetry(opCtx, "replaying prepared transaction", dbName, [&] {
        WriteUnitOfWork wunit(opCtx);

        // we might replay a prepared transaction behind oldest timestamp.
        opCtx->recoveryUnit()->setRoundUpPreparedTimestamps(true);

        BSONObjBuilder resultWeDontCareAbout;

        status = _applyOperationsForTransaction(opCtx, ops, mode);
        if (status.isOK()) {
            opCtx->recoveryUnit()->setPrepareTimestamp(commitTimestamp);
            wunit.prepare();

            // Calls setCommitTimestamp() to set commit timestamp of the transaction and
            // clears the commit timestamp in the recovery unit when tsBlock goes out of the
            // scope. It is necessary that we clear the commit timestamp because there can be
            // another transaction in the same recovery unit calling setTimestamp().
            TimestampBlock tsBlock(opCtx, commitTimestamp);
            opCtx->recoveryUnit()->setDurableTimestamp(durableTimestamp);
            wunit.commit();
        }
    });
    return status;
}
}  // namespace

/**
 * Helper used to get previous oplog entry from the same transaction.
 */
const repl::OplogEntry getPreviousOplogEntry(OperationContext* opCtx,
                                             const repl::OplogEntry& entry) {
    const auto prevOpTime = entry.getPrevWriteOpTimeInTransaction();
    invariant(prevOpTime);
    TransactionHistoryIterator iter(prevOpTime.get());
    invariant(iter.hasNext());
    const auto prevOplogEntry = iter.next(opCtx);

    return prevOplogEntry;
}


Status applyCommitTransaction(OperationContext* opCtx,
                              const OplogEntry& entry,
                              repl::OplogApplication::Mode mode) {
    IDLParserErrorContext ctx("commitTransaction");
    auto commitOplogEntryOpTime = entry.getOpTime();
    auto commitCommand = CommitTransactionOplogObject::parse(ctx, entry.getObject());
    invariant(commitCommand.getCommitTimestamp());

    switch (mode) {
        case repl::OplogApplication::Mode::kRecovering: {
            return _applyTransactionFromOplogChain(opCtx,
                                                   entry,
                                                   mode,
                                                   *commitCommand.getCommitTimestamp(),
                                                   commitOplogEntryOpTime.getTimestamp());
        }
        case repl::OplogApplication::Mode::kInitialSync: {
            // Initial sync should never apply 'commitTransaction' since it unpacks committed
            // transactions onto various applier threads.
            MONGO_UNREACHABLE;
        }
        case repl::OplogApplication::Mode::kApplyOpsCmd: {
            // Return error if run via applyOps command.
            uasserted(50987, "commitTransaction is only used internally by secondaries.");
        }
        case repl::OplogApplication::Mode::kSecondary: {
            // Transaction operations are in its own batch, so we can modify their opCtx.
            invariant(entry.getSessionId());
            invariant(entry.getTxnNumber());
            opCtx->setLogicalSessionId(*entry.getSessionId());
            opCtx->setTxnNumber(*entry.getTxnNumber());

            // The write on transaction table may be applied concurrently, so refreshing state
            // from disk may read that write, causing starting a new transaction on an existing
            // txnNumber. Thus, we start a new transaction without refreshing state from disk.
            MongoDOperationContextSessionWithoutRefresh sessionCheckout(opCtx);

            auto transaction = TransactionParticipant::get(opCtx);
            invariant(transaction);
            transaction.unstashTransactionResources(opCtx, "commitTransaction");
            transaction.commitPreparedTransaction(
                opCtx, *commitCommand.getCommitTimestamp(), commitOplogEntryOpTime);
            return Status::OK();
        }
    }
    MONGO_UNREACHABLE;
}

Status applyAbortTransaction(OperationContext* opCtx,
                             const OplogEntry& entry,
                             repl::OplogApplication::Mode mode) {
    switch (mode) {
        case repl::OplogApplication::Mode::kRecovering: {
            // We don't put transactions into the prepare state until the end of recovery,
            // so there is no transaction to abort.
            return Status::OK();
        }
        case repl::OplogApplication::Mode::kInitialSync: {
            // We don't put transactions into the prepare state until the end of initial sync,
            // so there is no transaction to abort.
            return Status::OK();
        }
        case repl::OplogApplication::Mode::kApplyOpsCmd: {
            // Return error if run via applyOps command.
            uasserted(50972, "abortTransaction is only used internally by secondaries.");
        }
        case repl::OplogApplication::Mode::kSecondary: {
            // Transaction operations are in its own batch, so we can modify their opCtx.
            invariant(entry.getSessionId());
            invariant(entry.getTxnNumber());
            opCtx->setLogicalSessionId(*entry.getSessionId());
            opCtx->setTxnNumber(*entry.getTxnNumber());

            // The write on transaction table may be applied concurrently, so refreshing state
            // from disk may read that write, causing starting a new transaction on an existing
            // txnNumber. Thus, we start a new transaction without refreshing state from disk.
            MongoDOperationContextSessionWithoutRefresh sessionCheckout(opCtx);

            auto transaction = TransactionParticipant::get(opCtx);
            transaction.unstashTransactionResources(opCtx, "abortTransaction");
            transaction.abortTransaction(opCtx);
            return Status::OK();
        }
    }
    MONGO_UNREACHABLE;
}

repl::MultiApplier::Operations readTransactionOperationsFromOplogChain(
    OperationContext* opCtx,
    const OplogEntry& lastEntryInTxn,
    const std::vector<OplogEntry*>& cachedOps) noexcept {
    // Traverse the oplog chain with its own snapshot and read timestamp.
    ReadSourceScope readSourceScope(opCtx);

    repl::MultiApplier::Operations ops;

    // The cachedOps are the ops for this transaction that are from the same oplog application batch
    // as the commit or prepare, those which have not necessarily been written to the oplog.  These
    // ops are in order of increasing timestamp.
    const auto oldestEntryInBatch = cachedOps.empty() ? lastEntryInTxn : *cachedOps.front();

    // The lastEntryWrittenToOplogOpTime is the OpTime of the latest entry for this transaction
    // which is expected to be present in the oplog.  It is the entry before the first cachedOp,
    // unless there are no cachedOps in which case it is the entry before the commit or prepare.
    const auto lastEntryWrittenToOplogOpTime = oldestEntryInBatch.getPrevWriteOpTimeInTransaction();
    invariant(lastEntryWrittenToOplogOpTime < lastEntryInTxn.getOpTime());

    TransactionHistoryIterator iter(lastEntryWrittenToOplogOpTime.get());

    // If we started with a prepared commit, we want to forget about that operation and move onto
    // the prepare.
    auto prepareOrUnpreparedCommit = lastEntryInTxn;
    if (lastEntryInTxn.isPreparedCommit()) {
        // A prepared-commit must be in its own batch and thus have no cached ops.
        invariant(cachedOps.empty());
        invariant(iter.hasNext());
        prepareOrUnpreparedCommit = iter.nextFatalOnErrors(opCtx);
    }
    invariant(prepareOrUnpreparedCommit.getCommandType() == OplogEntry::CommandType::kApplyOps);

    // The non-DurableReplOperation fields of the extracted transaction operations will match those
    // of the lastEntryInTxn. For a prepared commit, this will include the commit oplog entry's
    // 'ts' field, which is what we want.
    auto lastEntryInTxnObj = lastEntryInTxn.toBSON();

    // First retrieve and transform the ops from the oplog, which will be retrieved in reverse
    // order.
    while (iter.hasNext()) {
        const auto& operationEntry = iter.nextFatalOnErrors(opCtx);
        invariant(operationEntry.isPartialTransaction());
        auto prevOpsEnd = ops.size();
        repl::ApplyOps::extractOperationsTo(operationEntry, lastEntryInTxnObj, &ops);

        // Because BSONArrays do not have fast way of determining size without iterating through
        // them, and we also have no way of knowing how many oplog entries are in a transaction
        // without iterating, reversing each applyOps and then reversing the whole array is
        // about as good as we can do to get the entire thing in chronological order.  Fortunately
        // STL arrays of BSON objects should be fast to reverse (just pointer copies).
        std::reverse(ops.begin() + prevOpsEnd, ops.end());
    }
    std::reverse(ops.begin(), ops.end());

    // Next retrieve and transform the ops from the current batch, which are in increasing timestamp
    // order.
    for (auto* cachedOp : cachedOps) {
        const auto& operationEntry = *cachedOp;
        invariant(operationEntry.isPartialTransaction());
        repl::ApplyOps::extractOperationsTo(operationEntry, lastEntryInTxnObj, &ops);
    }

    // Reconstruct the operations from the prepare or unprepared commit oplog entry.
    repl::ApplyOps::extractOperationsTo(prepareOrUnpreparedCommit, lastEntryInTxnObj, &ops);
    return ops;
}

namespace {
/**
 * This is the part of applyPrepareTransaction which is common to steady state, initial sync and
 * recovery oplog application.
 */
Status _applyPrepareTransaction(OperationContext* opCtx,
                                const OplogEntry& entry,
                                repl::OplogApplication::Mode mode) {

    // The operations here are reconstructed at their prepare time.  However, that time will
    // be ignored because there is an outer write unit of work during their application.
    // The prepare time of the transaction is set explicitly below.
    auto ops = readTransactionOperationsFromOplogChain(opCtx, entry, {});

    if (mode == repl::OplogApplication::Mode::kRecovering ||
        mode == repl::OplogApplication::Mode::kInitialSync) {
        // We might replay a prepared transaction behind oldest timestamp.  Note that since this is
        // scoped to the storage transaction, and readTransactionOperationsFromOplogChain implicitly
        // abandons the storage transaction when it releases the global lock, this must be done
        // after readTransactionOperationsFromOplogChain.
        opCtx->recoveryUnit()->setRoundUpPreparedTimestamps(true);
    }

    // Block application of prepare oplog entries on secondaries when a concurrent background index
    // build is running.
    // This will prevent hybrid index builds from corrupting an index on secondary nodes if a
    // prepared transaction becomes prepared during a build but commits after the index build
    // commits.
    for (const auto& op : ops) {
        auto ns = op.getNss();
        auto uuid = *op.getUuid();
        if (BackgroundOperation::inProgForNs(ns)) {
            warning() << "blocking replication until index builds are finished on "
                      << redact(ns.toString()) << ", due to prepared transaction";
            BackgroundOperation::awaitNoBgOpInProgForNs(ns);
            IndexBuildsCoordinator::get(opCtx)->awaitNoIndexBuildInProgressForCollection(uuid);
        }
    }

    // Transaction operations are in their own batch, so we can modify their opCtx.
    invariant(entry.getSessionId());
    invariant(entry.getTxnNumber());
    opCtx->setLogicalSessionId(*entry.getSessionId());
    opCtx->setTxnNumber(*entry.getTxnNumber());
    // The write on transaction table may be applied concurrently, so refreshing state
    // from disk may read that write, causing starting a new transaction on an existing
    // txnNumber. Thus, we start a new transaction without refreshing state from disk.
    MongoDOperationContextSessionWithoutRefresh sessionCheckout(opCtx);

    auto transaction = TransactionParticipant::get(opCtx);
    transaction.unstashTransactionResources(opCtx, "prepareTransaction");

    // Set this in case the application of any ops need to use the prepare timestamp of this
    // transaction. It should be cleared automatically when the transaction finishes.
    if (mode == repl::OplogApplication::Mode::kRecovering) {
        transaction.setPrepareOpTimeForRecovery(opCtx, entry.getOpTime());
    }

    auto status = _applyOperationsForTransaction(opCtx, ops, mode);
    fassert(31137, status);

    if (MONGO_FAIL_POINT(applyOpsHangBeforePreparingTransaction)) {
        LOG(0) << "Hit applyOpsHangBeforePreparingTransaction failpoint";
        MONGO_FAIL_POINT_PAUSE_WHILE_SET_OR_INTERRUPTED(opCtx,
                                                        applyOpsHangBeforePreparingTransaction);
    }

    transaction.prepareTransaction(opCtx, entry.getOpTime());
    transaction.stashTransactionResources(opCtx);

    return Status::OK();
}

/**
 * Apply a prepared transaction when we are reconstructing prepared transactions.
 */
void _reconstructPreparedTransaction(OperationContext* opCtx,
                                     const OplogEntry& prepareEntry,
                                     repl::OplogApplication::Mode mode) {
    repl::UnreplicatedWritesBlock uwb(opCtx);

    // Snapshot transaction can never conflict with the PBWM lock.
    opCtx->lockState()->setShouldConflictWithSecondaryBatchApplication(false);

    // When querying indexes, we return the record matching the key if it exists, or an
    // adjacent document. This means that it is possible for us to hit a prepare conflict if
    // we query for an incomplete key and an adjacent key is prepared.
    // We ignore prepare conflicts on recovering nodes because they may encounter prepare
    // conflicts that did not occur on the primary.
    opCtx->recoveryUnit()->setPrepareConflictBehavior(
        PrepareConflictBehavior::kIgnoreConflictsAllowWrites);

    // We might replay a prepared transaction behind oldest timestamp.
    opCtx->recoveryUnit()->setRoundUpPreparedTimestamps(true);

    // Checks out the session, applies the operations and prepares the transaction.
    uassertStatusOK(_applyPrepareTransaction(opCtx, prepareEntry, mode));
}
}  // namespace

/**
 * Make sure that if we are in replication recovery, we don't apply the prepare transaction oplog
 * entry until we either see a commit transaction oplog entry or are at the very end of recovery.
 * Otherwise, only apply the prepare transaction oplog entry if we are a secondary. We shouldn't get
 * here for initial sync and applyOps should error.
 */
Status applyPrepareTransaction(OperationContext* opCtx,
                               const OplogEntry& entry,
                               repl::OplogApplication::Mode mode) {
    switch (mode) {
        case repl::OplogApplication::Mode::kRecovering: {
            if (!serverGlobalParams.enableMajorityReadConcern) {
                error()
                    << "Cannot replay a prepared transaction when 'enableMajorityReadConcern' is "
                       "set to false. Restart the server with --enableMajorityReadConcern=true "
                       "to complete recovery.";
                fassertFailed(51146);
            }

            // Don't apply the operations from the prepared transaction until either we see a commit
            // transaction oplog entry during recovery or are at the end of recovery.
            return Status::OK();
        }
        case repl::OplogApplication::Mode::kInitialSync: {
            // Initial sync should never apply 'prepareTransaction' since it unpacks committed
            // transactions onto various applier threads at commit time.
            MONGO_UNREACHABLE;
        }
        case repl::OplogApplication::Mode::kApplyOpsCmd: {
            // Return error if run via applyOps command.
            uasserted(51145,
                      "prepare applyOps oplog entry is only used internally by secondaries.");
        }
        case repl::OplogApplication::Mode::kSecondary: {
            return _applyPrepareTransaction(opCtx, entry, repl::OplogApplication::Mode::kSecondary);
        }
    }
    MONGO_UNREACHABLE;
}

void reconstructPreparedTransactions(OperationContext* opCtx, repl::OplogApplication::Mode mode) {
    if (MONGO_FAIL_POINT(skipReconstructPreparedTransactions)) {
        log() << "Hit skipReconstructPreparedTransactions failpoint";
        return;
    }
    // Read the transactions table and the oplog collection without a timestamp.
    // The below DBDirectClient read uses AutoGetCollectionForRead which could implicitly change the
    // read source to kLastApplied. So we need to explicitly set the read source to kNoTimestamp to
    // force reads in this scope to be untimestamped.
    ReadSourceScope readSourceScope(opCtx, RecoveryUnit::ReadSource::kNoTimestamp);

    DBDirectClient client(opCtx);
    const auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace,
                                     {BSON("state"
                                           << "prepared")});

    // Iterate over each entry in the transactions table that has a prepared transaction.
    while (cursor->more()) {
        const auto txnRecordObj = cursor->next();
        const auto txnRecord = SessionTxnRecord::parse(
            IDLParserErrorContext("recovering prepared transaction"), txnRecordObj);

        invariant(txnRecord.getState() == DurableTxnStateEnum::kPrepared);

        // Get the prepareTransaction oplog entry corresponding to this transactions table entry.
        const auto prepareOpTime = txnRecord.getLastWriteOpTime();
        invariant(!prepareOpTime.isNull());
        TransactionHistoryIterator iter(prepareOpTime);
        invariant(iter.hasNext());
        auto prepareOplogEntry = iter.nextFatalOnErrors(opCtx);

        {
            // Make a new opCtx so that we can set the lsid when applying the prepare transaction
            // oplog entry.
            auto newClient =
                opCtx->getServiceContext()->makeClient("reconstruct-prepared-transactions");
            AlternativeClientRegion acr(newClient);
            const auto newOpCtx = cc().makeOperationContext();

            _reconstructPreparedTransaction(newOpCtx.get(), prepareOplogEntry, mode);
        }
    }
}
}  // namespace mongo
