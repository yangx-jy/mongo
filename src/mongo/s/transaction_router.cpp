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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kTransaction

#include "mongo/platform/basic.h"

#include "mongo/s/transaction_router.h"

#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/router_transactions_metrics.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/net/socket_utils.h"

namespace mongo {
namespace {

// TODO SERVER-39704: Remove this fail point once the router can safely retry within a transaction
// on stale version and snapshot errors.
MONGO_FAIL_POINT_DEFINE(enableStaleVersionAndSnapshotRetriesWithinTransactions);

const char kCoordinatorField[] = "coordinator";
const char kReadConcernLevelSnapshotName[] = "snapshot";

const auto getTransactionRouter = Session::declareDecoration<TransactionRouter>();

bool isTransactionCommand(const BSONObj& cmd) {
    auto cmdName = cmd.firstElement().fieldNameStringData();
    return cmdName == "abortTransaction" || cmdName == "commitTransaction" ||
        cmdName == "prepareTransaction" || cmdName == CoordinateCommitTransaction::kCommandName;
}

/**
 * Attaches the given atClusterTime to the readConcern object in the given command object, removing
 * afterClusterTime if present. Assumes the given command object has a readConcern field and has
 * readConcern level snapshot.
 */
BSONObj appendAtClusterTimeToReadConcern(BSONObj cmdObj, LogicalTime atClusterTime) {
    dassert(cmdObj.hasField(repl::ReadConcernArgs::kReadConcernFieldName));

    BSONObjBuilder cmdAtClusterTimeBob;
    for (auto&& elem : cmdObj) {
        if (elem.fieldNameStringData() == repl::ReadConcernArgs::kReadConcernFieldName) {
            BSONObjBuilder readConcernBob =
                cmdAtClusterTimeBob.subobjStart(repl::ReadConcernArgs::kReadConcernFieldName);
            for (auto&& rcElem : elem.Obj()) {
                // afterClusterTime cannot be specified with atClusterTime.
                if (rcElem.fieldNameStringData() !=
                    repl::ReadConcernArgs::kAfterClusterTimeFieldName) {
                    readConcernBob.append(rcElem);
                }
            }

            dassert(readConcernBob.hasField(repl::ReadConcernArgs::kLevelFieldName) &&
                    readConcernBob.asTempObj()[repl::ReadConcernArgs::kLevelFieldName].String() ==
                        kReadConcernLevelSnapshotName);

            readConcernBob.append(repl::ReadConcernArgs::kAtClusterTimeFieldName,
                                  atClusterTime.asTimestamp());
        } else {
            cmdAtClusterTimeBob.append(elem);
        }
    }

    return cmdAtClusterTimeBob.obj();
}

BSONObj appendReadConcernForTxn(BSONObj cmd,
                                repl::ReadConcernArgs readConcernArgs,
                                boost::optional<LogicalTime> atClusterTime) {
    // Check for an existing read concern. The first statement in a transaction may already have
    // one, in which case its level should always match the level of the transaction's readConcern.
    if (cmd.hasField(repl::ReadConcernArgs::kReadConcernFieldName)) {
        repl::ReadConcernArgs existingReadConcernArgs;
        dassert(existingReadConcernArgs.initialize(cmd));
        dassert(existingReadConcernArgs.getLevel() == readConcernArgs.getLevel());

        return atClusterTime ? appendAtClusterTimeToReadConcern(std::move(cmd), *atClusterTime)
                             : cmd;
    }

    BSONObjBuilder bob(std::move(cmd));
    readConcernArgs.appendInfo(&bob);

    return atClusterTime ? appendAtClusterTimeToReadConcern(bob.asTempObj(), *atClusterTime)
                         : bob.obj();
}

BSONObjBuilder appendFieldsForStartTransaction(BSONObj cmd,
                                               repl::ReadConcernArgs readConcernArgs,
                                               boost::optional<LogicalTime> atClusterTime,
                                               bool doAppendStartTransaction) {
    auto cmdWithReadConcern = !readConcernArgs.isEmpty()
        ? appendReadConcernForTxn(std::move(cmd), readConcernArgs, atClusterTime)
        : std::move(cmd);

    BSONObjBuilder bob(std::move(cmdWithReadConcern));

    if (doAppendStartTransaction) {
        bob.append(OperationSessionInfoFromClient::kStartTransactionFieldName, true);
    }

    return bob;
}

// Commands that are idempotent in a transaction context and can be blindly retried in the middle of
// a transaction. Writing aggregates (e.g. with a $out or $merge) is disallowed in a transaction, so
// aggregates must be read operations. Note: aggregate and find do have the side-effect of creating
// cursors, but any established during an unsuccessful attempt are best-effort killed.
const StringMap<int> alwaysRetryableCmds = {
    {"aggregate", 1}, {"distinct", 1}, {"find", 1}, {"getMore", 1}, {"killCursors", 1}};

bool isReadConcernLevelAllowedInTransaction(repl::ReadConcernLevel readConcernLevel) {
    return readConcernLevel == repl::ReadConcernLevel::kSnapshotReadConcern ||
        readConcernLevel == repl::ReadConcernLevel::kMajorityReadConcern ||
        readConcernLevel == repl::ReadConcernLevel::kLocalReadConcern;
}

// Returns if the error code would be considered a retryable error for a retryable write.
bool isRetryableWritesError(ErrorCodes::Error code) {
    return std::find(RemoteCommandRetryScheduler::kAllRetriableErrors.begin(),
                     RemoteCommandRetryScheduler::kAllRetriableErrors.end(),
                     code) != RemoteCommandRetryScheduler::kAllRetriableErrors.end();
}

// Returns if a transaction's commit result is unknown based on the given statuses. A result is
// considered unknown if it would be given the "UnknownTransactionCommitResult" as defined by the
// driver transactions specification or fails with one of the errors for invalid write concern that
// are specifically not given the "UnknownTransactionCommitResult" label. Additionally,
// TransactionTooOld is considered unknown because a command that fails with it could not have done
// meaningful work.
//
// The "UnknownTransactionCommitResult" specification:
// https://github.com/mongodb/specifications/blob/master/source/transactions/transactions.rst#unknowntransactioncommitresult.
bool isCommitResultUnknown(const Status& commitStatus, const Status& commitWCStatus) {
    if (!commitStatus.isOK()) {
        return isRetryableWritesError(commitStatus.code()) ||
            ErrorCodes::isExceededTimeLimitError(commitStatus.code()) ||
            commitStatus.code() == ErrorCodes::TransactionTooOld;
    }

    if (!commitWCStatus.isOK()) {
        return true;
    }

    return false;
}

BSONObj sendCommitDirectlyToShards(OperationContext* opCtx, const std::vector<ShardId>& shardIds) {
    // Assemble requests.
    std::vector<AsyncRequestsSender::Request> requests;
    for (const auto& shardId : shardIds) {
        CommitTransaction commitCmd;
        commitCmd.setDbName(NamespaceString::kAdminDb);
        const auto commitCmdObj = commitCmd.toBSON(
            BSON(WriteConcernOptions::kWriteConcernField << opCtx->getWriteConcern().toBSON()));
        requests.emplace_back(shardId, commitCmdObj);
    }

    // Send the requests.
    MultiStatementTransactionRequestsSender ars(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(),
        NamespaceString::kAdminDb,
        requests,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        Shard::RetryPolicy::kIdempotent);

    BSONObj lastResult;

    // Receive the responses.
    while (!ars.done()) {
        auto response = ars.next();

        uassertStatusOK(response.swResponse);
        lastResult = response.swResponse.getValue().data;

        // If any shard returned an error, return the error immediately.
        const auto commandStatus = getStatusFromCommandResult(lastResult);
        if (!commandStatus.isOK()) {
            return lastResult;
        }

        // If any participant had a writeConcern error, return the participant's writeConcern
        // error immediately.
        const auto writeConcernStatus = getWriteConcernStatusFromCommandResult(lastResult);
        if (!writeConcernStatus.isOK()) {
            return lastResult;
        }
    }

    // If all the responses were ok, return the last response.
    return lastResult;
}

// Helper to convert the CommitType enum into a human readable string for diagnostics.
std::string commitTypeToString(TransactionRouter::CommitType state) {
    switch (state) {
        case TransactionRouter::CommitType::kNotInitiated:
            return "notInitiated";
        case TransactionRouter::CommitType::kNoShards:
            return "noShards";
        case TransactionRouter::CommitType::kSingleShard:
            return "singleShard";
        case TransactionRouter::CommitType::kSingleWriteShard:
            return "singleWriteShard";
        case TransactionRouter::CommitType::kReadOnly:
            return "readOnly";
        case TransactionRouter::CommitType::kTwoPhaseCommit:
            return "twoPhaseCommit";
        case TransactionRouter::CommitType::kRecoverWithToken:
            return "recoverWithToken";
    }
    MONGO_UNREACHABLE;
}

}  // unnamed namespace

TransactionRouter::TransactionRouter() = default;

TransactionRouter::~TransactionRouter() = default;

TransactionRouter::Observer::Observer(const ObservableSession& osession)
    : Observer(&getTransactionRouter(osession.get())) {}

TransactionRouter::Router::Router(OperationContext* opCtx)
    : Observer([opCtx]() -> TransactionRouter* {
          if (auto session = OperationContextSession::get(opCtx)) {
              return &getTransactionRouter(session);
          }
          return nullptr;
      }()) {}

TransactionRouter::Participant::Participant(bool inIsCoordinator,
                                            StmtId inStmtIdCreatedAt,
                                            ReadOnly inReadOnly,
                                            SharedTransactionOptions inSharedOptions)
    : isCoordinator(inIsCoordinator),
      readOnly(inReadOnly),
      sharedOptions(std::move(inSharedOptions)),
      stmtIdCreatedAt(inStmtIdCreatedAt) {}

BSONObj TransactionRouter::Observer::reportState(OperationContext* opCtx,
                                                 bool sessionIsActive) const {
    BSONObjBuilder builder;
    reportState(opCtx, &builder, sessionIsActive);
    return builder.obj();
}

void TransactionRouter::Observer::reportState(OperationContext* opCtx,
                                              BSONObjBuilder* builder,
                                              bool sessionIsActive) const {
    _reportState(opCtx, builder, sessionIsActive);
}

void TransactionRouter::Observer::_reportState(OperationContext* opCtx,
                                               BSONObjBuilder* builder,
                                               bool sessionIsActive) const {
    if (o().txnNumber == kUninitializedTxnNumber) {
        // This transaction router is not yet initialized.
        return;
    }

    // Append relevant client metadata.

    builder->append("type", sessionIsActive ? "activeSession" : "idleSession");
    builder->append("host", getHostNameCachedAndPort());
    builder->append("desc", sessionIsActive ? "active transaction" : "inactive transaction");

    const auto& lastClientInfo = o().lastClientInfo;
    builder->append("client", lastClientInfo.clientHostAndPort);
    builder->append("connectionId", lastClientInfo.connectionId);
    builder->append("appName", lastClientInfo.appName);
    builder->append("clientMetadata", lastClientInfo.clientMetadata);

    // Append session and transaction metadata.

    {
        BSONObjBuilder lsid(builder->subobjStart("lsid"));
        _sessionId().serialize(&lsid);
    }

    BSONObjBuilder transactionBuilder(builder->subobjStart("transaction"));

    {
        BSONObjBuilder parametersBuilder(transactionBuilder.subobjStart("parameters"));
        parametersBuilder.append("txnNumber", o().txnNumber);
        parametersBuilder.append("autocommit", false);
        if (!o().readConcernArgs.isEmpty()) {
            o().readConcernArgs.appendInfo(&parametersBuilder);
        }
    }

    // Append current transaction info.

    if (_atClusterTimeHasBeenSet()) {
        builder->append("globalReadTimestamp", o().atClusterTime->getTime().asTimestamp());
    }

    const auto& timingStats = o().timingStats;

    builder->append("startWallClockTime", dateToISOStringLocal(timingStats.startWallClockTime));

    auto tickSource = opCtx->getServiceContext()->getTickSource();
    auto curTicks = tickSource->getTicks();

    builder->append("timeOpenMicros",
                    durationCount<Microseconds>(timingStats.getDuration(tickSource, curTicks)));

    builder->append(
        "timeActiveMicros",
        durationCount<Microseconds>(timingStats.getTimeActiveMicros(tickSource, curTicks)));

    builder->append(
        "timeInactiveMicros",
        durationCount<Microseconds>(timingStats.getTimeInactiveMicros(tickSource, curTicks)));

    int numReadOnlyParticipants = 0;
    int numNonReadOnlyParticipants = 0;

    // We don't know the participants if we're recovering the commit.
    if (o().commitType != CommitType::kRecoverWithToken) {
        builder->append("numParticipants", static_cast<int>(o().participants.size()));

        BSONArrayBuilder participantsArrayBuilder;
        for (auto const& participantPair : o().participants) {
            BSONObjBuilder participantBuilder;
            participantBuilder.append("name", participantPair.first);
            participantBuilder.append("coordinator", participantPair.second.isCoordinator);

            if (participantPair.second.readOnly == Participant::ReadOnly::kReadOnly) {
                participantBuilder.append("readOnly", true);
                ++numReadOnlyParticipants;
            } else if (participantPair.second.readOnly == Participant::ReadOnly::kNotReadOnly) {
                participantBuilder.append("readOnly", false);
                ++numNonReadOnlyParticipants;
            }
            participantsArrayBuilder.append(participantBuilder.obj());
        }

        transactionBuilder.appendArray("participants", participantsArrayBuilder.obj());
    }

    if (o().commitType != CommitType::kNotInitiated) {
        transactionBuilder.append("commitStartWallClockTime",
                                  dateToISOStringLocal(timingStats.commitStartWallClockTime));
        transactionBuilder.append("commitType", commitTypeToString(o().commitType));
    }

    transactionBuilder.append("numReadOnlyParticipants", numReadOnlyParticipants);
    transactionBuilder.append("numNonReadOnlyParticipants", numNonReadOnlyParticipants);

    transactionBuilder.done();

    builder->append("active", sessionIsActive);
}

bool TransactionRouter::Observer::_atClusterTimeHasBeenSet() const {
    return o().atClusterTime.is_initialized() && o().atClusterTime->timeHasBeenSet();
}

const LogicalSessionId& TransactionRouter::Observer::_sessionId() const {
    const auto* owningSession = getTransactionRouter.owner(_tr);
    return owningSession->getSessionId();
}

BSONObj TransactionRouter::Participant::attachTxnFieldsIfNeeded(
    BSONObj cmd, bool isFirstStatementInThisParticipant) const {
    bool hasStartTxn = false;
    bool hasAutoCommit = false;
    bool hasTxnNum = false;

    BSONObjIterator iter(cmd);
    while (iter.more()) {
        auto elem = iter.next();

        if (OperationSessionInfoFromClient::kStartTransactionFieldName ==
            elem.fieldNameStringData()) {
            hasStartTxn = true;
        } else if (OperationSessionInfoFromClient::kAutocommitFieldName ==
                   elem.fieldNameStringData()) {
            hasAutoCommit = true;
        } else if (OperationSessionInfo::kTxnNumberFieldName == elem.fieldNameStringData()) {
            hasTxnNum = true;
        }
    }

    // TODO: SERVER-37045 assert when attaching startTransaction to killCursors command.

    // The first command sent to a participant must start a transaction, unless it is a transaction
    // command, which don't support the options that start transactions, i.e. startTransaction and
    // readConcern. Otherwise the command must not have a read concern.
    bool mustStartTransaction = isFirstStatementInThisParticipant && !isTransactionCommand(cmd);

    if (!mustStartTransaction) {
        dassert(!cmd.hasField(repl::ReadConcernArgs::kReadConcernFieldName));
    }

    BSONObjBuilder newCmd = mustStartTransaction
        ? appendFieldsForStartTransaction(std::move(cmd),
                                          sharedOptions.readConcernArgs,
                                          sharedOptions.atClusterTime,
                                          !hasStartTxn)
        : BSONObjBuilder(std::move(cmd));

    if (isCoordinator) {
        newCmd.append(kCoordinatorField, true);
    }

    if (!hasAutoCommit) {
        newCmd.append(OperationSessionInfoFromClient::kAutocommitFieldName, false);
    }

    if (!hasTxnNum) {
        newCmd.append(OperationSessionInfo::kTxnNumberFieldName, sharedOptions.txnNumber);
    } else {
        auto osi =
            OperationSessionInfoFromClient::parse("OperationSessionInfo"_sd, newCmd.asTempObj());
        invariant(sharedOptions.txnNumber == *osi.getTxnNumber());
    }

    return newCmd.obj();
}

void TransactionRouter::Router::processParticipantResponse(OperationContext* opCtx,
                                                           const ShardId& shardId,
                                                           const BSONObj& responseObj) {
    auto participant = getParticipant(shardId);
    invariant(participant, "Participant should exist if processing participant response");

    if (p().terminationInitiated) {
        // Do not process the transaction metadata after commit or abort have been initiated,
        // since a participant's state is partially reset on commit and abort.
        return;
    }

    auto commandStatus = getStatusFromCommandResult(responseObj);
    if (!commandStatus.isOK()) {
        return;
    }

    if (participant->stmtIdCreatedAt != p().latestStmtId) {
        uassert(
            51112,
            str::stream() << "readOnly field for participant " << shardId
                          << " should have been set on the participant's first successful response",
            participant->readOnly != Participant::ReadOnly::kUnset);
    }

    auto txnResponseMetadata =
        TxnResponseMetadata::parse("processParticipantResponse"_sd, responseObj);

    if (txnResponseMetadata.getReadOnly()) {
        if (participant->readOnly == Participant::ReadOnly::kUnset) {
            LOG(3) << txnIdToString() << " Marking " << shardId << " as read-only";
            _setReadOnlyForParticipant(opCtx, shardId, Participant::ReadOnly::kReadOnly);
            return;
        }

        uassert(51113,
                str::stream() << "Participant shard " << shardId
                              << " claimed to be read-only for a transaction after previously "
                                 "claiming to have done a write for the transaction",
                participant->readOnly == Participant::ReadOnly::kReadOnly);
        return;
    }

    // The shard reported readOnly:false on this statement.

    if (participant->readOnly != Participant::ReadOnly::kNotReadOnly) {
        LOG(3) << txnIdToString() << " Marking " << shardId << " as having done a write";

        _setReadOnlyForParticipant(opCtx, shardId, Participant::ReadOnly::kNotReadOnly);

        if (!p().recoveryShardId) {
            LOG(3) << txnIdToString() << " Choosing " << shardId << " as recovery shard";
            p().recoveryShardId = shardId;
        }
    }
}

LogicalTime TransactionRouter::AtClusterTime::getTime() const {
    invariant(_atClusterTime != LogicalTime::kUninitialized);
    invariant(_stmtIdSelectedAt);
    return _atClusterTime;
}

bool TransactionRouter::AtClusterTime::timeHasBeenSet() const {
    return _atClusterTime != LogicalTime::kUninitialized;
}

void TransactionRouter::AtClusterTime::setTime(LogicalTime atClusterTime, StmtId currentStmtId) {
    invariant(atClusterTime != LogicalTime::kUninitialized);
    _atClusterTime = atClusterTime;
    _stmtIdSelectedAt = currentStmtId;
}

bool TransactionRouter::AtClusterTime::canChange(StmtId currentStmtId) const {
    return !_stmtIdSelectedAt || *_stmtIdSelectedAt == currentStmtId;
}

bool TransactionRouter::Router::mustUseAtClusterTime() const {
    return o().atClusterTime.is_initialized();
}

LogicalTime TransactionRouter::Router::getSelectedAtClusterTime() const {
    invariant(o().atClusterTime);
    return o().atClusterTime->getTime();
}

const boost::optional<ShardId>& TransactionRouter::Router::getCoordinatorId() const {
    return o().coordinatorId;
}

const boost::optional<ShardId>& TransactionRouter::Router::getRecoveryShardId() const {
    return p().recoveryShardId;
}

BSONObj TransactionRouter::Router::attachTxnFieldsIfNeeded(OperationContext* opCtx,
                                                           const ShardId& shardId,
                                                           const BSONObj& cmdObj) {
    RouterTransactionsMetrics::get(opCtx)->incrementTotalRequestsTargeted();
    if (auto txnPart = getParticipant(shardId)) {
        LOG(4) << txnIdToString()
               << " Sending transaction fields to existing participant: " << shardId;
        return txnPart->attachTxnFieldsIfNeeded(cmdObj, false);
    }

    auto txnPart = _createParticipant(opCtx, shardId);
    LOG(4) << txnIdToString() << " Sending transaction fields to new participant: " << shardId;
    if (!p().isRecoveringCommit) {
        // Don't update participant stats during recovery since the participant list isn't known.
        RouterTransactionsMetrics::get(opCtx)->incrementTotalContactedParticipants();
    }

    return txnPart.attachTxnFieldsIfNeeded(cmdObj, true);
}

void TransactionRouter::Router::_verifyParticipantAtClusterTime(const Participant& participant) {
    const auto& participantAtClusterTime = participant.sharedOptions.atClusterTime;
    invariant(participantAtClusterTime);
    invariant(*participantAtClusterTime == o().atClusterTime->getTime());
}

const TransactionRouter::Participant* TransactionRouter::Router::getParticipant(
    const ShardId& shard) {
    const auto iter = o().participants.find(shard.toString());
    if (iter == o().participants.end())
        return nullptr;

    if (o().atClusterTime) {
        _verifyParticipantAtClusterTime(iter->second);
    }

    return &iter->second;
}

TransactionRouter::Participant& TransactionRouter::Router::_createParticipant(
    OperationContext* opCtx, const ShardId& shard) {

    // The first participant is chosen as the coordinator.
    auto isFirstParticipant = o().participants.empty();
    if (isFirstParticipant) {
        invariant(!o().coordinatorId);
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        o(lk).coordinatorId = shard.toString();
    }

    SharedTransactionOptions sharedOptions = {
        o().txnNumber,
        o().readConcernArgs,
        o().atClusterTime ? boost::optional<LogicalTime>(o().atClusterTime->getTime())
                          : boost::none};

    stdx::lock_guard<Client> lk(*opCtx->getClient());
    auto resultPair =
        o(lk).participants.try_emplace(shard.toString(),
                                       TransactionRouter::Participant(isFirstParticipant,
                                                                      p().latestStmtId,
                                                                      Participant::ReadOnly::kUnset,
                                                                      std::move(sharedOptions)));

    return resultPair.first->second;
}

void TransactionRouter::Router::_setReadOnlyForParticipant(OperationContext* opCtx,
                                                           const ShardId& shard,
                                                           const Participant::ReadOnly readOnly) {
    invariant(readOnly != Participant::ReadOnly::kUnset);

    const auto iter = o().participants.find(shard.toString());
    invariant(iter != o().participants.end());
    const auto currentParticipant = iter->second;

    auto newParticipant =
        TransactionRouter::Participant(currentParticipant.isCoordinator,
                                       currentParticipant.stmtIdCreatedAt,
                                       readOnly,
                                       std::move(currentParticipant.sharedOptions));

    stdx::lock_guard<Client> lk(*opCtx->getClient());
    o(lk).participants.erase(iter);
    o(lk).participants.try_emplace(shard.toString(), std::move(newParticipant));
}

void TransactionRouter::Router::_assertAbortStatusIsOkOrNoSuchTransaction(
    const AsyncRequestsSender::Response& response) const {
    auto shardResponse = uassertStatusOKWithContext(
        std::move(response.swResponse),
        str::stream() << "Failed to send abort to shard " << response.shardId
                      << " between retries of statement " << p().latestStmtId);

    auto status = getStatusFromCommandResult(shardResponse.data);
    uassert(ErrorCodes::NoSuchTransaction,
            str::stream() << txnIdToString() << "Transaction aborted between retries of statement "
                          << p().latestStmtId << " due to error: " << status
                          << " from shard: " << response.shardId,
            status.isOK() || status.code() == ErrorCodes::NoSuchTransaction);

    // abortTransaction is sent with no write concern, so there's no need to check for a write
    // concern error.
}

std::vector<ShardId> TransactionRouter::Router::_getPendingParticipants() const {
    std::vector<ShardId> pendingParticipants;
    for (const auto& participant : o().participants) {
        if (participant.second.stmtIdCreatedAt == p().latestStmtId) {
            pendingParticipants.emplace_back(ShardId(participant.first));
        }
    }
    return pendingParticipants;
}

void TransactionRouter::Router::_clearPendingParticipants(OperationContext* opCtx) {
    const auto pendingParticipants = _getPendingParticipants();

    // Send abort to each pending participant. This resets their transaction state and guarantees no
    // transactions will be left open if the retry does not re-target any of these shards.
    std::vector<AsyncRequestsSender::Request> abortRequests;
    for (const auto& participant : pendingParticipants) {
        abortRequests.emplace_back(participant, BSON("abortTransaction" << 1));
    }
    auto responses = gatherResponses(opCtx,
                                     NamespaceString::kAdminDb,
                                     ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                     Shard::RetryPolicy::kIdempotent,
                                     abortRequests);

    // Verify each abort succeeded or failed with NoSuchTransaction, which may happen if the
    // transaction was already implicitly aborted on the shard.
    for (const auto& response : responses) {
        _assertAbortStatusIsOkOrNoSuchTransaction(response);
    }

    // Remove each aborted participant from the participant list. Remove after sending abort, so
    // they are not added back to the participant list by the transaction tracking inside the ARS.
    for (const auto& participant : pendingParticipants) {
        // If the participant being removed was chosen as the recovery shard, reset the recovery
        // shard. This is safe because this participant is a pending participant, meaning it
        // cannot have been returned in the recoveryToken on an earlier statement.
        if (p().recoveryShardId && *p().recoveryShardId == participant) {
            p().recoveryShardId.reset();
        }

        stdx::lock_guard<Client> lk(*opCtx->getClient());
        invariant(o(lk).participants.erase(participant));
    }

    // If there are no more participants, also clear the coordinator id because a new one must be
    // chosen by the retry.
    if (o().participants.empty()) {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        o(lk).coordinatorId.reset();
        return;
    }

    // If participants were created by an earlier command, the coordinator must be one of them.
    invariant(o().coordinatorId);
    invariant(o().participants.count(*o().coordinatorId) == 1);
}

bool TransactionRouter::Router::canContinueOnStaleShardOrDbError(StringData cmdName) const {
    if (MONGO_FAIL_POINT(enableStaleVersionAndSnapshotRetriesWithinTransactions)) {
        // We can always retry on the first overall statement because all targeted participants must
        // be pending, so the retry will restart the local transaction on each one, overwriting any
        // effects from the first attempt.
        if (p().latestStmtId == p().firstStmtId) {
            return true;
        }

        // Only idempotent operations can be retried if the error came from a later statement
        // because non-pending participants targeted by the statement may receive the same statement
        // id more than once, and currently statement ids are not tracked by participants so the
        // operation would be applied each time.
        //
        // Note that the retry will fail if any non-pending participants returned a stale version
        // error during the latest statement, because the error will abort their local transactions
        // but the router's retry will expect them to be in-progress.
        if (alwaysRetryableCmds.count(cmdName)) {
            return true;
        }
    }

    return false;
}

void TransactionRouter::Router::onStaleShardOrDbError(OperationContext* opCtx,
                                                      StringData cmdName,
                                                      const Status& errorStatus) {
    invariant(canContinueOnStaleShardOrDbError(cmdName));

    LOG(3) << txnIdToString()
           << " Clearing pending participants after stale version error: " << errorStatus;

    // Remove participants created during the current statement so they are sent the correct options
    // if they are targeted again by the retry.
    _clearPendingParticipants(opCtx);
}

void TransactionRouter::Router::onViewResolutionError(OperationContext* opCtx,
                                                      const NamespaceString& nss) {
    // The router can always retry on a view resolution error.

    LOG(3) << txnIdToString()
           << " Clearing pending participants after view resolution error on namespace: " << nss;

    // Requests against views are always routed to the primary shard for its database, but the retry
    // on the resolved namespace does not have to re-target the primary, so pending participants
    // should be cleared.
    _clearPendingParticipants(opCtx);
}

bool TransactionRouter::Router::canContinueOnSnapshotError() const {
    if (MONGO_FAIL_POINT(enableStaleVersionAndSnapshotRetriesWithinTransactions)) {
        return o().atClusterTime && o().atClusterTime->canChange(p().latestStmtId);
    }

    return false;
}

void TransactionRouter::Router::onSnapshotError(OperationContext* opCtx,
                                                const Status& errorStatus) {
    invariant(canContinueOnSnapshotError());

    LOG(3) << txnIdToString()
           << " Clearing pending participants and resetting global snapshot "
              "timestamp after snapshot error: "
           << errorStatus << ", previous timestamp: " << o().atClusterTime->getTime();

    // The transaction must be restarted on all participants because a new read timestamp will be
    // selected, so clear all pending participants. Snapshot errors are only retryable on the first
    // client statement, so all participants should be cleared, including the coordinator.
    _clearPendingParticipants(opCtx);
    invariant(o().participants.empty());
    invariant(!o().coordinatorId);

    stdx::lock_guard<Client> lk(*opCtx->getClient());

    // Reset the global snapshot timestamp so the retry will select a new one.
    o(lk).atClusterTime.reset();
    o(lk).atClusterTime.emplace();
}

void TransactionRouter::Router::setDefaultAtClusterTime(OperationContext* opCtx) {
    if (!o().atClusterTime || !o().atClusterTime->canChange(p().latestStmtId)) {
        return;
    }

    auto defaultTime = LogicalClock::get(opCtx)->getClusterTime();
    _setAtClusterTime(
        opCtx, repl::ReadConcernArgs::get(opCtx).getArgsAfterClusterTime(), defaultTime);
}

void TransactionRouter::Router::_setAtClusterTime(
    OperationContext* opCtx,
    const boost::optional<LogicalTime>& afterClusterTime,
    LogicalTime candidateTime) {
    stdx::lock_guard<Client> lk(*opCtx->getClient());

    // If the user passed afterClusterTime, the chosen time must be greater than or equal to it.
    if (afterClusterTime && *afterClusterTime > candidateTime) {
        o(lk).atClusterTime->setTime(*afterClusterTime, p().latestStmtId);
        return;
    }

    LOG(2) << txnIdToString() << " Setting global snapshot timestamp to " << candidateTime
           << " on statement " << p().latestStmtId;

    o(lk).atClusterTime->setTime(candidateTime, p().latestStmtId);
}

void TransactionRouter::Router::beginOrContinueTxn(OperationContext* opCtx,
                                                   TxnNumber txnNumber,
                                                   TransactionActions action) {
    if (txnNumber < o().txnNumber) {
        // This transaction is older than the transaction currently in progress, so throw an error.
        uasserted(ErrorCodes::TransactionTooOld,
                  str::stream() << "txnNumber " << txnNumber << " is less than last txnNumber "
                                << o().txnNumber << " seen in session " << _sessionId());
    } else if (txnNumber == o().txnNumber) {
        // This is the same transaction as the one in progress.
        switch (action) {
            case TransactionActions::kStart: {
                uasserted(ErrorCodes::ConflictingOperationInProgress,
                          str::stream() << "txnNumber " << o().txnNumber << " for session "
                                        << _sessionId() << " already started");
            }
            case TransactionActions::kContinue: {
                uassert(ErrorCodes::InvalidOptions,
                        "Only the first command in a transaction may specify a readConcern",
                        repl::ReadConcernArgs::get(opCtx).isEmpty());

                repl::ReadConcernArgs::get(opCtx) = o().readConcernArgs;

                ++p().latestStmtId;
                _onContinue(opCtx);
                break;
            }
            case TransactionActions::kCommit:
                ++p().latestStmtId;
                _onContinue(opCtx);
                break;
        }
    } else if (txnNumber > o().txnNumber) {
        // This is a newer transaction.
        switch (action) {
            case TransactionActions::kStart: {
                auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
                uassert(
                    ErrorCodes::InvalidOptions,
                    "The first command in a transaction cannot specify a readConcern level other "
                    "than local, majority, or snapshot",
                    !readConcernArgs.hasLevel() ||
                        isReadConcernLevelAllowedInTransaction(readConcernArgs.getLevel()));

                _resetRouterState(opCtx, txnNumber);

                {
                    stdx::lock_guard<Client> lk(*opCtx->getClient());
                    o(lk).readConcernArgs = readConcernArgs;
                }

                if (o().readConcernArgs.getLevel() ==
                    repl::ReadConcernLevel::kSnapshotReadConcern) {
                    stdx::lock_guard<Client> lk(*opCtx->getClient());
                    o(lk).atClusterTime.emplace();
                }

                _onNewTransaction(opCtx);
                LOG(3) << txnIdToString() << " New transaction started";
                break;
            }
            case TransactionActions::kContinue: {
                uasserted(ErrorCodes::NoSuchTransaction,
                          str::stream()
                              << "cannot continue txnId " << o().txnNumber << " for session "
                              << _sessionId() << " with txnId " << txnNumber);
            }
            case TransactionActions::kCommit: {
                _resetRouterState(opCtx, txnNumber);
                // If the first action seen by the router for this transaction is to commit, that
                // means that the client is attempting to recover a commit decision.
                p().isRecoveringCommit = true;

                _onBeginRecoveringDecision(opCtx);
                LOG(3) << txnIdToString() << " Commit recovery started";
                break;
            }
        };
    }

    _updateLastClientInfo(opCtx->getClient());
}

void TransactionRouter::Router::stash(OperationContext* opCtx) {
    auto tickSource = opCtx->getServiceContext()->getTickSource();
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    o(lk).timingStats.trySetInactive(tickSource, tickSource->getTicks());
}

BSONObj TransactionRouter::Router::_handOffCommitToCoordinator(OperationContext* opCtx) {
    invariant(o().coordinatorId);
    auto coordinatorIter = o().participants.find(*o().coordinatorId);
    invariant(coordinatorIter != o().participants.end());

    std::vector<CommitParticipant> participantList;
    for (const auto& participantEntry : o().participants) {
        CommitParticipant commitParticipant;
        commitParticipant.setShardId(participantEntry.first);
        participantList.push_back(std::move(commitParticipant));
    }

    CoordinateCommitTransaction coordinateCommitCmd;
    coordinateCommitCmd.setDbName("admin");
    coordinateCommitCmd.setParticipants(participantList);
    const auto coordinateCommitCmdObj = coordinateCommitCmd.toBSON(
        BSON(WriteConcernOptions::kWriteConcernField << opCtx->getWriteConcern().toBSON()));

    LOG(3) << txnIdToString()
           << " Committing using two-phase commit, coordinator: " << *o().coordinatorId;

    MultiStatementTransactionRequestsSender ars(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(),
        NamespaceString::kAdminDb,
        {{*o().coordinatorId, coordinateCommitCmdObj}},
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        Shard::RetryPolicy::kIdempotent);

    auto response = ars.next();
    invariant(ars.done());
    uassertStatusOK(response.swResponse);

    return response.swResponse.getValue().data;
}

BSONObj TransactionRouter::Router::commitTransaction(
    OperationContext* opCtx, const boost::optional<TxnRecoveryToken>& recoveryToken) {
    p().terminationInitiated = true;

    auto commitRes = _commitTransaction(opCtx, recoveryToken);

    auto commitStatus = getStatusFromCommandResult(commitRes);
    auto commitWCStatus = getWriteConcernStatusFromCommandResult(commitRes);

    if (isCommitResultUnknown(commitStatus, commitWCStatus)) {
        // Don't update stats if we don't know the result of the transaction. The client may choose
        // to retry commit, which will update stats if the result is determined.
        //
        // Note that we also don't end the transaction if _commitTransaction() throws, which it
        // should only do on failure to send a request, in which case the commit result is unknown.
        return commitRes;
    }

    if (commitStatus.isOK()) {
        _onSuccessfulCommit(opCtx);
    } else {
        // Note that write concern errors are never considered a fatal commit error because they
        // should be retryable, so it is fine to only pass the top-level status.
        _onNonRetryableCommitError(opCtx, commitStatus);
    }

    return commitRes;
}

BSONObj TransactionRouter::Router::_commitTransaction(
    OperationContext* opCtx, const boost::optional<TxnRecoveryToken>& recoveryToken) {

    if (p().isRecoveringCommit) {
        uassert(50940,
                "Cannot recover the transaction decision without a recoveryToken",
                recoveryToken);
        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            o(lk).commitType = CommitType::kRecoverWithToken;
            _onStartCommit(lk, opCtx);
        }

        return _commitWithRecoveryToken(opCtx, *recoveryToken);
    }

