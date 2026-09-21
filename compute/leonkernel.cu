#include "leonkernel.cuh"

#include "defines.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace {

// Заготовленные варианты числа слов в строке. В разделяемой памяти — до
// восьми (512 бит), в глобальной — до MAX_BLOCKWORDS.
constexpr int SHARED_MAX_WORDS = 8;

// До стольких строк на лэйн опоры группы ищет один варп (см. Гаусс в ядре).
constexpr int SMALL_ROWS_PER_LANE = 8;

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

// Блок = попытка. В разделяемой памяти: если влезла, матрица k x WORDS;
// таблица четырёх русских (2^S x WORDS); порядок столбцов (uint16 x n);
// флаги опорных строк и ключи группы (uint8 x k). Не влезшая матрица — в
// своём куске P.scratch.
template <int WORDS, bool GLOBAL>
__global__ void leonTrialsKernel(LeonLaunch P)
{
    constexpr int STRIDE = rowStride(WORDS);
    extern __shared__ uint64_t s_mem[];
    const int k = P.rows;
    const int n = P.cols;
    const int S = P.gaussGroup;

    uint64_t* m;
    uint64_t* table;
    if (GLOBAL) {
        m     = P.scratch + size_t(blockIdx.x) * size_t(k) * STRIDE;
        table = s_mem;
    } else {
        m     = s_mem;
        table = m + size_t(k) * STRIDE;
    }
    uint16_t* const order = reinterpret_cast<uint16_t*>(table + (size_t(1) << S) * STRIDE);
    uint8_t*  const flag  = reinterpret_cast<uint8_t*>(order + n);   // строка уже опорная
    uint8_t*  const key   = flag + k;                                 // биты строки на столбцах группы

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

    // 3. Гаусс. S = 1 — по столбцу (матрица в разделяемой памяти, см.
    //    gaussGroupFor); иначе методом четырёх русских: столбцы порядка
    //    берутся группами по S. Опоры группы ищутся по ключам — битам
    //    строк на её столбцах — последовательным исключением в ключах, без
    //    сложения строк: столбец опорный, если у какой-то незанятой строки
    //    после исключения прежних опор группы бит стоит, — то же правило
    //    «первый подходящий», что у Гаусса по одному столбцу и у хоста,
    //    поэтому опорные столбцы те же.
    //    Найденные s опорных строк приводятся друг по другу (обратная
    //    матрица их ключей на опорных столбцах, s x s), по ним строится
    //    таблица всех 2^s сумм, и каждая прочая строка складывается с одной
    //    записью таблицы по своим битам на опорных столбцах — вместо до s
    //    сложений. Строки не переставляются, опорные помечаются флагом.
    //    Поиск опор — либо одним варпом по всем ключам (короткие матрицы,
    //    без барьеров блока), либо всем блоком: нить ведёт свои строки
    //    (tid, tid + B, …), их ключи и флаги читает и правит только она,
    //    чужое — лишь номер опоры и её ключ, на столбец один барьер.
    __shared__ int      s_min[2][32];       // минимум кандидата по варпам, по чётности столбца
    __shared__ int      s_pivotRow[8];      // опорные строки группы
    __shared__ int      s_pivotCol[8];      // их столбцы
    __shared__ unsigned s_inv[8];           // строки обратной матрицы ключей
    __shared__ uint64_t s_stage[8 * STRIDE];   // исходные опорные строки
    const int lane  = tid & 31;
    const int warp  = tid >> 5;
    const int warps = B >> 5;
    int found = 0;             // опорных строк найдено — у всех нитей одно и то же
    if (S == 1) {
        // Гаусс по одному столбцу — для матрицы в разделяемой памяти (см.
        // gaussGroupFor). Опора ищется одним варпом, прочие ждут; на
        // столбец два барьера.
        for (int idx = 0; idx < n && found < k; ++idx) {
            const int c = order[idx];
            if (tid < 32) {
                int best = 0x7fffffff;
                for (int r = tid; r < k; r += 32)
                    if (!flag[r] && bitAt(m + size_t(r) * STRIDE, c)) { best = r; break; }
                #pragma unroll
                for (int off = 16; off > 0; off >>= 1)
                    best = min(best, __shfl_xor_sync(0xffffffffu, best, off));
                if (tid == 0) s_min[idx & 1][0] = best;
            }
            __syncthreads();
            const int src = s_min[idx & 1][0];
            if (src == 0x7fffffff)
                continue;   // столбец зависим от прежних
            const uint64_t* pivot = m + size_t(src) * STRIDE;
            for (int r = tid; r < k; r += B) {
                if (r == src) { flag[r] = 1; continue; }
                uint64_t* row = m + size_t(r) * STRIDE;
                if (bitAt(row, c)) {
                    #pragma unroll
                    for (int w = 0; w < WORDS; ++w) row[w] ^= pivot[w];
                }
            }
            ++found;
            __syncthreads();
        }
    }
    for (int idx = 0; S > 1 && idx < n && found < k; idx += S) {
        const int width = min(S, n - idx);

        // Ключи своих строк: биты на столбцах группы, младший — первый столбец.
        for (int r = tid; r < k; r += B) {
            const uint64_t* row = m + size_t(r) * STRIDE;
            unsigned x = 0;
            for (int j = 0; j < width; ++j)
                x |= unsigned(bitAt(row, order[idx + j])) << j;
            key[r] = uint8_t(x);
        }

        // Опоры группы: по столбцу — незанятая строка с битом j, первая по
        // номеру; её ключ вычитается из ключей прочих строк с битом j.
        int s = 0;
        if (k <= 32 * SMALL_ROWS_PER_LANE) {
            // Короткая матрица: все ключи обходит один варп, без барьеров
            // блока — на 96 строках обмены и барьеры восьми варпов стоили
            // больше самого Гаусса. Остальные варпы ждут у барьера.
            __syncthreads();   // ключи чужих строк
            if (warp == 0) {
                for (int j = 0; j < width && found + s < k; ++j) {
                    int best = 0x7fffffff;
                    for (int r = lane; r < k; r += 32)
                        if (!flag[r] && ((key[r] >> j) & 1u)) { best = r; break; }
                    #pragma unroll
                    for (int off = 16; off > 0; off >>= 1)
                        best = min(best, __shfl_xor_sync(0xffffffffu, best, off));
                    if (best == 0x7fffffff)
                        continue;   // столбец зависим от прежних
                    const unsigned kp = key[best];
                    if (lane == 0) { s_pivotRow[s] = best; s_pivotCol[s] = order[idx + j]; }
                    __syncwarp();
                    for (int r = lane; r < k; r += 32) {
                        if (r == best) { flag[r] = 1; continue; }
                        if ((key[r] >> j) & 1u) key[r] ^= uint8_t(kp);
                    }
                    __syncwarp();
                    ++s;
                }
                if (lane == 0) s_min[0][0] = s;
            }
            __syncthreads();
            s = s_min[0][0];
        } else {
            // Длинная: нить ведёт свои строки, чужое — только номер опоры и
            // её ключ; на столбец один барьер.
            for (int j = 0; j < width && found + s < k; ++j) {
                int best = 0x7fffffff;
                for (int r = tid; r < k; r += B)
                    if (!flag[r] && ((key[r] >> j) & 1u)) { best = r; break; }
                #pragma unroll
                for (int off = 16; off > 0; off >>= 1)
                    best = min(best, __shfl_xor_sync(0xffffffffu, best, off));
                if (lane == 0) s_min[j & 1][warp] = best;
                __syncthreads();
                int p = 0x7fffffff;
                for (int w = 0; w < warps; ++w) p = min(p, s_min[j & 1][w]);
                if (p == 0x7fffffff)
                    continue;   // столбец зависим от прежних
                const unsigned kp = key[p];   // опору её хозяин не правит
                if (tid == 0) { s_pivotRow[s] = p; s_pivotCol[s] = order[idx + j]; }
                for (int r = tid; r < k; r += B) {
                    if (r == p) { flag[r] = 1; continue; }
                    if ((key[r] >> j) & 1u) key[r] ^= uint8_t(kp);
                }
                ++s;
            }
            __syncthreads();
        }
        found += s;
        if (s == 0)
            continue;

        // Обратная матрица ключей опор на опорных столбцах — Гаусс–Жордан
        // на s строках по 2s бит: ключи собирают s лэйнов нулевого варпа,
        // исключение — один лэйн; остальные варпы тем временем копируют
        // исходные опорные строки в буфер: приведённые пишутся на их место.
        if (warp == 0) {
            if (lane < s) {
                unsigned kk = 0;
                const uint64_t* row = m + size_t(s_pivotRow[lane]) * STRIDE;
                for (int t = 0; t < s; ++t)
                    kk |= unsigned(bitAt(row, s_pivotCol[t])) << t;
                s_inv[lane] = kk | (1u << (8 + lane));
            }
            __syncwarp();
            if (lane == 0) {
                unsigned a[8];
                for (int i = 0; i < 8; ++i) a[i] = i < s ? s_inv[i] : 0u;
                for (int t = 0; t < s; ++t) {
                    int src = t;   // найдётся: ключи опор на опорных столбцах независимы
                    while (src < s - 1 && !((a[src] >> t) & 1u)) ++src;
                    const unsigned tmp = a[t]; a[t] = a[src]; a[src] = tmp;
                    for (int i = 0; i < s; ++i)
                        if (i != t && ((a[i] >> t) & 1u)) a[i] ^= a[t];
                }
                for (int i = 0; i < s; ++i) s_inv[i] = a[i] >> 8;
            }
        }
        for (int e = tid; e < s * WORDS; e += B) {
            const int i = e / WORDS, w = e % WORDS;
            s_stage[i * STRIDE + w] = m[size_t(s_pivotRow[i]) * STRIDE + w];
        }
        __syncthreads();
        // Приведённая опора i — сумма исходных по строке i обратной матрицы;
        // она же — запись таблицы с одним битом i.
        for (int e = tid; e < s * WORDS; e += B) {
            const int i = e / WORDS, w = e % WORDS;
            uint64_t v = 0;
            for (int t = 0; t < s; ++t)
                if ((s_inv[i] >> t) & 1u) v ^= s_stage[t * STRIDE + w];
            m[size_t(s_pivotRow[i]) * STRIDE + w] = v;
            table[(size_t(1) << i) * STRIDE + w]  = v;
        }
        __syncthreads();
        // Остальные записи таблицы — суммы записей с одним битом.
        const int entries = 1 << s;
        for (int e = tid; e < entries * WORDS; e += B) {
            const int x = e / WORDS, w = e % WORDS;
            if (x & (x - 1)) {
                uint64_t v = 0;
                for (int i = 0; i < s; ++i)
                    if ((x >> i) & 1) v ^= table[(size_t(1) << i) * STRIDE + w];
                table[size_t(x) * STRIDE + w] = v;
            } else if (x == 0) {
                table[w] = 0ULL;
            }
        }
        __syncthreads();
        // Исключение: каждая своя строка, кроме опор группы, складывается с
        // записью таблицы по своим битам на опорных столбцах.
        for (int r = tid; r < k; r += B) {
            bool pivot = false;
            for (int i = 0; i < s; ++i) pivot |= (r == s_pivotRow[i]);
            if (pivot)
                continue;
            uint64_t* row = m + size_t(r) * STRIDE;
            uint64_t acc[WORDS];
            #pragma unroll
            for (int w = 0; w < WORDS; ++w) acc[w] = row[w];
            unsigned x = 0;
            for (int i = 0; i < s; ++i) {
                const int c = s_pivotCol[i];
                uint64_t v = 0;
                #pragma unroll
                for (int w = 0; w < WORDS; ++w) v |= (w == (c >> 6)) ? acc[w] : 0ULL;
                x |= unsigned((v >> (c & 63)) & 1ULL) << i;
            }
            if (x == 0)
                continue;
            const uint64_t* t = table + size_t(x) * STRIDE;
            #pragma unroll
            for (int w = 0; w < WORDS; ++w) row[w] = acc[w] ^ t[w];
        }
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
    const int p = P.rowsPerTrial;
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

size_t sharedBytesFor(int rows, int cols, int words, bool global, int group)
{
    size_t bytes = size_t(cols) * sizeof(uint16_t) + 2 * size_t(rows);
    bytes += (size_t(1) << group) * rowStride(words) * sizeof(uint64_t);
    if (!global)
        bytes += size_t(rows) * rowStride(words) * sizeof(uint64_t);
    return bytes;
}

// Статическая разделяемая память ядра: буфер опорных строк, минимумы по
// варпам, опоры, обратная матрица — с запасом.
size_t staticSharedBytes(int words)
{
    return 8 * size_t(rowStride(words)) * sizeof(uint64_t) + 512;
}

// Ширина группы четырёх русских. Матрице в разделяемой памяти группы не
// нужны — единица, Гаусс по столбцу: там сложение строк дёшево, а группа
// несёт обратную матрицу, буфер, таблицу и шесть барьеров (ncu, 256
// попыток: 336x96 — 2,9 мс по столбцу против 3,3 с таблицей, случайная
// [500,250] — 22 против 32). Матрице в глобальной памяти каждый проход по
// строкам — латентность памяти, и вшестеро меньше проходов дают 4,5 раза
// (Хэмминг [31,26]², [961,676]: 3,6 с → 0,8). Там группа — самая широкая
// до 8, при которой таблица не отнимает у мультипроцессора блоков: их
// число режет либо предел нитей, либо разделяемая память, и группа
// расширяется, пока память не стала теснее нитей (или не теснее, чем уже
// при группе из одного столбца). Ноль — не помещается даже с группой из
// одного столбца.
int gaussGroupFor(int rows, int cols, int words, bool global, int threadsPerBlock)
{
    if (!global)
        return sharedBytesFor(rows, cols, words, false, 1) <= size_t(Constants::MAX_SHARED_BYTES) ? 1 : 0;
    static int sharedPerSm = -1, reservedPerBlock = 0, threadsPerSm = 0;
    if (sharedPerSm < 0) {
        int device = 0;
        cudaGetDevice(&device);
        if (cudaDeviceGetAttribute(&sharedPerSm, cudaDevAttrMaxSharedMemoryPerMultiprocessor, device) != cudaSuccess
            || sharedPerSm <= 0)
            sharedPerSm = 48 * 1024;
        if (cudaDeviceGetAttribute(&reservedPerBlock, cudaDevAttrReservedSharedMemoryPerBlock, device) != cudaSuccess)
            reservedPerBlock = 0;
        if (cudaDeviceGetAttribute(&threadsPerSm, cudaDevAttrMaxThreadsPerMultiProcessor, device) != cudaSuccess
            || threadsPerSm <= 0)
            threadsPerSm = 1024;
    }
    const int byThreads = std::max(1, threadsPerSm / std::max(1, threadsPerBlock));
    auto blocksFor = [&](int group) {
        const size_t bytes = sharedBytesFor(rows, cols, words, global, group)
                           + staticSharedBytes(words) + size_t(reservedPerBlock);
        return int(size_t(sharedPerSm) / bytes);
    };
    int best = 0;
    for (int group = 1; group <= 8; ++group) {
        if (sharedBytesFor(rows, cols, words, global, group) > size_t(Constants::MAX_SHARED_BYTES))
            break;
        if (group > 1 && blocksFor(group) < std::min(byThreads, blocksFor(1)))
            break;
        best = group;
    }
    return best;
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
    return gaussGroupFor(rows, cols, wordsPerRow, false, LEON_THREADS) > 0;
}

size_t leonSharedBytes(int rows, int cols, int wordsPerRow)
{
    const int words = leonPaddedWords(wordsPerRow);
    if (rows <= 0 || cols <= 0 || words == 0)
        return 0;
    const bool global = !leonFitsShared(rows, cols, wordsPerRow);
    const int  group  = gaussGroupFor(rows, cols, words, global, LEON_THREADS);
    return group == 0 ? 0 : sharedBytesFor(rows, cols, words, global, group);
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

    const int group = gaussGroupFor(launch.rows, launch.cols, words, global, threadsPerBlock);
    if (group == 0)
        throw std::invalid_argument("порядок столбцов не помещается в разделяемую память");
    const size_t sharedBytes = sharedBytesFor(launch.rows, launch.cols, words, global, group);
    LeonLaunch L = launch;
    L.gaussGroup = group;

    #define LAUNCH_LEON(W, G) leonTrialsKernel<W, G><<<L.trials, threadsPerBlock, sharedBytes, stream>>>(L)
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
