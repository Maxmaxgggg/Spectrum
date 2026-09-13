#include "leonkernel.cuh"

#include "defines.h"

#include <stdexcept>
#include <string>

namespace {

// Заготовленные варианты числа слов в строке. В разделяемой памяти — до
// восьми (512 бит), в глобальной — до MAX_BLOCKWORDS.
constexpr int SHARED_MAX_WORDS = 8;

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

// Выкладывает слово a ^ b (b может быть nullptr) в буфер. Зовётся редко —
// только для слов нужного веса, — поэтому собирать слово заново дешевле, чем
// держать его в регистрах на каждом уровне перебора.
template <int WORDS>
__device__ __forceinline__ void emitWord(const LeonLaunch& P, const uint64_t* a, const uint64_t* b)
{
    const unsigned slot = atomicAdd(P.outCount, 1u);
    if (slot >= P.capacity)
        return;   // переполнение хост увидит по счётчику и повторит пачку
    uint64_t* dst = P.outWords + size_t(slot) * P.wordsPerRow;
    for (int w = 0; w < P.wordsPerRow; ++w)
        dst[w] = a[w] ^ (b ? b[w] : 0ULL);
}

__device__ __forceinline__ bool light(int weight, const LeonLaunch& P)
{
    return weight > 0 && weight <= P.maxWeight;
}

// Блок = попытка. В разделяемой памяти: порядок столбцов (uint16 x n), флаги
// строк для исключения (uint8 x k) и, если влезла, матрица k x WORDS; иначе
// матрица — в своём куске P.scratch.
template <int WORDS, bool GLOBAL>
__global__ void leonTrialsKernel(LeonLaunch P)
{
    extern __shared__ uint64_t s_mem[];
    const int k = P.rows;
    const int n = P.cols;

    uint64_t* m;
    uint16_t* order;
    if (GLOBAL) {
        m     = P.scratch + size_t(blockIdx.x) * size_t(k) * WORDS;
        order = reinterpret_cast<uint16_t*>(s_mem);
    } else {
        m     = s_mem;
        order = reinterpret_cast<uint16_t*>(m + size_t(k) * WORDS);
    }
    uint8_t* const flag = reinterpret_cast<uint8_t*>(order + n);
    __shared__ int s_src;
    __shared__ int s_pivotRow;

    const int tid = threadIdx.x;
    const int B   = blockDim.x;

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

    // 4. Перебор комбинаций до p строк. В регистрах держатся только префиксы;
    //    вес последнего уровня считается на лету, слово собирается заново
    //    лишь при выкладке.
    const int p = P.rowsPerTrial;
    uint64_t acc[WORDS], acc2[WORDS];

    for (int i = tid; i < k; i += B) {
        const uint64_t* row = m + size_t(i) * WORDS;
        int weight = 0;
        #pragma unroll
        for (int w = 0; w < WORDS; ++w) weight += __popcll(row[w]);
        if (light(weight, P))
            emitWord<WORDS>(P, row, nullptr);
    }
    if (p == 2) {
        // Пар мало, перекос между нитями роли не играет — Гаусс дороже.
        for (int i = tid; i < k - 1; i += B) {
            #pragma unroll
            for (int w = 0; w < WORDS; ++w) acc[w] = m[size_t(i) * WORDS + w];
            for (int j = i + 1; j < k; ++j) {
                const uint64_t* row = m + size_t(j) * WORDS;
                int weight = 0;
                #pragma unroll
                for (int w = 0; w < WORDS; ++w) weight += __popcll(acc[w] ^ row[w]);
                if (light(weight, P))
                    emitWord<WORDS>(P, acc, row);
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
            int weight = 0;
            #pragma unroll
            for (int w = 0; w < WORDS; ++w) {
                acc[w] = m[size_t(i) * WORDS + w] ^ m[size_t(j) * WORDS + w];
                weight += __popcll(acc[w]);
            }
            if (light(weight, P))
                emitWord<WORDS>(P, acc, nullptr);
            for (int l = j + 1; l < k; ++l) {
                const uint64_t* rowL = m + size_t(l) * WORDS;
                if (p == 3) {
                    int wt = 0;
                    #pragma unroll
                    for (int w = 0; w < WORDS; ++w) wt += __popcll(acc[w] ^ rowL[w]);
                    if (light(wt, P))
                        emitWord<WORDS>(P, acc, rowL);
                    continue;
                }
                int wt = 0;
                #pragma unroll
                for (int w = 0; w < WORDS; ++w) {
                    acc2[w] = acc[w] ^ rowL[w];
                    wt += __popcll(acc2[w]);
                }
                if (light(wt, P))
                    emitWord<WORDS>(P, acc2, nullptr);
                for (int q = l + 1; q < k; ++q) {
                    const uint64_t* rowQ = m + size_t(q) * WORDS;
                    int wq = 0;
                    #pragma unroll
                    for (int w = 0; w < WORDS; ++w) wq += __popcll(acc2[w] ^ rowQ[w]);
                    if (light(wq, P))
                        emitWord<WORDS>(P, acc2, rowQ);
                }
            }
        }
    }
}

size_t sharedBytesFor(int rows, int cols, int words, bool global)
{
    size_t bytes = size_t(cols) * sizeof(uint16_t) + size_t(rows);
    if (!global)
        bytes += size_t(rows) * words * sizeof(uint64_t);
    return bytes;
}

} // namespace

int leonPaddedWords(int wordsPerRow)
{
    static const int sizes[] = { 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 28, 32 };
    for (int candidate : sizes)
        if (candidate >= wordsPerRow) return candidate;
    return 0;
}

bool leonFitsShared(int rows, int cols, int wordsPerRow)
{
    if (rows <= 0 || cols <= 0 || wordsPerRow <= 0 || wordsPerRow > SHARED_MAX_WORDS)
        return false;
    return sharedBytesFor(rows, cols, wordsPerRow, false) <= size_t(Constants::MAX_SHARED_BYTES);
}

size_t leonSharedBytes(int rows, int cols, int wordsPerRow)
{
    if (rows <= 0 || cols <= 0 || leonPaddedWords(wordsPerRow) == 0)
        return 0;
    return sharedBytesFor(rows, cols, leonPaddedWords(wordsPerRow),
                          !leonFitsShared(rows, cols, wordsPerRow));
}

void launchLeonTrials(const LeonLaunch& launch, int threadsPerBlock, cudaStream_t stream)
{
    const int  words  = leonPaddedWords(launch.wordsPerRow);
    const bool global = !leonFitsShared(launch.rows, launch.cols, launch.wordsPerRow);
    if (words == 0)
        throw std::invalid_argument("строка матрицы длиннее, чем умеет ядро случайного поиска");
    if (global && launch.scratch == nullptr)
        throw std::invalid_argument("матрица не помещается в разделяемую память, а рабочий буфер не задан");
    if (launch.trials <= 0)
        return;

    const size_t sharedBytes = sharedBytesFor(launch.rows, launch.cols, words, global);
    if (sharedBytes > size_t(Constants::MAX_SHARED_BYTES))
        throw std::invalid_argument("порядок столбцов не помещается в разделяемую память");

    #define LAUNCH_LEON(W, G) leonTrialsKernel<W, G><<<launch.trials, threadsPerBlock, sharedBytes, stream>>>(launch)
    if (!global) {
        switch (words) {
            case 1: LAUNCH_LEON(1, false); break;   case 2: LAUNCH_LEON(2, false); break;
            case 3: LAUNCH_LEON(3, false); break;   case 4: LAUNCH_LEON(4, false); break;
            case 5: LAUNCH_LEON(5, false); break;   case 6: LAUNCH_LEON(6, false); break;
            case 7: LAUNCH_LEON(7, false); break;   default: LAUNCH_LEON(8, false); break;
        }
    } else {
        switch (words) {
            case  1: LAUNCH_LEON( 1, true); break;   case  2: LAUNCH_LEON( 2, true); break;
            case  3: LAUNCH_LEON( 3, true); break;   case  4: LAUNCH_LEON( 4, true); break;
            case  5: LAUNCH_LEON( 5, true); break;   case  6: LAUNCH_LEON( 6, true); break;
            case  7: LAUNCH_LEON( 7, true); break;   case  8: LAUNCH_LEON( 8, true); break;
            case 10: LAUNCH_LEON(10, true); break;   case 12: LAUNCH_LEON(12, true); break;
            case 14: LAUNCH_LEON(14, true); break;   case 16: LAUNCH_LEON(16, true); break;
            case 20: LAUNCH_LEON(20, true); break;   case 24: LAUNCH_LEON(24, true); break;
            case 28: LAUNCH_LEON(28, true); break;   default: LAUNCH_LEON(32, true); break;
        }
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
