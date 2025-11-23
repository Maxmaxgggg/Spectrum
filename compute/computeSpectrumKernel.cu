#include "computeSpectrumKernel.cuh"

__constant__ quint64 d_binomTable[BINOM_TABLE_SIZE];
__constant__ quint64 d_matrix[MAX_CONST_WORDS];

__device__ inline quint64 getBinome(const quint64* binomTable, int n, int k) {
    return binomTable[n * (MAX_K + 1) + k];
}

__device__ inline quint64 generateBitMaskGPU(const quint64* binomTable, unsigned k, unsigned r, quint64 idx) {
    if (r == 0) return 0ULL;
    if (r > k) return 0ULL;

    quint64 mask = 0ULL;
    unsigned nextPos = 0;
    quint64 rank = idx;

    for (unsigned i = r; i > 0; --i) {
        unsigned j = nextPos;
        while (j <= k - i) {
            quint64 c = getBinome(binomTable, k - j - 1, i - 1);
            if (c <= rank) {
                rank -= c;
                ++j;
            }
            else break;
        }
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
        return d_matrix[(size_t)row * blockCount + wordIdx];
    }
}

__host__ void copyMatrixAndTableToSymbol(quint64* h_matrix,
    quint64** h_binomTable,
    quint64 wordsNeeded)
{
    int width = MAX_K + 1;
    quint64* flat = (quint64*)malloc(BINOM_TABLE_SIZE * sizeof(quint64));
    for (int n = 0; n <= MAX_K; ++n) {
        for (int r = 0; r <= n; ++r) {
            flat[n * width + r] = h_binomTable[n][r];
        }
    }
    cudaMemcpyToSymbol(d_matrix, h_matrix, wordsNeeded * sizeof(quint64), 0, cudaMemcpyHostToDevice);
    cudaMemcpyToSymbol(d_binomTable, flat, BINOM_TABLE_SIZE * sizeof(quint64), 0, cudaMemcpyHostToDevice);
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
    quint64 r)
{
    computeSpectrumKernel << <numOfBlocks, threadsPerBlock, (n + 1) * sizeof(quint64), stream >> > (
        d_spectrum,
        n,
        k,
        blockCount,
        chunkOffset,
        chunkSize,
        r);
}

__device__ inline int bitPosFromSingleBit(quint64 x) {
    return __ffsll(x) - 1;
}

__global__ void computeSpectrumKernel(
    quint64* d_spectrum,
    int n,
    int k,
    int blockCount,
    quint64 chunkOffset,
    quint64 chunkSize,
    quint64 r)
{
    extern __shared__ quint64 s_spectrum[];
    int tid = threadIdx.x;
    int threadsPerBlock = blockDim.x;

    for (int i = tid; i <= n; i += threadsPerBlock) s_spectrum[i] = 0ULL;
    __syncthreads();

    quint64 globalThreadIdx = (quint64)blockIdx.x * blockDim.x + threadIdx.x;
    quint64 totalThreads = (quint64)gridDim.x * blockDim.x;
    if (globalThreadIdx >= totalThreads) return;

    quint64 combosPerThread = (chunkSize + totalThreads - 1) / totalThreads;
    quint64 start = globalThreadIdx * combosPerThread;
    if (start >= chunkSize) return;
    quint64 end = start + combosPerThread;
    if (end > chunkSize) end = chunkSize;

    quint64 codeword[MAX_BLOCKWORDS];
#pragma unroll
    for (int b = 0; b < blockCount; ++b) codeword[b] = 0ULL;

    quint64 idx = start;
    quint64 combIdx = chunkOffset + idx;
    quint64 mask = generateBitMaskGPU(d_binomTable, (unsigned)k, (unsigned)r, combIdx);

    quint64 temp = mask;
    while (temp) {
        quint64 single = temp & -temp;
        int pos = bitPosFromSingleBit(single);
        temp &= (temp - 1);
        for (int w = 0; w < blockCount; ++w) codeword[w] ^= readMatrixWord(d_matrix, pos, w, blockCount);
    }

    int weight = 0;
    #pragma unroll
    for (int w = 0; w < blockCount; ++w) weight += __popcll(codeword[w]);
    atomicAdd(&s_spectrum[weight], 1ULL);

    for (idx = start + 1; idx < end; ++idx) {
        quint64 nextCombIdx = chunkOffset + idx;
        quint64 next_mask = generateBitMaskGPU(d_binomTable, (unsigned)k, (unsigned)r, nextCombIdx);

        quint64 diff = mask ^ next_mask;
        // ќбработка всех изменившихс€ битов: может быть больше двух битов при лексикографическом пор€дке.
        // ƒл€ корректности перебираем все биты, которые были удалены, и все, которые добавлены.
        quint64 removed = mask & diff;
        while (removed) {
            quint64 single = removed & -removed;
            int remPos = bitPosFromSingleBit(single);
            removed &= (removed - 1);
            for (int w = 0; w < blockCount; ++w) {
                quint64 rowRem = readMatrixWord(d_matrix, remPos, w, blockCount);
                codeword[w] ^= rowRem;
            }
        }

        quint64 added = next_mask & diff;
        while (added) {
            quint64 single = added & -added;
            int addPos = bitPosFromSingleBit(single);
            added &= (added - 1);
            for (int w = 0; w < blockCount; ++w) {
                quint64 rowAdd = readMatrixWord(d_matrix, addPos, w, blockCount);
                codeword[w] ^= rowAdd;
            }
        }

        int weight2 = 0;
        #pragma unroll
        for (int w = 0; w < blockCount; ++w) weight2 += __popcll(codeword[w]);
        atomicAdd(&s_spectrum[weight2], 1ULL);

        mask = next_mask;
    }

    __syncthreads();

    if (tid == 0) {
        for (int i = 0; i <= n; ++i) {
            quint64 v = s_spectrum[i];
            if (v) atomicAdd(&d_spectrum[i], v);
        }
    }
}










