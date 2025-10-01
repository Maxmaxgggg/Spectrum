#include "computeSpectrumKernel.cuh"

__constant__ quint64 d_binomTable[BINOM_TABLE_SIZE];
__constant__ quint64 d_matrix[MAX_CONST_WORDS];     



__device__ inline quint64 getBinome( const quint64* binomTable, int n, int k) {
    return binomTable[n * (MAX_K + 1) + k];
}

__device__ inline quint64 generateBitMaskGPU(  const quint64* binomTable, unsigned k, unsigned r, quint64 idx ) {
    if (r == 0) return 0ULL;
    if (r > k) return 0ULL;

    quint64 mask = 0ULL;
    unsigned nextPos = 0;   // минимальная позиция, которую ещё можно занять
    quint64 rank = idx;

    for (unsigned i = r; i > 0; --i) {
        unsigned j = nextPos;
        while (j <= k - i) {
            quint64 c = getBinome(binomTable, k - j - 1, i - 1);
            if (c <= rank) {
                rank -= c;
                ++j;
            }
            else {
                break;
            }
        }
        // j — позиция очередной единицы
        mask |= (1ULL << j);
        nextPos = j + 1;
    }
    return mask;
}



__device__ __forceinline__ quint64 readMatrixWord(const quint64* matrix_global, int row, int wordIdx, int blockCount)
{
    if (matrix_global) {
        return matrix_global[(size_t)row * blockCount + wordIdx];
    }
    else {
        // читаем из symbol d_matrix
        return d_matrix[(size_t)row * blockCount + wordIdx];
    }
}
__host__ void copyMatrixAndTableToSymbol( quint64* h_matrix,
                                          quint64** h_binomTable,
                                          quint64 wordsNeeded
) {
    // Преобразуем двумерный массив в одномерный для копирования на видеокарту
    int width = MAX_K + 1;
    quint64*  flat = (quint64*)malloc(BINOM_TABLE_SIZE*sizeof(quint64));
    for (int n = 0; n <= MAX_K; n++) {
        for (int r = 0; r <= n; r++) {
            printf("%d", h_binomTable[n][r]);
            flat[n * width + r] = h_binomTable[n][r];
        }
    }
    cudaMemcpyToSymbol( d_matrix,     h_matrix, wordsNeeded * sizeof(quint64),      0, cudaMemcpyHostToDevice );
    cudaMemcpyToSymbol( d_binomTable, flat,     BINOM_TABLE_SIZE * sizeof(quint64), 0, cudaMemcpyHostToDevice );
    free(flat);

}
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
)
{
    computeSpectrumKernel<<<numOfBlocks, threadsPerBlock, (n + 1) * sizeof(quint64), stream >>>(
        d_spectrum,
        n,
        k,
        blockCount,
        chunkOffset,
        chunkSize,
        r
    );
}
__global__ void computeSpectrumKernel(
                    quint64* d_spectrum,
                    int n,
                    int k,
                    int blockCount,
                    quint64 chunkOffset,
                    quint64 chunkSize,
                    quint64 r
) 
{

    extern __shared__ quint64 s_spectrum[];
    int tid = threadIdx.x;
    int threadsPerBlock = blockDim.x;

    // Инициализируем локальную shared-таблицу нулями (параллельно по потокам)
    for (int i = tid; i <= n; i += threadsPerBlock) {
        s_spectrum[i] = 0ULL;
    }
    __syncthreads();

    quint64 globalThreadIdx = (quint64)blockIdx.x * blockDim.x + threadIdx.x;
    quint64 totalThreads = (quint64)gridDim.x * blockDim.x;
    if ( globalThreadIdx >= chunkSize)
         return;
    // Каждый поток идёт по своему stride, но не выходит за границы chunkSize
    for (quint64 localIdx = globalThreadIdx; localIdx < chunkSize; localIdx += totalThreads) {
        quint64 combIdx = chunkOffset + localIdx;

        // Генерация комбинации (маски) для данного combIdx и r
        quint64 mask = generateBitMaskGPU(d_binomTable,(unsigned)k, (unsigned)r, combIdx );

        // Локальный codeword: XOR по строкам, хранится в регистре/локальной памяти
        // Предполагаем blockCount <= MAX_BLOCKWORDS. Если больше — нужно менять MAX_BLOCKWORDS.
        quint64 codeword[MAX_BLOCKWORDS];
        #pragma unroll
        for (int b = 0; b < blockCount; ++b) codeword[b] = 0ULL;

        // XOR строк (читаем по словам)
        // Перебираем строки 0..k-1 и если в mask бит установлен — XOR строку
        for (int row = 0; row < k; ++row) {
            if ((mask >> row) & 1ULL) {
                // XOR слова строки row
                for (int w = 0; w < blockCount; ++w) {
                    codeword[w] ^= readMatrixWord(d_matrix, row, w, blockCount);
                }
            }
        }

        // посчитаем вес (popcount) суммируем по словам
        int weight = 0;
        #pragma unroll
        for (int w = 0; w < blockCount; ++w) {
            weight += __popcll(codeword[w]);
        }

        // аккумулируем в shared-таблицу
        atomicAdd(&s_spectrum[weight], 1ULL);
    } // end for localIdx

    __syncthreads();

    // Сводим local s_spectrum в глобальный d_spectrum — делаем это одним потоком блока
    if (tid == 0) {
        for (int i = 0; i <= n; ++i) {
            quint64 v = s_spectrum[i];
            if (v) atomicAdd(&d_spectrum[i], v);
        }
    }
}