    if (o().participants.empty()) {
        // The participants list can be empty if a transaction was began on mongos, but it never
        // ended up targeting any hosts. Such cases are legal for example if a find is issued
        // against a non-existent database.
        uassert(ErrorCodes::IllegalOperation,
                "Cannot commit without participants",
                o().txnNumber != kUninitializedTxnNumber);
        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            o(lk).commitType = CommitType::kNoShards;
            _onStartCommit(lk, opCtx);
        }

        return BSON("ok" << 1);
    }

    std::vector<ShardId> readOnlyShards;
    std::vector<ShardId> writeShards;
    for (const auto& participant : o().participants) {
        switch (participant.second.readOnly) {
            case Participant::ReadOnly::kUnset:
                uasserted(ErrorCodes::NoSuchTransaction,
                          str::stream()
                              << txnIdToString() << " Failed to commit transaction "
                              << "because a previous statement on the transaction "
                              << "participant " << participant.first << " was unsuccessful.");
            case Participant::ReadOnly::kReadOnly:
                readOnlyShards.push_back(participant.first);
                break;
            case Participant::ReadOnly::kNotReadOnly:
                writeShards.push_back(participant.first);
                break;
        }
    }

    if (o().participants.size() == 1) {
        ShardId shardId = o().participants.cbegin()->first;
        LOG(3) << txnIdToString()
               << " Committing single-shard transaction, single participant: " << shardId;

        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            o(lk).commitType = CommitType::kSingleShard;
            _onStartCommit(lk, opCtx);
        }


        return sendCommitDirectlyToShards(opCtx, {shardId});
    }

    if (writeShards.size() == 0) {
        LOG(3) << txnIdToString() << " Committing read-only transaction on "
               << readOnlyShards.size() << " shards";
        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            o(lk).commitType = CommitType::kReadOnly;
            _onStartCommit(lk, opCtx);
        }

        return sendCommitDirectlyToShards(opCtx, readOnlyShards);
    }

    if (writeShards.size() == 1) {
        LOG(3) << txnIdToString() << " Committing single-write-shard transaction with "
               << readOnlyShards.size()
               << " read-only shards, write shard: " << writeShards.front();
        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            o(lk).commitType = CommitType::kSingleWriteShard;
            _onStartCommit(lk, opCtx);
        }

        const auto readOnlyShardsResponse = sendCommitDirectlyToShards(opCtx, readOnlyShards);

        if (!getStatusFromCommandResult(readOnlyShardsResponse).isOK() ||
            !getWriteConcernStatusFromCommandResult(readOnlyShardsResponse).isOK()) {
            return readOnlyShardsResponse;
        }
        return sendCommitDirectlyToShards(opCtx, writeShards);
    }

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        o(lk).commitType = CommitType::kTwoPhaseCommit;
        _onStartCommit(lk, opCtx);
    }

    return _handOffCommitToCoordinator(opCtx);
}

