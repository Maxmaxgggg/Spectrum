#include "leonkernel.cuh"

#include "defines.h"

#include <stdexcept>
#include <string>

namespace {

// Заготовленные варианты числа слов в строке. В разделяемой памяти — до
// восьми (512 бит), в глобальной — до MAX_BLOCKWORDS.
constexpr int SHARED_MAX_WORDS = 8;

// Шаг строки в рабочей копии — слов. Нечётный: 64-битные обращения варпа
// к соседним строкам тогда попадают в разные банки разделяемой памяти, а с
// чётным шагом (6 слов = 48 байт) половина обращений сталкивалась.
__host__ __device__ constexpr int rowStride(int words) { return (words & 1) ? words : words + 1; }

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
// Тот же хеш, что Leon::hashWord на хосте — по wordsPerRow словам.
__device__ __forceinline__ uint64_t fingerprint(const LeonLaunch& P, const uint64_t* a, const uint64_t* b)
{
    uint64_t h = 0x9E3779B97F4A7C15ULL;
    for (int w = 0; w < P.wordsPerRow; ++w) {
        h ^= a[w] ^ (b ? b[w] : 0ULL);
        h *= 0xFF51AFD7ED558CCDULL;
        h ^= h >> 33;
    }
    return h == 0ULL ? 1ULL : h;   // ноль значит «пусто»
}

__device__ __forceinline__ void storeWord(const LeonLaunch& P, unsigned slot, const uint64_t* a, const uint64_t* b)
{
    uint64_t* dst = P.outWords + size_t(slot) * P.wordsPerRow;
    for (int w = 0; w < P.wordsPerRow; ++w)
        dst[w] = a[w] ^ (b ? b[w] : 0ULL);
}

// Слово найдено. Без таблицы — в буфер. С таблицей — сначала поиск без
// записи: повтор считается на месте. Новому слову сперва занимается место
// в буфере (нет места — слово не трогает таблицу и найдётся при повторе
// пачки), потом ячейка: проиграли гонку тому же слову — поимка засчитана
// ему, а слово всё равно выкладывается (хост повторы отбросит); ячейки не
// нашлось — выкладывается без счёта.
template <int WORDS>
__device__ __forceinline__ void emitWord(const LeonLaunch& P, const uint64_t* a, const uint64_t* b, int weight)
{
    if (P.seenFp != nullptr) {
        const uint64_t fp = fingerprint(P, a, b);
        uint64_t pos   = fp & P.seenMask;
        int      probe = 0;
        for (; probe < SEEN_PROBES; ++probe, pos = (pos + 1) & P.seenMask) {
            const uint64_t cur = P.seenFp[pos];
            if (cur == fp) {
                atomicAdd(P.seenHits + pos, 1u);
                return;
            }
            if (cur == 0ULL)
                break;
        }
        const unsigned slot = atomicAdd(P.outCount, 1u);
        if (slot >= P.capacity)
            return;
        for (; probe < SEEN_PROBES; ++probe, pos = (pos + 1) & P.seenMask) {
            const uint64_t old = atomicCAS(reinterpret_cast<unsigned long long*>(P.seenFp + pos),
                                           0ULL, static_cast<unsigned long long>(fp));
            if (old == 0ULL) {
                // Новое: вес пишет только победитель CAS, счётчик — все.
                P.seenWeight[pos] = uint16_t(weight);
                atomicAdd(P.seenHits + pos, 1u);
                atomicAdd(P.seenCount, 1u);
                break;
            }
            if (old == fp) {
                atomicAdd(P.seenHits + pos, 1u);
                break;
            }
        }
        storeWord(P, slot, a, b);
        return;
    }
    const unsigned slot = atomicAdd(P.outCount, 1u);
    if (slot >= P.capacity)
        return;   // переполнение хост увидит по счётчику и повторит пачку
    storeWord(P, slot, a, b);
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
    constexpr int STRIDE = rowStride(WORDS);
    extern __shared__ uint64_t s_mem[];
    const int k = P.rows;
    const int n = P.cols;

    uint64_t* m;
    uint16_t* order;
    if (GLOBAL) {
        m     = P.scratch + size_t(blockIdx.x) * size_t(k) * STRIDE;
        order = reinterpret_cast<uint16_t*>(s_mem);
    } else {
        m     = s_mem;
        order = reinterpret_cast<uint16_t*>(m + size_t(k) * STRIDE);
    }
    uint8_t* const flag = reinterpret_cast<uint8_t*>(order + n);   // строка уже опорная

    const int tid = threadIdx.x;
    const int B   = blockDim.x;

    // 1. Копия матрицы; слова за wordsPerRow — нули. Флаги опорных строк — нули.
    for (int e = tid; e < k * WORDS; e += B) {
        const int r = e / WORDS, w = e % WORDS;
        m[size_t(r) * STRIDE + w] = (w < P.wordsPerRow) ? P.matrix[size_t(r) * P.wordsPerRow + w] : 0ULL;
    }
    for (int r = tid; r < k; r += B)
        flag[r] = 0;
    // 2. Случайный порядок столбцов — Фишер–Йетс одной нитью, n обменов.
    if (tid == 0) {
        for (int c = 0; c < n; ++c) order[c] = uint16_t(c);
        uint64_t state = seedFor(P.firstTrial + blockIdx.x) | 1ULL;
        for (int c = n - 1; c > 0; --c) {
            const int j = int(__umul64hi(xorshiftNext(state), uint64_t(c + 1)));
            const uint16_t t = order[c]; order[c] = order[j]; order[j] = t;
        }
    }
    __syncthreads();

    // 3. Гаусс: опорные столбцы берутся в случайном порядке, первый
    //    подходящий — то же правило, что и на хосте. Строки не
    //    переставляются: опорная строка помечается использованной и в
    //    дальнейшем поиске не участвует; перебору порядок строк безразличен.
    //    Нить ведёт свои строки целиком (tid, tid + B, …): бит опорного
    //    столбца читается из своей строки до её изменения, и флаги не нужны.
    //    На столбец два барьера вместо пяти: после поиска опоры и после
    //    исключения.
    __shared__ int s_src[2];   // опорная строка столбца; по чётности столбца, чтобы
                               // нулевой варп мог писать следующую, пока остальные читают эту
    int found = 0;             // опорных строк найдено — у всех нитей одно и то же
    for (int idx = 0; idx < n && found < k; ++idx) {
        const int c = order[idx];

        // Поиск — одним варпом: у лэйна строки lane, lane + 32, … по
        // возрастанию, первая подходящая и есть его минимум; минимум по
        // варпу — обменами. Остальные варпы ждут у барьера, им тут нечего
        // считать.
        if (tid < 32) {
            int best = 0x7fffffff;
            for (int r = tid; r < k; r += 32)
                if (!flag[r] && bitAt(m + size_t(r) * STRIDE, c)) { best = r; break; }
            #pragma unroll
            for (int off = 16; off > 0; off >>= 1)
                best = min(best, __shfl_xor_sync(0xffffffffu, best, off));
            if (tid == 0) s_src[idx & 1] = best;
        }
        __syncthreads();
        const int src = s_src[idx & 1];
        if (src == 0x7fffffff)
            continue;   // столбец зависим от прежних

        // Исключение: все прочие строки с единицей в c складываются с
        // опорной. Опорная строка не меняется, её читают все.
        const uint64_t* pivot = m + size_t(src) * STRIDE;
        for (int r = tid; r < k; r += B) {
            if (r == src) {
                flag[r] = 1;
                continue;
            }
            uint64_t* row = m + size_t(r) * STRIDE;
            if (bitAt(row, c)) {
                #pragma unroll
                for (int w = 0; w < WORDS; ++w) row[w] ^= pivot[w];
            }
        }
        ++found;
        __syncthreads();
    }
    // Ранг проверен хостом заранее; если что — попытка просто пустая.
    if (found < k)
        return;

    // 4. Перебор комбинаций до p строк. Единица работы — варп, а не нить:
    //    у 32 лэйнов либо общий префикс в регистрах и свои хвосты подряд
    //    (строки читаются соседние — без конфликтов банков при нечётном
    //    шаге), либо свои префиксы и общий хвост (строка читается
    //    широковещательно). В обоих случаях лэйны делают одинаковое число
    //    шагов: раньше у каждой нити был свой хвост своей длины, и в варпе
    //    работало в среднем 8 лэйнов из 32.
    const int p     = P.rowsPerTrial;
    const int lane  = tid & 31;
    const int warp  = tid >> 5;
    const int warps = B >> 5;
    uint64_t acc[WORDS], acc2[WORDS];

    auto rowAt = [&](int r) { return m + size_t(r) * STRIDE; };
    auto load = [&](uint64_t* dst, const uint64_t* row) {
        #pragma unroll
        for (int w = 0; w < WORDS; ++w) dst[w] = row[w];
    };
    auto weightXor = [&](const uint64_t* a, const uint64_t* b) {
        int wt = 0;
        #pragma unroll
        for (int w = 0; w < WORDS; ++w) wt += __popcll(a[w] ^ b[w]);
        return wt;
    };
    auto weightOf = [&](const uint64_t* a) {
        int wt = 0;
        #pragma unroll
        for (int w = 0; w < WORDS; ++w) wt += __popcll(a[w]);
        return wt;
    };

    // Одиночные строки — по нитям.
    for (int i = tid; i < k; i += B) {
        const uint64_t* row = rowAt(i);
        const int weight = weightOf(row);
        if (light(weight, P))
            emitWord<WORDS>(P, row, nullptr, weight);
    }
    if (p < 2)
        return;

    if (p == 2) {
        // Варп берёт строку i, лэйны — j > i подряд.
        for (int i = warp; i < k - 1; i += warps) {
            load(acc, rowAt(i));
            for (int j = i + 1 + lane; j < k; j += 32) {
                const uint64_t* row = rowAt(j);
                const int wt = weightXor(acc, row);
                if (light(wt, P))
                    emitWord<WORDS>(P, acc, row, wt);
            }
        }
        return;
    }

    if (p == 3) {
        // Пары (i, j) с длинным хвостом (j < 32): варп берёт пару, лэйны —
        // l > j подряд. С коротким (j >= 32): варп берёт j и до 32 строк i,
        // лэйн — своё i, хвост l > j у всех общий. И там и там варп
        // заполнен не меньше чем на две трети; раздача единиц по варпам —
        // круговая, счёт единиц у всех варпов один и тот же.
        const int split = k < 32 ? k : 32;
        int unit = 0;
        for (int j = 1; j < split; ++j)
            for (int i = 0; i < j; ++i) {
                if ((unit++ % warps) != warp)
                    continue;
                const uint64_t* rj = rowAt(j);
                load(acc, rowAt(i));
                #pragma unroll
                for (int w = 0; w < WORDS; ++w) acc[w] ^= rj[w];
                if (lane == 0) {
                    const int wt = weightOf(acc);
                    if (light(wt, P))
                        emitWord<WORDS>(P, acc, nullptr, wt);
                }
                for (int l = j + 1 + lane; l < k; l += 32) {
                    const uint64_t* row = rowAt(l);
                    const int wt = weightXor(acc, row);
                    if (light(wt, P))
                        emitWord<WORDS>(P, acc, row, wt);
                }
            }
        for (int j = split; j < k; ++j)
            for (int c = 0; c < j; c += 32) {
                if ((unit++ % warps) != warp)
                    continue;
                const int  i    = c + lane;
                const bool mine = i < j;
                const uint64_t* rj = rowAt(j);
                if (mine) {
                    load(acc, rowAt(i));
                    #pragma unroll
                    for (int w = 0; w < WORDS; ++w) acc[w] ^= rj[w];
                    const int wt = weightOf(acc);
                    if (light(wt, P))
                        emitWord<WORDS>(P, acc, nullptr, wt);
                }
                for (int l = j + 1; l < k; ++l) {
                    const uint64_t* row = rowAt(l);
                    if (mine) {
                        const int wt = weightXor(acc, row);
                        if (light(wt, P))
                            emitWord<WORDS>(P, acc, row, wt);
                    }
                }
            }
        return;
    }

    // p == 4: варп берёт пару (i, j), хвост l у всех общий, а четвёртая
    // строка q > l — по лэйнам подряд.
    int unit = 0;
    for (int j = 1; j < k; ++j)
        for (int i = 0; i < j; ++i) {
            if ((unit++ % warps) != warp)
                continue;
            const uint64_t* rj = rowAt(j);
            load(acc, rowAt(i));
            #pragma unroll
            for (int w = 0; w < WORDS; ++w) acc[w] ^= rj[w];
            if (lane == 0) {
                const int wt = weightOf(acc);
                if (light(wt, P))
                    emitWord<WORDS>(P, acc, nullptr, wt);
            }
            for (int l = j + 1; l < k; ++l) {
                const uint64_t* rl = rowAt(l);
                #pragma unroll
                for (int w = 0; w < WORDS; ++w) acc2[w] = acc[w] ^ rl[w];
                if (lane == 0) {
                    const int wt = weightOf(acc2);
                    if (light(wt, P))
                        emitWord<WORDS>(P, acc2, nullptr, wt);
                }
                for (int q = l + 1 + lane; q < k; q += 32) {
                    const uint64_t* row = rowAt(q);
                    const int wt = weightXor(acc2, row);
                    if (light(wt, P))
                        emitWord<WORDS>(P, acc2, row, wt);
                }
            }
        }
}

__global__ void seenRehashKernel(const uint64_t* oldFp, const unsigned* oldHits, const uint16_t* oldWeight,
                                 uint64_t oldCapacity,
                                 uint64_t* newFp, unsigned* newHits, uint16_t* newWeight, uint64_t newMask)
{
    const uint64_t i = uint64_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= oldCapacity)
        return;
    const uint64_t fp = oldFp[i];
    if (fp == 0ULL)
        return;
    // Новая таблица вдвое больше и не переполнена — ячейка найдётся.
    uint64_t pos = fp & newMask;
    for (;;) {
        const uint64_t old = atomicCAS(reinterpret_cast<unsigned long long*>(newFp + pos),
                                       0ULL, static_cast<unsigned long long>(fp));
        if (old == 0ULL) {
            newHits[pos]   = oldHits[i];
            newWeight[pos] = oldWeight[i];
            return;
        }
        pos = (pos + 1) & newMask;
    }
}

size_t sharedBytesFor(int rows, int cols, int words, bool global)
{
    size_t bytes = size_t(cols) * sizeof(uint16_t) + size_t(rows);
    if (!global)
        bytes += size_t(rows) * rowStride(words) * sizeof(uint64_t);
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

int leonRowStride(int wordsPerRow)
{
    const int padded = leonPaddedWords(wordsPerRow);
    return padded == 0 ? 0 : rowStride(padded);
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

void launchSeenRehash(const uint64_t* oldFp, const unsigned* oldHits, const uint16_t* oldWeight,
                      uint64_t oldCapacity,
                      uint64_t* newFp, unsigned* newHits, uint16_t* newWeight, uint64_t newMask,
                      cudaStream_t stream)
{
    const unsigned threads = 256;
    const unsigned blocks  = unsigned((oldCapacity + threads - 1) / threads);
    seenRehashKernel<<<blocks, threads, 0, stream>>>(oldFp, oldHits, oldWeight, oldCapacity,
                                                     newFp, newHits, newWeight, newMask);
    const cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess)
        throw std::invalid_argument(
            std::string("не удалось запустить перекладку таблицы виденных слов: ") + cudaGetErrorString(err));
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
