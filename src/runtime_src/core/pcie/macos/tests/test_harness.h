// A deliberately small assertion harness for host-only tests.
//
// Two properties matter more than features. Each test binary exits non-zero if
// anything failed, and prints exactly one summary line a script can parse:
//
//   TEST <name>: <passed> passed, <failed> failed
//
// The existing card tools print numbers for a human to read, which is how a
// 180x performance regression and three tools broken on a second card all went
// unnoticed. A verdict and an exit code are the point; no framework needed.
#pragma once
#include <cstdio>
#include <cstring>
#include <string>

namespace t {

inline int passed = 0;
inline int failed = 0;
inline const char* suite = "unnamed";

inline void
ok(bool cond, const char* what)
{
  if (cond) {
    ++passed;
  }
  else {
    ++failed;
    std::printf("  FAIL %s\n", what);
  }
}

template <typename A, typename B>
inline void
eq(const A& got, const B& want, const char* what)
{
  if (got == want) {
    ++passed;
    return;
  }
  ++failed;
  std::printf("  FAIL %s\n", what);
}

// Specialisation worth having: string mismatches are the ones you want printed.
inline void
eq_str(const std::string& got, const std::string& want, const char* what)
{
  if (got == want) {
    ++passed;
    return;
  }
  ++failed;
  std::printf("  FAIL %s: got \"%s\", want \"%s\"\n", what, got.c_str(), want.c_str());
}

inline int
report()
{
  std::printf("TEST %s: %d passed, %d failed\n", suite, passed, failed);
  return failed ? 1 : 0;
}

} // namespace t