BSONObj TransactionRouter::Router::abortTransaction(OperationContext* opCtx) {
    // Update stats on scope exit so the transaction is considered "active" while waiting on abort
    // responses.
    auto updateStatsGuard = makeGuard([&] { _onExplicitAbort(opCtx); });

    // The router has yet to send any commands to a remote shard for this transaction.
    // Return the same error that would have been returned by a shard.
    uassert(ErrorCodes::NoSuchTransaction,
            "no known command has been sent by this router for this transaction",
            !o().participants.empty());

    p().terminationInitiated = true;

    auto abortCmd = BSON("abortTransaction" << 1 << WriteConcernOptions::kWriteConcernField
                                            << opCtx->getWriteConcern().toBSON());
    std::vector<AsyncRequestsSender::Request> abortRequests;
    for (const auto& participantEntry : o().participants) {
        abortRequests.emplace_back(ShardId(participantEntry.first), abortCmd);
    }

    LOG(3) << txnIdToString() << " Aborting transaction on " << o().participants.size()
           << " shard(s)";

    const auto responses = gatherResponses(opCtx,
                                           NamespaceString::kAdminDb,
                                           ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                           Shard::RetryPolicy::kIdempotent,
                                           abortRequests);

    BSONObj lastResult;
    for (const auto& response : responses) {
        uassertStatusOK(response.swResponse);

        lastResult = response.swResponse.getValue().data;

        // If any shard returned an error, return the error immediately.
        const auto commandStatus = getStatusFromCommandResult(lastResult);
        if (!commandStatus.isOK()) {
            return lastResult;
        }

        // If any participant had a writeConcern error, return the participant's writeConcern
        // error immediately.
        const auto writeConcernStatus = getWriteConcernStatusFromCommandResult(lastResult);
        if (!writeConcernStatus.isOK()) {
            return lastResult;
        }
    }

    // If all the responses were ok, return the last response.
    return lastResult;
}

