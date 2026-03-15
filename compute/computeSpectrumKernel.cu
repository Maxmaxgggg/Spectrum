#include "computeSpectrumKernel.cuh"

// Порождающая матрица в константной памяти
__constant__ quint64 d_matrix[Constants::MAX_CONST_WORDS];

// Функция для чтения слова из матрицы
__device__ __forceinline__  quint64 readMatrixWord(const quint64* matrix_global, int row, int wordIdx, int wordsPerRow)
{
    if ( matrix_global != nullptr ) {
        return matrix_global[(size_t)row * wordsPerRow + wordIdx];
    }
    else {
        return d_matrix[(size_t)row * wordsPerRow + wordIdx];
    }
}
__device__ inline quint64 getBinome(const quint64* binomTable, int n, int k) {
    return binomTable[n * (Constants::MAX_SHORT_CODE_LENGTH + 1) + k];
}

// Функция для генерации битовых масок на GPU
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

__device__ inline int bitPosFromSingleBit(quint64 x) {
    return __ffsll(x) - 1;
}
// Функция для генерации следующего массива позиций из текущего
__device__ __forceinline__ bool nextPositions(int16_t* a, int k, int n)
{
    // a[0..k-1] — строго возрастающий массив позиций

    int i = k - 1;

    // Ищем самый правый элемент, который ещё можно увеличить
    while (i >= 0 && a[i] == n - k + i)
        --i;

    // Если такого нет — это последняя комбинация
    if (i < 0)
        return false;

    // Увеличиваем его
    ++a[i];

    for (int j = i + 1; j < k; ++j)
        a[j] = a[i] + (j - i);

    return true;
}
// Функция для получения отличающихся элементов между двумя массивами позиций
__device__ __forceinline__ void diffPositions(
    const int16_t* prevPositions,
    const int16_t* currPositions,
    int            numOfPositions,
    int16_t*       changedPositions,
    int&           numChanged
) {
    int i = 0, j = 0;
    numChanged = 0;

    while ( i < numOfPositions || j < numOfPositions ) {
        if ( j == numOfPositions || (i < numOfPositions && prevPositions[i] < currPositions[j]) ) {
            changedPositions[numChanged++] = prevPositions[i++];
        }
        else if (i == numOfPositions || currPositions[j] < prevPositions[i]) {
            changedPositions[numChanged++] = currPositions[j++];
        }
        else {
            ++i;
            ++j;
        }
    }
}
// Функция для копирования матрицы с хоста в константную память
__host__ cudaError_t copyMatrixToConstant( const quint64* h_matrix, size_t matrixSizeInWords ) {
    // Определяем число байт для копирования
    size_t bytes = matrixSizeInWords * Constants::WORD_SIZE;
    // Проверяем, что не вышли за пределы константной памяти
    if (bytes > Constants::CONST_MEM_SIZE ) {
        throw("Error: matrix size (%zu bytes) exceeds constant memory limit");
        return cudaError_t::cudaErrorMemoryValueTooLarge;
    }
    // Копируем матрицу в константную память
    return cudaMemcpyToSymbol(d_matrix, h_matrix, bytes, 0, cudaMemcpyHostToDevice);
}









__host__ void launchSpectrumKernelShort(
    quint64* d_spectrum,
    const quint64* d_binomTable,
    int numOfBlocks,
    int threadsPerBlock,
    cudaStream_t stream,
    int n,
    int k,
    int blockCount,
    quint64 chunkOffset,
    quint64 chunkSize,
    quint64 r)
{
    computeSpectrumKernelShort << <numOfBlocks, threadsPerBlock, (n + 1) * sizeof(quint64), stream >> > (
        d_spectrum,
        d_binomTable,
        n,
        k,
        blockCount,
        chunkOffset,
        chunkSize,
        r);
}


