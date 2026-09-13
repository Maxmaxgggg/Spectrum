#include "leonkernel.cuh"

#include "defines.h"

#include <stdexcept>
#include <string>

namespace {

// Строка длиннее восьми слов (512 бит) на видеокарте не перебирается: слова
// комбинаций живут в регистрах, и их число известно на этапе компиляции.
constexpr int MAX_LEON_WORDS = 8;

// Генератор порядка столбцов. Обязан совпадать с Leon::shuffledColumns на
// хосте бит в бит: тогда попытка с одним номером даёт одно и то же множество
// на CPU и на GPU, и результаты двух путей сравнимы напрямую — этим и
// проверяется ядро.
__device__ __forceinline__ uint64_t seedFor(uint64_t index)
{
    uint64_t z = index + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

__device__ __forceinline__ uint64_t xorshiftNext(uint64_t& state)
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

__device__ __forceinline__ bool bitAt(const uint64_t* row, int c)
{
    return (row[c >> 6] >> (c & 63)) & 1ULL;
}

template <int WORDS>
__device__ __forceinline__ void emitIfLight(const uint64_t* word, const LeonLaunch& P)
{
    int weight = 0;
    #pragma unroll
    for (int w = 0; w < WORDS; ++w)
        weight += __popcll(word[w]);
    if (weight == 0 || weight > P.maxWeight)
        return;
    const unsigned slot = atomicAdd(P.outCount, 1u);
    if (slot >= P.capacity)
        return;   // переполнение хост увидит по счётчику и повторит пачку
    uint64_t* dst = P.outWords + size_t(slot) * P.wordsPerRow;
    for (int w = 0; w < P.wordsPerRow; ++w)
        dst[w] = word[w];
}

// Блок = попытка. Разделяемая память: матрица k x WORDS, порядок столбцов
// (uint16 x n), флаги строк для исключения (uint8 x k).
template <int WORDS>
__global__ void leonTrialsKernel(LeonLaunch P)
{
    extern __shared__ uint64_t s_mem[];
    uint64_t* const m     = s_mem;
    uint16_t* const order = reinterpret_cast<uint16_t*>(m + size_t(P.rows) * WORDS);
    uint8_t*  const flag  = reinterpret_cast<uint8_t*>(order + P.cols);
    __shared__ int s_src;
    __shared__ int s_pivotRow;

    const int tid = threadIdx.x;
    const int B   = blockDim.x;
    const int k   = P.rows;
    const int n   = P.cols;

    // 1. Копия матрицы; слова за wordsPerRow — нули.
    for (int e = tid; e < k * WORDS; e += B) {
        const int r = e / WORDS, w = e % WORDS;
        m[e] = (w < P.wordsPerRow) ? P.matrix[size_t(r) * P.wordsPerRow + w] : 0ULL;
    }
    // 2. Случайный порядок столбцов — Фишер–Йетс одной нитью, n обменов.
    if (tid == 0) {
        for (int c = 0; c < n; ++c) order[c] = uint16_t(c);
        uint64_t state = seedFor(P.firstTrial + blockIdx.x) | 1ULL;
        for (int c = n - 1; c > 0; --c) {
            const int j = int(xorshiftNext(state) % uint64_t(c + 1));
            const uint16_t t = order[c]; order[c] = order[j]; order[j] = t;
        }
        s_pivotRow = 0;
    }
    __syncthreads();

    // 3. Гаусс: опорные столбцы берутся в случайном порядке, первый
    //    подходящий. То же правило, что и на хосте.
    for (int idx = 0; idx < n; ++idx) {
        const int pivotRow = s_pivotRow;
        if (pivotRow >= k)
            break;
        const int c = order[idx];

        if (tid == 0) s_src = 0x7fffffff;
        __syncthreads();
        for (int r = pivotRow + tid; r < k; r += B)
            if (bitAt(m + size_t(r) * WORDS, c))
                atomicMin(&s_src, r);
        __syncthreads();
        const int src = s_src;
        // Барьер до continue: иначе нить 0 успеет обнулить s_src для
        // следующего столбца раньше, чем остальные его прочитают.
        __syncthreads();
        if (src == 0x7fffffff)
            continue;

        if (src != pivotRow && tid < WORDS) {
            const uint64_t t = m[size_t(src) * WORDS + tid];
            m[size_t(src) * WORDS + tid]      = m[size_t(pivotRow) * WORDS + tid];
            m[size_t(pivotRow) * WORDS + tid] = t;
        }
        __syncthreads();

        // Флаги отдельно от исключения: иначе нить, стирающая бит c в слове
        // строки, обгоняла бы соседей, читающих этот бит для той же строки.
        for (int r = tid; r < k; r += B)
            flag[r] = (r != pivotRow) && bitAt(m + size_t(r) * WORDS, c);
        __syncthreads();
        for (int e = tid; e < k * WORDS; e += B) {
            const int r = e / WORDS, w = e % WORDS;
            if (flag[r])
                m[e] ^= m[size_t(pivotRow) * WORDS + w];
        }
        if (tid == 0) s_pivotRow = pivotRow + 1;
        __syncthreads();
    }
    // Ранг проверен хостом заранее; если что — попытка просто пустая.
    if (s_pivotRow < k)
        return;

    // 4. Перебор комбинаций до p строк.
    const int p = P.rowsPerTrial;
    uint64_t w1[WORDS], w2[WORDS], w3[WORDS], w4[WORDS];

    for (int i = tid; i < k; i += B) {
        #pragma unroll
        for (int w = 0; w < WORDS; ++w) w1[w] = m[size_t(i) * WORDS + w];
        emitIfLight<WORDS>(w1, P);
    }
    if (p == 2) {
        // Пар мало, перекос между нитями роли не играет — Гаусс дороже.
        for (int i = tid; i < k - 1; i += B) {
            #pragma unroll
            for (int w = 0; w < WORDS; ++w) w1[w] = m[size_t(i) * WORDS + w];
            for (int j = i + 1; j < k; ++j) {
                #pragma unroll
                for (int w = 0; w < WORDS; ++w) w2[w] = w1[w] ^ m[size_t(j) * WORDS + w];
                emitIfLight<WORDS>(w2, P);
            }
        }
    }
    else if (p >= 3) {
        // Пара (i, j) — префикс, нити берут их вперемежку: у каждой набор из
        // больших и малых j, и работа выравнивается. Разбор номера пары стоит
        // O(k), но размазан по внутреннему циклу.
        const int npairs = k * (k - 1) / 2;
        for (int idx = tid; idx < npairs; idx += B) {
            int i = 0, rem = idx;
            while (rem >= k - 1 - i) { rem -= k - 1 - i; ++i; }
            const int j = i + 1 + rem;
            #pragma unroll
            for (int w = 0; w < WORDS; ++w)
                w2[w] = m[size_t(i) * WORDS + w] ^ m[size_t(j) * WORDS + w];
            emitIfLight<WORDS>(w2, P);
            for (int l = j + 1; l < k; ++l) {
                #pragma unroll
                for (int w = 0; w < WORDS; ++w) w3[w] = w2[w] ^ m[size_t(l) * WORDS + w];
                emitIfLight<WORDS>(w3, P);
                if (p >= 4) {
                    for (int q = l + 1; q < k; ++q) {
                        #pragma unroll
                        for (int w = 0; w < WORDS; ++w) w4[w] = w3[w] ^ m[size_t(q) * WORDS + w];
                        emitIfLight<WORDS>(w4, P);
                    }
                }
            }
        }
    }
}

} // namespace

size_t leonSharedBytes(int rows, int cols, int wordsPerRow)
{
    if (rows <= 0 || cols <= 0 || wordsPerRow <= 0 || wordsPerRow > MAX_LEON_WORDS)
        return 0;
    const size_t bytes = size_t(rows) * wordsPerRow * sizeof(uint64_t)
                       + size_t(cols) * sizeof(uint16_t)
                       + size_t(rows);
    if (bytes > size_t(Constants::MAX_SHARED_BYTES))
        return 0;
    return bytes;
}

void launchLeonTrials(const LeonLaunch& launch, int threadsPerBlock, cudaStream_t stream)
{
    const size_t sharedBytes = leonSharedBytes(launch.rows, launch.cols, launch.wordsPerRow);
    if (sharedBytes == 0)
        throw std::invalid_argument("матрица не подходит для случайного поиска на видеокарте");
    if (launch.trials <= 0)
        return;

    #define LAUNCH_LEON(W) leonTrialsKernel<W><<<launch.trials, threadsPerBlock, sharedBytes, stream>>>(launch)
    switch (launch.wordsPerRow) {
        case 1: LAUNCH_LEON(1); break;   case 2: LAUNCH_LEON(2); break;
        case 3: LAUNCH_LEON(3); break;   case 4: LAUNCH_LEON(4); break;
        case 5: LAUNCH_LEON(5); break;   case 6: LAUNCH_LEON(6); break;
        case 7: LAUNCH_LEON(7); break;   default: LAUNCH_LEON(8); break;
    }
    #undef LAUNCH_LEON

    const cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess)
        throw std::invalid_argument(
            std::string("не удалось запустить ядро случайного поиска: ") + cudaGetErrorString(err)
            + " (блоков " + std::to_string(launch.trials)
            + ", нитей "  + std::to_string(threadsPerBlock)
            + ", разделяемой памяти " + std::to_string(sharedBytes) + " байт)");
}