void TransactionRouter::Router::implicitlyAbortTransaction(OperationContext* opCtx,
                                                           const Status& errorStatus) {
    if (o().commitType == CommitType::kTwoPhaseCommit ||
        o().commitType == CommitType::kRecoverWithToken) {
        LOG(3) << txnIdToString()
               << " Router not sending implicit abortTransaction because commit "
                  "may have been handed off to the coordinator";
        return;
    }

    // Update stats on scope exit so the transaction is considered "active" while waiting on abort
    // responses.
    auto updateStatsGuard = makeGuard([&] { _onImplicitAbort(opCtx, errorStatus); });

    if (o().participants.empty()) {
        return;
    }

    p().terminationInitiated = true;

    auto abortCmd = BSON("abortTransaction" << 1);
    std::vector<AsyncRequestsSender::Request> abortRequests;
    for (const auto& participantEntry : o().participants) {
        abortRequests.emplace_back(ShardId(participantEntry.first), abortCmd);
    }

    LOG(3) << txnIdToString() << " Implicitly aborting transaction on " << o().participants.size()
           << " shard(s) due to error: " << errorStatus;

    try {
        // Ignore the responses.
        gatherResponses(opCtx,
                        NamespaceString::kAdminDb,
                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                        Shard::RetryPolicy::kIdempotent,
                        abortRequests);
    } catch (const DBException& ex) {
        LOG(3) << txnIdToString() << " Implicitly aborting transaction failed "
               << causedBy(ex.toStatus());
        // Ignore any exceptions.
    }
}

