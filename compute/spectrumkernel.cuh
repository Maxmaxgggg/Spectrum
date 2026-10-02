#pragma once

#include <cuda_runtime.h>
#include "constants.h"
#include <vector>
typedef uint64_t quint64;

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


// Ядра шаблонные по числу слов в строке и объявлены в .cu — снаружи нужны
// только обёртки запуска, они и выбирают вариант (wordvariants.h).
//
// Во всех ядрах n — длина кода (столбцов матрицы), k — её строк,
// wordsPerRow — 64-битных слов в строке, r — сколько строк складывается.
// У коротких кодов (k <= 63) матрица в константной памяти
// (copyMatrixToConstant).

// Простой XOR и Брауэр–Циммерман: сочетания из r строк с номерами
// [chunkOffset, chunkOffset + chunkSize) внутри множества slot.
__host__ void launchXorShort(
    quint64 * d_spectrum,
    const quint64 * d_binomTable,
    int blocks,
    int threadsPerBlock,
    cudaStream_t stream,
    int n,
    int k,
    int wordsPerRow,
    quint64 chunkOffset,
    quint64 chunkSize,
    quint64 r,
    MatrixSlot slot = MatrixSlot()
);

// Длинные коды (k >= 64): нить перебирает masksPerThread сочетаний подряд от
// своего стартового (d_startPositions, по MAX_POSITIONS позиций на нить).
// matrixGlobal — матрица в глобальной памяти; nullptr — в константной.
__host__ void launchXorLong(
    int              blocks,
    int              threadsPerBlock,
    cudaStream_t     stream,
    uint64_t*        d_spectrum,
    const uint64_t*  matrixGlobal,
    int              n,
    int              k,
    int              wordsPerRow,
    uint64_t         chunkSize,
    int16_t*         d_startPositions,
    uint64_t         masksPerThread,
    uint64_t         numStartMasks,
    uint64_t         r,
    uint64_t*        d_maskCounter,
    MatrixSlot       slot = MatrixSlot()
);
// Код Грея (и дуальный расчёт): маски с номерами [chunkOffset,
// chunkOffset + chunkSize) в порядке кода Грея.
__host__ void launchGray(
    int blocks,
    int threadsPerBlock,
    cudaStream_t stream,
    quint64* d_spectrum,
    int n,
    int k,
    int wordsPerRow,
    quint64 chunkOffset,
    quint64 chunkSize
);