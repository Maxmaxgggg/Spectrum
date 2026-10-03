// Только для проверочной сборки clang: на Linux uint64_t — unsigned long, а не
// unsigned long long, как на Windows; и в clang 18 нет __ldcg.
#pragma once
#include <cuda_runtime.h>
__device__ inline unsigned long atomicAdd(unsigned long* a, unsigned long v)
{ return (unsigned long)atomicAdd((unsigned long long*)a, (unsigned long long)v); }
__device__ inline unsigned long atomicCAS(unsigned long* a, unsigned long c, unsigned long v)
{ return (unsigned long)atomicCAS((unsigned long long*)a, (unsigned long long)c, (unsigned long long)v); }
template <class T> __device__ inline T __ldcg(const T* p) { return *(const volatile T*)p; }
typedef unsigned long long u64ll_;
