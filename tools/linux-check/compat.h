// Только для проверочной сборки под Linux: интринсики MSVC через встроенные GCC.
#pragma once
#ifndef _MSC_VER
#include <cstdint>
#include <limits>
#include <thread>
static inline unsigned long long __popcnt64(unsigned long long v) { return (unsigned long long)__builtin_popcountll(v); }
static inline unsigned char _BitScanForward64(unsigned long* index, unsigned long long mask)
{
    if (!mask) return 0;
    *index = (unsigned long)__builtin_ctzll(mask);
    return 1;
}
#endif
typedef unsigned long long u64ll_;
