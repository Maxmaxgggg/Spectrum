#pragma once

#include <cuda_runtime.h>
#include "defines.h"
#include <vector>
typedef uint64_t quint64;
#pragma once

// Макрос для проверки ошибок
#ifndef CUDA_CALL
#define CUDA_CALL(call) do { \
    cudaError_t err = (call); \
    if (err != cudaSuccess) { \
        fprintf(stderr, "CUDA error %s:%d: %s\n", __FILE__, __LINE__, cudaGetErrorString(err)); \
        std::abort(); \
    } \
} while(0)
#endif

__host__ cudaError_t copyMatrixToConstant(const quint64* h_matrix, size_t wordsNeeded);


__global__ void computeSpectrumKernelShort(
    quint64 * d_spectrum,
    const quint64* d_binomTable,
    int n,
    int k,
    int blockCount,
    quint64 chunkOffset,
    quint64 chunkSize,
    quint64 r
);
__host__ void launchSpectrumKernelShort(
    quint64 * d_spectrum,
    const quint64 * d_binomTable,
    int numOfBlocks,
    int threadsPerBlock,
    cudaStream_t stream,
    int n,
    int k,
    int blockCount,
    quint64 chunkOffset,
    quint64 chunkSize,
    quint64 r
);

__global__ void computeSpectrumKernelLong(
    uint64_t*        d_spectrum,       
    const uint64_t*  d_matrix,   
    int              numCols,     
    int              numRows,     
    int              wordsPerRow, 
    uint64_t         chunkSize,   
    int16_t*         d_startPositions, 
    uint64_t         masksPerThread,
    uint64_t         numStartMasks, 
    uint64_t         numOfOnes,
    uint64_t*        d_maskCounter
);

__host__ void launchSpectrumKernelLong(
    int              numBlocks,
    int              threadsPerBlock,
    cudaStream_t     stream,
    uint64_t*        d_spectrum,
    const uint64_t*  d_matrix,
    int              numCols,
    int              numRows,
    int              wordsPerRow,
    uint64_t         chunkSize,
    int16_t*         d_startPositions,
    uint64_t         masksPerThread,
    uint64_t         numStartMasks,
    uint64_t         numOfOnes,
    uint64_t*        d_maskCounter
);
__global__ void computeSpectrumKernelGrayShort(
    quint64* d_spectrum,
    int n,
    int k,
    int blockCount,
    quint64 chunkOffset,
    quint64 chunkSize
);
__host__ void launchSpectrumKernelGrayShort(
    int numOfBlocks,
    int threadsPerBlock,
    cudaStream_t stream,
    quint64* d_spectrum,
    int n,
    int k,
    int blockCount,
    quint64 chunkOffset,
    quint64 chunkSize
);