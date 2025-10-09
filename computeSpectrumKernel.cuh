#include <cuda_runtime.h>
#include "defines.h"
#include <vector>
typedef uint64_t quint64;

__host__ void copyMatrixAndTableToSymbol(quint64* h_matrix,
    quint64** h_binomTable,
    quint64 wordsNeeded
);
__global__ void computeSpectrumKernel(
    quint64* d_spectrum,
    int n,
    int k,
    int blockCount,
    quint64 chunkOffset,
    quint64 chunkSize,
    quint64 r
);
__host__ void launchSpectrumKernel(
    int numOfBlocks,
    int threadsPerBlock,
    cudaStream_t stream,
    quint64* d_spectrum,
    int n,
    int k,
    int blockCount,
    quint64 chunkOffset,
    quint64 chunkSize,
    quint64 r
);
__global__ void computeSpectrumKernelGray(
    quint64* d_spectrum,
    int n,
    int k,
    int blockCount,
    quint64 chunkOffset,
    quint64 chunkSize
);
__host__ void launchSpectrumKernelGray(
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