std::string TransactionRouter::Router::txnIdToString() const {
    return str::stream() << _sessionId().getId() << ":" << o().txnNumber;
}

void TransactionRouter::Router::appendRecoveryToken(BSONObjBuilder* builder) const {
    BSONObjBuilder recoveryTokenBuilder(
        builder->subobjStart(CommitTransaction::kRecoveryTokenFieldName));
    TxnRecoveryToken recoveryToken;

    // The recovery shard is chosen on the first statement that did a write (transactions that only
    // did reads do not need to be recovered; they can just be retried).
    if (p().recoveryShardId) {
        invariant(o().participants.find(*p().recoveryShardId)->second.readOnly ==
                  Participant::ReadOnly::kNotReadOnly);
        recoveryToken.setRecoveryShardId(*p().recoveryShardId);
    }

    recoveryToken.serialize(&recoveryTokenBuilder);
    recoveryTokenBuilder.doneFast();
}

void TransactionRouter::Router::_resetRouterState(OperationContext* opCtx,
                                                  const TxnNumber& txnNumber) {
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    o(lk).txnNumber = txnNumber;
    o(lk).commitType = CommitType::kNotInitiated;
    p().isRecoveringCommit = false;
    o(lk).participants.clear();
    o(lk).coordinatorId.reset();
    p().recoveryShardId.reset();
    o(lk).readConcernArgs = {};
    o(lk).atClusterTime.reset();
    o(lk).abortCause = std::string();
    o(lk).timingStats = TimingStats();
    p().terminationInitiated = false;

    auto tickSource = opCtx->getServiceContext()->getTickSource();
    auto curTicks = tickSource->getTicks();
    o(lk).timingStats.trySetActive(opCtx, curTicks);

    // TODO SERVER-37115: Parse statement ids from the client and remember the statement id
    // of the command that started the transaction, if one was included.
    p().latestStmtId = kDefaultFirstStmtId;
    p().firstStmtId = kDefaultFirstStmtId;
};

