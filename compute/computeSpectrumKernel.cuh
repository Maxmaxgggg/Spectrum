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

// Какая из матриц перебирается. В обычном расчёте матрица одна. У
// Брауэра–Циммермана их несколько, они лежат в памяти подряд, и слово
// засчитывается, только если его не засчитало другое множество: то, на
// котором у слова меньше единиц, а при равенстве — первое по порядку.
// Маски множеств уезжают в константную память отдельно, copyMasksToConstant.
struct MatrixSlot
{
    int rowBase  = 0;   // первая строка этой матрицы в общем массиве
    int setIndex = 0;   // номер множества
    int setCount = 1;   // всего множеств; 1 — обычный перебор без проверки
};

// Маски информационных множеств: setCount масок по wordsPerRow слов подряд.
__host__ cudaError_t copyMasksToConstant(const quint64* h_masks, int setCount, int wordsPerRow);


// Ядро коротких кодов шаблонное и объявлено в .cu — снаружи нужна
// только обёртка запуска, она и выбирает вариант по числу слов.
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
    quint64 r,
    MatrixSlot slot = MatrixSlot()
);

// Ядра шаблонные по числу слов в строке и объявлены в .cu — снаружи
// нужны только обёртки запуска, они и выбирают вариант.

__host__ void launchSpectrumKernelLong(
    int              numBlocks,
    int              threadsPerBlock,
    cudaStream_t     stream,
    uint64_t*        d_spectrum,
    const uint64_t*  matrixGlobal,
    int              numCols,
    int              numRows,
    int              wordsPerRow,
    uint64_t         chunkSize,
    int16_t*         d_startPositions,
    uint64_t         masksPerThread,
    uint64_t         numStartMasks,
    uint64_t         numOfOnes,
    uint64_t*        d_maskCounter,
    MatrixSlot       slot = MatrixSlot()
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