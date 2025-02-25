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

#include "mongo/platform/basic.h"

#include <cstdio>
#include <cstdlib>
#include <cxxabi.h>
#include <functional>
#include <sstream>

#include <fmt/format.h>
#include <fmt/printf.h>

#include <libunwind.h>

#include "mongo/base/backtrace_visibility_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/stacktrace.h"

namespace mongo {

// Must be a named namespace so the functions we want to unwind through have external linkage.
// Without that, the compiler optimizes them away.
namespace unwind_test_detail {

using namespace fmt::literals;

std::string trace() {
    std::string out;
    unw_cursor_t cursor;
    unw_context_t context;

    // Initialize cursor to current frame for local unwinding.
    unw_getcontext(&context);
    unw_init_local(&cursor, &context);
    // Unwind frames one by one, going up the frame stack.
    while (unw_step(&cursor) > 0) {
        unw_word_t offset, pc;
        unw_get_reg(&cursor, UNW_REG_IP, &pc);
        if (pc == 0) {
            break;
        }
        out += "0x{:x}:"_format(pc);
        char sym[32 << 10];
        char* name = sym;
        int err;
        if ((err = unw_get_proc_name(&cursor, sym, sizeof(sym), &offset)) != 0) {
            out += " -- error: unable to obtain symbol name for this frame: {:d}\n"_format(err);
            continue;
        }
        name = sym;
        int status;
        char* demangled_name;
        if ((demangled_name = abi::__cxa_demangle(sym, nullptr, nullptr, &status))) {
            name = demangled_name;
        }
        out += " ({:s}+0x{:x})\n"_format(name, offset);
        if (name != sym) {
            free(name);  // allocated by abi::__cxa_demangle
        }
    }
    return out;
}

struct Context {
    std::vector<std::function<void(Context&)>> plan;
    std::string s;
};

// Disable clang-format for the "if constexpr"
template <int N>
void callNext(Context& ctx) {
    if constexpr (N == 0) {
        ctx.s = trace();
    } else {
        ctx.plan[N - 1](ctx);
    }

    // Forces compiler to invoke next plan with `call` instead of `jmp`.
    asm volatile("");  // NOLINT
}

void assertAndRemovePrefix(std::string_view& v, const std::string_view prefix) {
    auto pos = v.find(prefix);
    ASSERT(pos != v.npos) << "expected to find '{}' in '{}'"_format(prefix, v);
    v.remove_prefix(pos);
    v.remove_prefix(prefix.length());
}

void assertAndRemoveSuffix(std::string_view& v, const std::string_view suffix) {
    auto pos = v.rfind(suffix);
    ASSERT(pos != v.npos) << "expected to find '{}' in '{}'"_format(suffix, v);
    v.remove_suffix(v.length() - pos);
}

template <size_t size>
void assertTraceContains(const std::string (&names)[size], const std::string stacktrace) {
    std::string_view view{stacktrace};
    assertAndRemovePrefix(view, "----- BEGIN BACKTRACE -----");
    assertAndRemovePrefix(view, "{\"backtrace\":");
    // Remove the rest of the JSON object, which is all one line.
    assertAndRemovePrefix(view, "\n");
    assertAndRemoveSuffix(view, "-----  END BACKTRACE  -----");
    std::string_view remainder{stacktrace};
    for (const auto& name : names) {
        auto pos = remainder.find(name);

        if (pos == remainder.npos) {
            unittest::log().setIsTruncatable(false)
                << std::endl
                << "--- BEGIN SAMPLE BACKTRACE ---" << std::endl
                << std::string(stacktrace) << "--- END SAMPLE BACKTRACE ---";
            FAIL("name '{}' is missing or out of order in sample backtrace"_format(
                std::string(name)));
        }
        remainder.remove_prefix(pos);
    }
}

TEST(Unwind, Demangled) {
    // Trickery with std::vector<std::function> is to hide from the optimizer.
    Context ctx{{
        callNext<0>,
        callNext<1>,
        callNext<2>,
        callNext<3>,
        callNext<4>,
        callNext<5>,
    }};
    ctx.plan.back()(ctx);
    // Check that these function names appear in the trace, in order.
    // There will of course be characters between them but ignore that.
    const std::string frames[] = {
        "void mongo::unwind_test_detail::callNext<0>(mongo::unwind_test_detail::Context&)",
        "void mongo::unwind_test_detail::callNext<1>(mongo::unwind_test_detail::Context&)",
        "void mongo::unwind_test_detail::callNext<2>(mongo::unwind_test_detail::Context&)",
        "void mongo::unwind_test_detail::callNext<3>(mongo::unwind_test_detail::Context&)",
        "void mongo::unwind_test_detail::callNext<4>(mongo::unwind_test_detail::Context&)",
        "void mongo::unwind_test_detail::callNext<5>(mongo::unwind_test_detail::Context&)",
        "main",
    };
    size_t pos = 0;
    for (const auto& expected : frames) {
        pos = ctx.s.find(expected, pos);
        ASSERT_NE(pos, ctx.s.npos) << ctx.s;
    }
}

TEST(Unwind, Linkage) {
    std::string stacktrace;

    // From backtrace_visibility_test.h. Calls a chain of functions and stores the backtrace at the
    // bottom in the "stacktrace" argument.
    normal_function(stacktrace);

    // Check that these function names appear in the trace, in order. The tracing code which
    // preceded our libunwind integration could *not* symbolize hidden/static_function.
    const std::string frames[] = {"printStackTrace",
                                  "static_function",
                                  "anonymous_namespace_function",
                                  "hidden_function",
                                  "normal_function"};
    assertTraceContains(frames, stacktrace);
}

}  // namespace unwind_test_detail
}  // namespace mongo