BSONObj TransactionRouter::Router::_commitWithRecoveryToken(OperationContext* opCtx,
                                                            const TxnRecoveryToken& recoveryToken) {
    uassert(ErrorCodes::NoSuchTransaction,
            "Recovery token is empty, meaning the transaction only performed reads and can be "
            "safely retried",
            recoveryToken.getRecoveryShardId());
    const auto& recoveryShardId = *recoveryToken.getRecoveryShardId();

    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();

    auto coordinateCommitCmd = [&] {
        CoordinateCommitTransaction coordinateCommitCmd;
        coordinateCommitCmd.setDbName("admin");
        coordinateCommitCmd.setParticipants({});

        auto rawCoordinateCommit = coordinateCommitCmd.toBSON(
            BSON(WriteConcernOptions::kWriteConcernField << opCtx->getWriteConcern().toBSON()));

        return attachTxnFieldsIfNeeded(opCtx, recoveryShardId, rawCoordinateCommit);
    }();

    auto recoveryShard = uassertStatusOK(shardRegistry->getShard(opCtx, recoveryShardId));
    return uassertStatusOK(recoveryShard->runCommandWithFixedRetryAttempts(
                               opCtx,
                               ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                               "admin",
                               coordinateCommitCmd,
                               Shard::RetryPolicy::kIdempotent))
        .response;
}

