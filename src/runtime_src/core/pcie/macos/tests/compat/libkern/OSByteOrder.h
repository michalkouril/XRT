// Enough of macOS's <libkern/OSByteOrder.h> to build the macOS shim's pure
// parsing code on a Linux host, so it can be unit tested and fuzzed without a
// Mac and without a card.
#pragma once
#include <cstdint>
#if defined(__linux__)
#include <byteswap.h>
static inline uint16_t OSSwapBigToHostInt16(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t OSSwapBigToHostInt32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint64_t OSSwapBigToHostInt64(uint64_t v) { return __builtin_bswap64(v); }
static inline uint16_t OSSwapHostToBigInt16(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t OSSwapHostToBigInt32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint64_t OSSwapHostToBigInt64(uint64_t v) { return __builtin_bswap64(v); }
#endif