__global__ void computeSpectrumKernelShort(
    quint64* d_spectrum,
    const quint64* d_binomTable,
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

    quint64 codeword[Constants::MAX_BLOCKWORDS];

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

    for (int w = 0; w < blockCount; ++w) weight += __popcll(codeword[w]);
    atomicAdd(&s_spectrum[weight], 1ULL);

    for (idx = start + 1; idx < end; ++idx) {
        quint64 nextCombIdx = chunkOffset + idx;
        quint64 next_mask = generateBitMaskGPU(d_binomTable, (unsigned)k, (unsigned)r, nextCombIdx);

        quint64 diff = mask ^ next_mask;

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
// Обертка для ядра для расчета частичных спектров длинных кодов
__host__ void launchSpectrumKernelLong(
    int numBlocks,
    int threadsPerBlock,
    cudaStream_t stream,
    uint64_t* d_spectrum,
    const uint64_t* d_matrix,
    int numCols,
    int numRows,
    int wordsPerRow,
    uint64_t chunkSize,
    int16_t* d_startPositions,
    uint64_t masksPerThread,
    uint64_t numStartMasks,
    uint64_t numOfOnes,
    uint64_t* d_maskCounter
) {
    size_t sharedBytes = (size_t)(numCols + 1) * sizeof(uint64_t);
    computeSpectrumKernelLong <<< numBlocks, threadsPerBlock, sharedBytes, stream >>> (
        d_spectrum,
        d_matrix,
        numCols,
        numRows,
        wordsPerRow,
        chunkSize,
        d_startPositions,
        masksPerThread,
        numStartMasks,
        numOfOnes,
        d_maskCounter
        );
}
__global__ void computeSpectrumKernelLong(
    uint64_t* d_spectrum,
    const uint64_t* d_matrix,
    int             numCols,
    int             numRows,
    int             wordsPerRow,
    uint64_t        chunkSize,
    int16_t* d_startPositions,
    uint64_t        masksPerThread,
    uint64_t        numStartMasks,
    uint64_t        numOfOnes,
    uint64_t*       d_maskCounter
)
{
    extern __shared__ uint64_t s_spectrum[];

    int tid = threadIdx.x;
    uint64_t gtid =
        (uint64_t)blockIdx.x * blockDim.x + (uint64_t)tid;

    /* -------- init shared histogram -------- */
    for (int i = tid; i <= numCols; i += blockDim.x)
        s_spectrum[i] = 0ULL;

    __syncthreads();

    /* -------- activity predicate -------- */
    uint64_t startRank = gtid * masksPerThread;

    bool threadIsActive =
        (gtid < numStartMasks) &&
        (startRank < chunkSize) &&
        (numOfOnes <= Constants::MAX_POSITIONS);

    /* -------- per-thread work -------- */
    if (threadIsActive) {

        const int16_t* slot =
            d_startPositions + gtid * Constants::MAX_POSITIONS;

        // positions
        int16_t a[Constants::MAX_POSITIONS];
        for (int i = 0; i < numOfOnes; ++i)
            a[i] = slot[i];

        // codeword
        uint64_t codeword[Constants::MAX_BLOCKWORDS];
        for (int w = 0; w < wordsPerRow; ++w)
            codeword[w] = 0ULL;

        /* ---- first mask ---- */
        for (int i = 0; i < numOfOnes; ++i) {
            int row = a[i];
            for (int w = 0; w < wordsPerRow; ++w)
                codeword[w] ^= readMatrixWord(d_matrix, row, w, wordsPerRow);
        }

        int weight = 0;
        for (int w = 0; w < wordsPerRow; ++w)
            weight += __popcll(codeword[w]);

        atomicAdd(&s_spectrum[weight], 1ULL);
        #ifdef _DEBUG
        atomicAdd(d_maskCounter, 1ULL);
        #endif
        /* ---- iterations ---- */
        uint64_t iters = masksPerThread;
        if (startRank + iters > chunkSize)
            iters = chunkSize - startRank;

        int16_t old_a[Constants::MAX_POSITIONS];

        for (uint64_t it = 1; it < iters; ++it) {

            for (int i = 0; i < numOfOnes; ++i)
                old_a[i] = a[i];

            if (!nextPositions(a, numOfOnes, numRows))
                break;

            int16_t changed[2 * Constants::MAX_POSITIONS];
            int numChanged;
            diffPositions(old_a, a, numOfOnes, changed, numChanged);

            if (numChanged > numOfOnes) {
                for (int w = 0; w < wordsPerRow; ++w)
                    codeword[w] = 0ULL;

                for (int i = 0; i < numOfOnes; ++i) {
                    int row = a[i];
                    for (int w = 0; w < wordsPerRow; ++w)
                        codeword[w] ^= readMatrixWord(d_matrix, row, w, wordsPerRow);
                }
            }
            else {
                for (int t = 0; t < numChanged; ++t) {
                    int row = changed[t];
                    for (int w = 0; w < wordsPerRow; ++w)
                        codeword[w] ^= readMatrixWord(d_matrix, row, w, wordsPerRow);
                }
            }

            weight = 0;
            for (int w = 0; w < wordsPerRow; ++w)
                weight += __popcll(codeword[w]);

            atomicAdd(&s_spectrum[weight], 1ULL);
            #ifdef _DEBUG
            atomicAdd(d_maskCounter, 1ULL);
            #endif
        }
    }

    /* -------- REQUIRED barrier -------- */
    __syncthreads();

    /* -------- merge shared -> global -------- */
    for (int i = tid; i <= numCols; i += blockDim.x) {
        uint64_t v = s_spectrum[i];
        if (v)
            atomicAdd(&d_spectrum[i], v);
    }
}


// Обертка для ядра для расчета полного спектра с использованием кода Грея для кодов с k < 64
__host__ void launchSpectrumKernelGrayShort(
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
    computeSpectrumKernelGrayShort << <numOfBlocks, threadsPerBlock, (n + 1) * sizeof(quint64), stream >> > (
        d_spectrum,
        n,
        k,
        blockCount,
        chunkOffset,
        chunkSize
        );
}

__global__ void computeSpectrumKernelGrayShort(
    quint64* d_spectrum,
    int n,
    int k,
    int blockCount,
    quint64 chunkOffset,   // начало (в Gray-порядке)
    quint64 chunkSize
) {
    extern __shared__ quint64 s_spectrum[];
    int tid = threadIdx.x;
    int threadsPerBlock = blockDim.x;

    // 1) инициализация shared
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
    quint64 codeword[Constants::MAX_BLOCKWORDS];
    #pragma unroll
    for (int b = 0; b < blockCount; ++b) codeword[b] = 0ULL;

    // 4) маска для k бит
    const quint64 maskAll = (k >= 64) ? ~0ULL : ((1ULL << k) - 1ULL);

    // gray function
    auto gray_of = [] __device__(quint64 i) -> quint64 { return (i ^ (i >> 1)); };

    // 5) начальный глобальный индекс и начальная маска
    quint64 idxGlobal = chunkOffset + startLocal;
    quint64 mask = (gray_of(idxGlobal) & maskAll);

    // 6) полный XOR для начальной маски
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

        // обработать все удалённые биты
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

    // 9) редукция shared -> global
    if (tid == 0) {
        for (int i = 0; i <= n; ++i) {
            quint64 v = s_spectrum[i];
            if (v) atomicAdd(&d_spectrum[i], v);
        }
    }
}