void TransactionRouter::Router::_logSlowTransaction(OperationContext* opCtx,
                                                    TerminationCause terminationCause) const {
    log() << "transaction " << _transactionInfoForLog(opCtx, terminationCause);
}

std::string TransactionRouter::Router::_transactionInfoForLog(
    OperationContext* opCtx, TerminationCause terminationCause) const {
    StringBuilder sb;

    BSONObjBuilder parametersBuilder;

    BSONObjBuilder lsidBuilder(parametersBuilder.subobjStart("lsid"));
    _sessionId().serialize(&lsidBuilder);
    lsidBuilder.doneFast();

    parametersBuilder.append("txnNumber", o().txnNumber);
    parametersBuilder.append("autocommit", false);

    if (!o().readConcernArgs.isEmpty()) {
        o().readConcernArgs.appendInfo(&parametersBuilder);
    }

    sb << "parameters:" << parametersBuilder.obj().toString() << ",";

    if (_atClusterTimeHasBeenSet()) {
        sb << " globalReadTimestamp:" << o().atClusterTime->getTime().toString() << ",";
    }

    if (o().commitType != CommitType::kRecoverWithToken) {
        // We don't know the participants if we're recovering the commit.
        sb << " numParticipants:" << o().participants.size() << ",";
    }

    if (o().commitType == CommitType::kTwoPhaseCommit) {
        invariant(o().coordinatorId);
        sb << " coordinator:" << *o().coordinatorId << ",";
    }

    auto tickSource = opCtx->getServiceContext()->getTickSource();
    auto curTicks = tickSource->getTicks();

    if (terminationCause == TerminationCause::kCommitted) {
        sb << " terminationCause:committed,";

        invariant(o().commitType != CommitType::kNotInitiated);
        invariant(o().abortCause.empty());
    } else {
        sb << " terminationCause:aborted,";

        invariant(!o().abortCause.empty());
        sb << " abortCause:" << o().abortCause << ",";
    }

    if (o().commitType != CommitType::kNotInitiated) {
        sb << " commitType:" << commitTypeToString(o().commitType) << ",";

        sb << " commitDurationMicros:"
           << durationCount<Microseconds>(o().timingStats.getCommitDuration(tickSource, curTicks))
           << ",";
    }

    sb << " timeActiveMicros:"
       << durationCount<Microseconds>(o().timingStats.getTimeActiveMicros(tickSource, curTicks))
       << ",";

    sb << " timeInactiveMicros:"
       << durationCount<Microseconds>(o().timingStats.getTimeInactiveMicros(tickSource, curTicks))
       << ",";

    // Total duration of the transaction. Logged at the end of the line for consistency with slow
    // command logging.
    sb << " " << duration_cast<Milliseconds>(o().timingStats.getDuration(tickSource, curTicks));

    return sb.str();
}

void TransactionRouter::Router::_onNewTransaction(OperationContext* opCtx) {
    auto routerTxnMetrics = RouterTransactionsMetrics::get(opCtx);
    routerTxnMetrics->incrementTotalStarted();
}

void TransactionRouter::Router::_onBeginRecoveringDecision(OperationContext* opCtx) {
    auto routerTxnMetrics = RouterTransactionsMetrics::get(opCtx);
    routerTxnMetrics->incrementTotalStarted();
}

void TransactionRouter::Router::_onImplicitAbort(OperationContext* opCtx,
                                                 const Status& errorStatus) {
    if (o().commitType != CommitType::kNotInitiated && o().timingStats.endTime == 0) {
        // If commit was started but an end time wasn't set, then we don't know the commit result
        // and can't consider the transaction over until the client retries commit and definitively
        // learns the result. Note that this behavior may lead to no logging in some cases, but
        // should avoid logging an incorrect decision.
        return;
    }

    // Implicit abort may execute multiple times if a misbehaving client keeps sending statements
    // for a txnNumber after receiving an error, so only remember the first abort cause.
    if (o().abortCause.empty()) {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        o(lk).abortCause = errorStatus.codeString();
    }

    _endTransactionTrackingIfNecessary(opCtx, TerminationCause::kAborted);
}

