// SPDX-License-Identifier: Apache-2.0
// macOS trace points. Apple's SDK ships its own DTRACE_PROBE macros in
// <sys/sdt.h>, so use XRT-private no-op names here instead of redefining them.
#ifndef core_common_detail_darwin_trace_h
#define core_common_detail_darwin_trace_h

#include "core/common/trace.h"

#define XRT_DETAIL_TRACE_POINT_LOG(probe, ...) do {} while (0)

#define XRT_DETAIL_TRACE_POINT_SCOPE(probe)                             \
  struct xrt_trace_scope { /* NOLINT */                                 \
    xrt_trace_scope() {}                                                \
    ~xrt_trace_scope() {}                                               \
  } xrt_trace_scope_instance

#define XRT_DETAIL_TRACE_POINT_SCOPE1(probe, arg1)                      \
  struct xrt_trace_scope1 {                                             \
    decltype(arg1) a1;                                                  \
    explicit xrt_trace_scope1(decltype(a1) aa1) : a1{aa1} {}            \
    ~xrt_trace_scope1() {}                                              \
  } xrt_trace_scope_instance{arg1}

#define XRT_DETAIL_TRACE_POINT_SCOPE2(probe, arg1, arg2)                \
  struct xrt_trace_scope2 {                                             \
    decltype(arg1) a1;                                                  \
    decltype(arg2) a2;                                                  \
    xrt_trace_scope2(decltype(a1) aa1, decltype(a2) aa2) : a1{aa1}, a2{aa2} {} \
    ~xrt_trace_scope2() {}                                              \
  } xrt_trace_scope_instance{arg1, arg2}

#endif
