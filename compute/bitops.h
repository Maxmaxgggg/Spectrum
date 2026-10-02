#pragma once

// Битовые приёмы, общие для хоста и ядер.
//
// Раньше каждый жил в двух-трёх копиях: gosperNext и разворот битов — в
// worker.cpp и в ядре коротких кодов, popcount и поиск младшей единицы — в
// worker.cpp, leonsearch и productcode, причём на хосте через интринсики
// MSVC без запасного пути для других компиляторов. Здесь одна запись на всё:
// в ядре функция берёт встроенную функцию CUDA, на хосте — интринсик MSVC
// или встроенную функцию GCC и Clang.
//
// Заголовок не тянет ни Qt, ни CUDA: его подключают и .cu, и обычные .cpp.

#include <cstdint>

#if defined(_MSC_VER) && !defined(__CUDA_ARCH__)
    #include <intrin.h>
#endif

// Функция для обеих сторон. В .cu её собирает nvcc и для хоста, и для
// устройства; в обычном .cpp это просто inline.
#ifdef __CUDACC__
    #define SPECTRUM_HD __host__ __device__ __forceinline__
#else
    #define SPECTRUM_HD inline
#endif

namespace Bits {

// Число единиц.
SPECTRUM_HD int popcount64(unsigned long long v)
{
#if defined(__CUDA_ARCH__)
    return __popcll(v);
#elif defined(_MSC_VER)
    return int(__popcnt64(v));
#else
    return __builtin_popcountll(v);
#endif
}

// Позиция младшей единицы. Вызывать только при v != 0.
SPECTRUM_HD int lowestSetBit(unsigned long long v)
{
#if defined(__CUDA_ARCH__)
    return __ffsll(static_cast<long long>(v)) - 1;
#elif defined(_MSC_VER)
    unsigned long index = 0;
    _BitScanForward64(&index, v);
    return int(index);
#else
    return __builtin_ctzll(v);
#endif
}

// Разворот всех 64 бит.
SPECTRUM_HD unsigned long long reverse64(unsigned long long v)
{
#if defined(__CUDA_ARCH__)
    return __brevll(v);
#else
    v = ((v >> 1)  & 0x5555555555555555ULL) | ((v & 0x5555555555555555ULL) << 1);
    v = ((v >> 2)  & 0x3333333333333333ULL) | ((v & 0x3333333333333333ULL) << 2);
    v = ((v >> 4)  & 0x0F0F0F0F0F0F0F0FULL) | ((v & 0x0F0F0F0F0F0F0F0FULL) << 4);
    v = ((v >> 8)  & 0x00FF00FF00FF00FFULL) | ((v & 0x00FF00FF00FF00FFULL) << 8);
    v = ((v >> 16) & 0x0000FFFF0000FFFFULL) | ((v & 0x0000FFFF0000FFFFULL) << 16);
    return (v >> 32) | (v << 32);
#endif
}

// Разворот младших k бит: бит p переходит в позицию k-1-p. 1 <= k <= 64.
SPECTRUM_HD unsigned long long reverseLowBits(unsigned long long v, int k)
{
    return reverse64(v) >> (64 - k);
}

// Следующая маска с тем же числом единиц в возрастающем числовом порядке —
// приём Госпера. Деление из классической записи заменено сдвигом: младший
// установленный бит есть степень двойки, его позиция и есть величина сдвига.
// Вызывать только при v != 0 и когда следующая комбинация существует.
SPECTRUM_HD unsigned long long gosperNext(unsigned long long v)
{
    const int t = lowestSetBit(v);
    const unsigned long long rr = v + (1ULL << t);
    return rr | ((v ^ rr) >> (t + 2));
}

// Старшие 64 бита произведения. Случайный индекс из [0, range) берётся как
// mulHigh64(random, range) — без деления: 64-битный остаток на видеокарте
// стоит сотни инструкций.
SPECTRUM_HD unsigned long long mulHigh64(unsigned long long a, unsigned long long b)
{
#if defined(__CUDA_ARCH__)
    return __umul64hi(a, b);
#elif defined(_MSC_VER)
    return __umulh(a, b);
#else
    return static_cast<unsigned long long>((static_cast<unsigned __int128>(a) * b) >> 64);
#endif
}

} // namespace Bits