void TransactionRouter::Router::_onExplicitAbort(OperationContext* opCtx) {
    // A behaving client should never try to commit after attempting to abort, so we can consider
    // the transaction terminated as soon as explicit abort is observed.
    if (o().abortCause.empty()) {
        // Note this code means the abort was from a user abortTransaction command.
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        o(lk).abortCause = "abort";
    }

    _endTransactionTrackingIfNecessary(opCtx, TerminationCause::kAborted);
}

void TransactionRouter::Router::_onStartCommit(WithLock wl, OperationContext* opCtx) {
    invariant(o().commitType != CommitType::kNotInitiated);

    if (o().timingStats.commitStartTime != 0) {
        return;
    }

    auto tickSource = opCtx->getServiceContext()->getTickSource();
    {
        o(wl).timingStats.commitStartTime = tickSource->getTicks();
        o(wl).timingStats.commitStartWallClockTime =
            opCtx->getServiceContext()->getPreciseClockSource()->now();
    }

    auto routerTxnMetrics = RouterTransactionsMetrics::get(opCtx);
    routerTxnMetrics->incrementCommitInitiated(o().commitType);
    if (o().commitType != CommitType::kRecoverWithToken) {
        // We only know the participant list if we're not recovering a decision.
        routerTxnMetrics->addToTotalParticipantsAtCommit(o().participants.size());
    }
}

void TransactionRouter::Router::_onNonRetryableCommitError(OperationContext* opCtx,
                                                           Status commitStatus) {
    // If the commit failed with a command error that can't be retried on, the transaction shouldn't
    // be able to eventually commit, so it can be considered over from the router's perspective.
    if (o().abortCause.empty()) {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        o(lk).abortCause = commitStatus.codeString();
    }
    _endTransactionTrackingIfNecessary(opCtx, TerminationCause::kAborted);
}

void TransactionRouter::Router::_onContinue(OperationContext* opCtx) {
    auto tickSource = opCtx->getServiceContext()->getTickSource();

    stdx::lock_guard<Client> lk(*opCtx->getClient());
    o(lk).timingStats.trySetActive(opCtx, tickSource->getTicks());
}

void TransactionRouter::Router::_onSuccessfulCommit(OperationContext* opCtx) {
    _endTransactionTrackingIfNecessary(opCtx, TerminationCause::kCommitted);
}

void TransactionRouter::Router::_endTransactionTrackingIfNecessary(
    OperationContext* opCtx, TerminationCause terminationCause) {
    if (o().timingStats.endTime != 0) {
        // If the transaction was already ended, don't end it again.
        return;
    }

    auto tickSource = opCtx->getServiceContext()->getTickSource();
    auto curTicks = tickSource->getTicks();

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());

        // In some error contexts, the transaction may not have been started yet, so try setting the
        // transaction's timing stats to active before ending it below. This is a no-op for already
        // active transactions.
        o(lk).timingStats.trySetActive(opCtx, curTicks);

        o(lk).timingStats.trySetInactive(tickSource, curTicks);
        o(lk).timingStats.endTime = curTicks;
    }

    if (shouldLog(logger::LogComponent::kTransaction, logger::LogSeverity::Debug(1)) ||
        o().timingStats.getDuration(tickSource, curTicks) >
            Milliseconds(serverGlobalParams.slowMS)) {
        _logSlowTransaction(opCtx, terminationCause);
    }

    auto routerTxnMetrics = RouterTransactionsMetrics::get(opCtx);
    if (terminationCause == TerminationCause::kAborted) {
        routerTxnMetrics->incrementTotalAborted();
        routerTxnMetrics->incrementAbortCauseMap(o().abortCause);
    } else {
        routerTxnMetrics->incrementTotalCommitted();
        routerTxnMetrics->incrementCommitSuccessful(
            o().commitType, o().timingStats.getCommitDuration(tickSource, curTicks));
    }
}

void TransactionRouter::Router::_updateLastClientInfo(Client* client) {
    stdx::lock_guard<Client> lk(*client);
    o(lk).lastClientInfo.update(client);
}

Microseconds TransactionRouter::TimingStats::getDuration(TickSource* tickSource,
                                                         TickSource::Tick curTicks) const {
    invariant(startTime > 0);

    // If the transaction hasn't ended, return how long it has been running for.
    if (endTime == 0) {
        return tickSource->ticksTo<Microseconds>(curTicks - startTime);
    }
    return tickSource->ticksTo<Microseconds>(endTime - startTime);
}

Microseconds TransactionRouter::TimingStats::getCommitDuration(TickSource* tickSource,
                                                               TickSource::Tick curTicks) const {
    invariant(commitStartTime > 0);

    // If the transaction hasn't ended, return how long commit has been running for.
    if (endTime == 0) {
        return tickSource->ticksTo<Microseconds>(curTicks - commitStartTime);
    }
    return tickSource->ticksTo<Microseconds>(endTime - commitStartTime);
}

Microseconds TransactionRouter::TimingStats::getTimeActiveMicros(TickSource* tickSource,
                                                                 TickSource::Tick curTicks) const {
    invariant(startTime > 0);

    if (lastTimeActiveStart != 0) {
        // The transaction is currently active, so return the active time so far plus the time since
        // the transaction became active.
        return timeActiveMicros + tickSource->ticksTo<Microseconds>(curTicks - lastTimeActiveStart);
    }
    return timeActiveMicros;
}

Microseconds TransactionRouter::TimingStats::getTimeInactiveMicros(
    TickSource* tickSource, TickSource::Tick curTicks) const {
    invariant(startTime > 0);

    auto micros = getDuration(tickSource, curTicks) - getTimeActiveMicros(tickSource, curTicks);
    dassert(micros >= Microseconds(0),
            str::stream() << "timeInactiveMicros should never be negative, was: " << micros);
    return micros;
}

void TransactionRouter::TimingStats::trySetActive(OperationContext* opCtx,
                                                  TickSource::Tick curTicks) {
    if (endTime != 0 || lastTimeActiveStart != 0) {
        // A transaction can't become active if it has already ended or is already active.
        return;
    }

    if (startTime == 0) {
        // If the transaction is becoming active for the first time, also set the transaction's
        // start time.
        startTime = curTicks;
        startWallClockTime = opCtx->getServiceContext()->getPreciseClockSource()->now();
    }
    lastTimeActiveStart = curTicks;
}

void TransactionRouter::TimingStats::trySetInactive(TickSource* tickSource,
                                                    TickSource::Tick curTicks) {
    if (endTime != 0 || lastTimeActiveStart == 0) {
        // If the transaction is already over or the router has already been stashed, the relevant
        // stats should have been updated earlier. In certain error scenarios, it's possible for a
        // transaction to be stashed twice in a row.
        return;
    }

    timeActiveMicros += tickSource->ticksTo<Microseconds>(curTicks - lastTimeActiveStart);
    lastTimeActiveStart = 0;
}

}  // namespace mongo