__host__ void launchSpectrumKernelGray(
    int numOfBlocks,
    int threadsPerBlock,
    cudaStream_t stream,
    quint64* d_spectrum,
    int n,
    int k,
    int blockCount,
    quint64 chunkOffset,   // индекс Gray-элемента начала чанка
    quint64 chunkSize      // сколько Gray-элементов в чанке
) {
    computeSpectrumKernelGray << <numOfBlocks, threadsPerBlock, (n + 1) * sizeof(quint64), stream >> > (
        d_spectrum,
        n,
        k,
        blockCount,
        chunkOffset,
        chunkSize
        );
}

// ---------- device helper (reuse readMatrixWord, bitPosFromSingleBit) ----------
// (предполагаетс€, что readMatrixWord и bitPosFromSingleBit уже объ€влены в файле)

// ---------- Gray kernel ----------
__global__ void computeSpectrumKernelGray(
    quint64* d_spectrum,
    int n,
    int k,
    int blockCount,
    quint64 chunkOffset,   // начало (в Gray-пор€дке)
    quint64 chunkSize
) {
    extern __shared__ quint64 s_spectrum[];
    int tid = threadIdx.x;
    int threadsPerBlock = blockDim.x;

    // 1) инициализаци€ shared
    for (int i = tid; i <= n; i += threadsPerBlock) s_spectrum[i] = 0ULL;
    __syncthreads();

    quint64 globalThreadIdx = (quint64)blockIdx.x * blockDim.x + threadIdx.x;
    quint64 totalThreads = (quint64)gridDim.x * blockDim.x;
    if (globalThreadIdx >= totalThreads) return;

    // 2) строгое равномерное разбиение [0..chunkSize)
    quint64 base = chunkSize / totalThreads;
    quint64 rem = chunkSize % totalThreads;
    quint64 startLocal = globalThreadIdx * base + (globalThreadIdx < rem ? globalThreadIdx : rem);
    quint64 cnt = base + (globalThreadIdx < rem ? 1 : 0);
    if (cnt == 0) return;
    quint64 endLocal = startLocal + cnt; // exclusive

    // 3) подготовка local codeword
    quint64 codeword[MAX_BLOCKWORDS];
    #pragma unroll
    for (int b = 0; b < blockCount; ++b) codeword[b] = 0ULL;

    // 4) маска дл€ k бит
    const quint64 maskAll = (k >= 64) ? ~0ULL : ((1ULL << k) - 1ULL);

    // gray function
    auto gray_of = [] __device__(quint64 i) -> quint64 { return (i ^ (i >> 1)); };

    // 5) начальный глобальный индекс и начальна€ маска
    quint64 idxGlobal = chunkOffset + startLocal;
    quint64 mask = (gray_of(idxGlobal) & maskAll);

    // 6) полный XOR дл€ начальной маски
    quint64 temp = mask;
    while (temp) {
        quint64 lowbit = temp & (~temp + 1ULL); // safe lowbit
        int pos = bitPosFromSingleBit(lowbit);
        temp &= (temp - 1ULL);
        for (int w = 0; w < blockCount; ++w) codeword[w] ^= readMatrixWord(d_matrix, pos, w, blockCount);
    }

    // 7) аккумулируем вес
    int weight = 0;
    #pragma unroll
    for (int w = 0; w < blockCount; ++w) weight += __popcll(codeword[w]);
    atomicAdd(&s_spectrum[weight], 1ULL);
    // 8) основной цикл по локальному диапазону (без перекрытий)
    for (quint64 local = startLocal + 1; local < endLocal; ++local) {
        quint64 i = chunkOffset + local;
        quint64 next_mask = (gray_of(i) & maskAll);
        quint64 diff = mask ^ next_mask;

        // обработать все удалЄнные биты
        quint64 removed = mask & diff;
        while (removed) {
            quint64 lowbit = removed & (~removed + 1ULL);
            int remPos = bitPosFromSingleBit(lowbit);
            removed &= (removed - 1ULL);
            for (int w = 0; w < blockCount; ++w)
                codeword[w] ^= readMatrixWord(d_matrix, remPos, w, blockCount);
        }

        // обработать все добавленные биты
        quint64 added = next_mask & diff;
        while (added) {
            quint64 lowbit = added & (~added + 1ULL);
            int addPos = bitPosFromSingleBit(lowbit);
            added &= (added - 1ULL);
            for (int w = 0; w < blockCount; ++w)
                codeword[w] ^= readMatrixWord(d_matrix, addPos, w, blockCount);
        }

        // аккумулируем вес
        int weight2 = 0;
        #pragma unroll
        for (int w = 0; w < blockCount; ++w) weight2 += __popcll(codeword[w]);
        atomicAdd(&s_spectrum[weight2], 1ULL);
        mask = next_mask;
    }

    __syncthreads();

    // 9) редукци€ shared -> global
    if (tid == 0) {
        for (int i = 0; i <= n; ++i) {
            quint64 v = s_spectrum[i];
            if (v) atomicAdd(&d_spectrum[i], v);
        }
    }
}







