#include "leonkernel.cuh"

#include "defines.h"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace {

// Заготовленные варианты числа слов в строке — до MAX_BLOCKWORDS, что в
// разделяемой памяти, что в глобальной.


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
// таблица четырёх русских (2^S x WORDS); ключи окна (uint32 x k); порядок
// столбцов (uint16 x n); флаги опорных строк и ключи группы (uint8 x k);
// пометки опорных столбцов (uint8 x n). Не влезшая матрица — в своём куске
// P.scratch, там же хеш-таблица окна.
// Регистров — под четыре блока на мультипроцессор у коротких строк (64 на
// нить) и два у длинных (128): опорная строка и суммы живут в регистрах,
// и без этой рамки компилятор берёт 72 и оставляет три блока.
template <int WORDS, bool GLOBAL>
__global__ void __launch_bounds__(LEON_THREADS, WORDS <= 8 ? 4 : 2) leonTrialsKernel(LeonLaunch P)
{
    constexpr int STRIDE = rowStride(WORDS);
    extern __shared__ uint64_t s_mem[];
    const int k = P.rows;
    const int n = P.cols;
    const int S = P.gaussGroup;

    uint64_t* const scratch = P.scratch ? P.scratch + size_t(blockIdx.x) * P.scratchWords : nullptr;
    uint64_t* m;
    uint64_t* table;
    if (GLOBAL) {
        m     = scratch;
        table = s_mem;
    } else {
        m     = s_mem;
        table = m + size_t(k) * STRIDE;
    }
    uint32_t* const winKey   = reinterpret_cast<uint32_t*>(table + (P.gaussTable ? (size_t(1) << S) * STRIDE : 0));
    uint16_t* const order    = reinterpret_cast<uint16_t*>(winKey + (P.window > 0 ? ((k + 1) & ~1) : 0));
    uint16_t* const rowOf    = order + n;                                // строка i-й по порядку опоры
    uint8_t*  const flag     = reinterpret_cast<uint8_t*>(rowOf + k);   // строка уже опорная
    uint8_t*  const key      = flag + k;                                 // биты строки на столбцах группы
    uint8_t*  const colPivot = key + k;                                  // столбец опорный

    const int tid = threadIdx.x;
    const int B   = blockDim.x;

    // 1. Копия матрицы; слова за wordsPerRow — нули. Флаги строк и столбцов — нули.
    for (int e = tid; e < k * WORDS; e += B) {
        const int r = e / WORDS, w = e % WORDS;
        m[size_t(r) * STRIDE + w] = (w < P.wordsPerRow) ? P.matrix[size_t(r) * P.wordsPerRow + w] : 0ULL;
    }
    for (int r = tid; r < k; r += B)
        flag[r] = 0;
    for (int c = tid; c < n; c += B)
        colPivot[c] = 0;
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
        // gaussGroupFor). Строки не переставляются: список кандидатов
        // (cand — строки, ещё не ставшие опорными) сжимается на месте, и
        // опору ищут по нему все варпы разом — свои кандидаты по
        // (tid, tid + B, …), минимум по варпам через s_min. На столбец два
        // барьера.
        //
        // Профиль (clock64, [961,676] в большой разделяемой памяти, один
        // блок на мультипроцессор): исключение упирается в пропускную
        // способность разделяемой памяти (полстроки на опору × 34 слова —
        // около 700 тактов на столбец), а поиск одним варпом стоил ещё
        // 700 и перестановка строк с барьером — 270. Биты своих строк
        // загружаются разом (маска need), опорная строка — в регистрах.
        uint16_t* const cand = rowOf;   // до конца Гаусса — список кандидатов
        for (int r = tid; r < k; r += B)
            cand[r] = uint16_t(r);
        __syncthreads();
        int remaining = k;
        for (int idx = 0; idx < n && found < k; ++idx) {
            const int c  = order[idx];
            const int cw = c >> 6;
            const uint64_t cm = 1ULL << (c & 63);
            int best = 0x7fffffff;
            for (int i = tid; i < remaining && best == 0x7fffffff; i += B * 4) {
                // По четыре кандидата за шаг: загрузки независимы.
                int      r[4];
                uint64_t v[4];
                #pragma unroll
                for (int j = 0; j < 4; ++j) {
                    r[j] = (i + B * j < remaining) ? int(cand[i + B * j]) : -1;
                    v[j] = r[j] >= 0 ? m[size_t(r[j]) * STRIDE + cw] : 0ULL;
                }
                #pragma unroll
                for (int j = 0; j < 4; ++j)
                    if (best == 0x7fffffff && r[j] >= 0 && (v[j] & cm)) best = (i + B * j);   // позиция в cand
            }
            #pragma unroll
            for (int off = 16; off > 0; off >>= 1)
                best = min(best, __shfl_xor_sync(0xffffffffu, best, off));
            if (lane == 0) s_min[idx & 1][warp] = best;
            __syncthreads();
            int pos = 0x7fffffff;
            for (int w = 0; w < warps; ++w) pos = min(pos, s_min[idx & 1][w]);
            if (pos == 0x7fffffff)
                continue;   // столбец зависим от прежних
            const int src = cand[pos];
            uint64_t pv[WORDS];
            #pragma unroll
            for (int w = 0; w < WORDS; ++w) pv[w] = m[size_t(src) * STRIDE + w];
            // Свои строки с единицей в столбце — маской, загрузки разом.
            unsigned need = 0;
            #pragma unroll
            for (int i = 0; i < 16; ++i) {
                const int r = tid + i * B;
                if (r < k && r != src && (m[size_t(r) * STRIDE + cw] & cm))
                    need |= 1u << i;
            }
            for (int r = tid + 16 * B; r < k; r += B)   // строк больше 16 на нить — редкость
                if (r != src && (m[size_t(r) * STRIDE + cw] & cm)) {
                    uint64_t* row = m + size_t(r) * STRIDE;
                    #pragma unroll
                    for (int w = 0; w < WORDS; ++w) row[w] ^= pv[w];
                }
            while (need) {
                const int i = __ffs(need) - 1;
                need &= need - 1;
                uint64_t* row = m + size_t(tid + i * B) * STRIDE;
                #pragma unroll
                for (int w = 0; w < WORDS; ++w) row[w] ^= pv[w];
            }
            // Опора выбывает из кандидатов: на её место — последний. Список
            // читается только при поиске, после барьера.
            if (tid == 0) {
                colPivot[c] = 1;
                cand[pos]   = cand[remaining - 1];
                cand[remaining - 1] = uint16_t(src);   // хвост списка — опоры по порядку с конца
            }
            --remaining;
            ++found;
            __syncthreads();
        }
        // Хвост cand — опоры в порядке появления с конца: rowOf[i] = опора i.
        // Кандидатов остаться не должно (ранг проверен хостом).
        for (int i = tid; i < k / 2; i += B) {
            const uint16_t a = cand[i], b = cand[k - 1 - i];
            cand[i] = b; cand[k - 1 - i] = a;
        }
        __syncthreads();
    }
    long long gprof[6] = { 0, 0, 0, 0, 0, 0 };   // PROFILE
    int groups = 0;
    for (int idx = 0; S > 1 && idx < n && found < k; idx += S) {
        const int width = min(S, n - idx);
        long long g0 = clock64(); ++groups;   // PROFILE

        // Ключи своих строк: биты на столбцах группы, младший — первый столбец.
        for (int r = tid; r < k; r += B) {
            const uint64_t* row = m + size_t(r) * STRIDE;
            unsigned x = 0;
            for (int j = 0; j < width; ++j)
                x |= unsigned(bitAt(row, order[idx + j])) << j;
            key[r] = uint8_t(x);
        }

        long long g1 = clock64(); gprof[0] += g1 - g0;   // PROFILE keys
        // Опоры группы: по столбцу — незанятая строка с битом j, первая по
        // номеру; её ключ вычитается из ключей прочих строк с битом j.
        int s = 0;
        if (k <= 32 * P.searchRowsPerLane) {
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
        long long g2 = clock64(); gprof[1] += g2 - g1;   // PROFILE search
        if (s == 0)
            continue;
        if (tid < s) { colPivot[s_pivotCol[tid]] = 1; rowOf[found - s + tid] = uint16_t(s_pivotRow[tid]); }

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
            if (P.gaussTable)
                table[(size_t(1) << i) * STRIDE + w] = v;
        }
        __syncthreads();
        if (P.gaussTable) {
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
        }
        long long g3 = clock64(); gprof[2] += g3 - g2;   // PROFILE phase B
        // Исключение: каждая своя строка, кроме опор группы, складывается с
        // записью таблицы по своим битам на опорных столбцах — или прямо с
        // приведёнными опорами по этим битам, если таблицы нет.
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
            if (P.gaussTable) {
                const uint64_t* t = table + size_t(x) * STRIDE;
                #pragma unroll
                for (int w = 0; w < WORDS; ++w) acc[w] ^= t[w];
            } else {
                for (int i = 0; i < s; ++i) {
                    if (!((x >> i) & 1u)) continue;
                    const uint64_t* t = m + size_t(s_pivotRow[i]) * STRIDE;
                    #pragma unroll
                    for (int w = 0; w < WORDS; ++w) acc[w] ^= t[w];
                }
            }
            #pragma unroll
            for (int w = 0; w < WORDS; ++w) row[w] = acc[w];
        }
        __syncthreads();
        long long g4 = clock64(); gprof[3] += g4 - g3;   // PROFILE phase C
    }
    if (S > 1 && blockIdx.x == 0 && tid == 0 && P.maxWeight == 0) {   // PROFILE
        for (int i = 0; i < 4; ++i) P.outWords[i] = uint64_t(gprof[i]);
        P.outWords[4] = 0; P.outWords[5] = 0;
        P.outWords[6] = uint64_t(groups);
    }
    // Ранг проверен хостом заранее; если что — попытка просто пустая.
    if (found < k)
        return;

    const int p = P.rowsPerTrial;
    auto rowAt = [&](int r) { return m + size_t(r) * STRIDE; };

    // 4а. Окно Штерна–Дюмера (см. Leon::trialStern): окно — последние
    //     window неопорных столбцов порядка, ключ строки — её биты на окне,
    //     половины — опоры [0, k/2) и [k/2, k) по порядку их появления
    //     (rowOf), как строки приведённой матрицы на хосте: попытка с тем
    //     же номером даёт те же пары, что и trialStern.
    //     Комбинации до p опор первой половины — список в рабочем буфере
    //     блока: запись (ключ, номера опор + 1 по 13 бит) под номером
    //     комбинации, цепочки по хешу ключа (head/next; открытая адресация
    //     не годится: у структурных кодов ключи повторяются сотнями, и
    //     сплошные серии ячеек ходят насквозь). Комбинации второй половины
    //     идут по цепочке и кладут найденные пары в общий список пар, а
    //     потом пары суммируются всеми нитями поровну: у пары до четырёх
    //     строк, а у одного ключа пар может быть сотни — считать их лэйном,
    //     нашедшим цепочку, значит держать варп в одной нити. Список пар
    //     полон — пара считается на месте. Пара «пусто, пусто» — нулевое
    //     слово.
    if (P.window > 0) {
        __shared__ int      s_win[32];
        __shared__ int      s_lw;
        __shared__ unsigned s_pairs;
        if (tid == 0) {
            int cnt = 0;
            for (int i = n - 1; i >= 0 && cnt < P.window; --i)
                if (!colPivot[order[i]]) s_win[cnt++] = order[i];
            s_lw    = cnt;
            s_pairs = 0;
        }
        __syncthreads();
        const int lw = s_lw;
        for (int i = tid; i < k; i += B) {
            const uint64_t* row = rowAt(rowOf[i]);
            uint32_t x = 0;
            for (int b = 0; b < lw; ++b)
                x |= uint32_t(bitAt(row, s_win[b])) << b;
            winKey[i] = x;
        }

        // Рабочий буфер блока: головы цепочек, записи, ссылки, список пар.
        int*      const head  = reinterpret_cast<int*>(scratch + (GLOBAL ? size_t(k) * STRIDE : 0));
        uint64_t* const entry = reinterpret_cast<uint64_t*>(head + P.hashSlots);
        int*      const next  = reinterpret_cast<int*>(entry + P.listA);
        uint64_t* const pairs = reinterpret_cast<uint64_t*>(next + ((P.listA + 1) & ~1u));
        for (uint64_t i = tid; i < P.hashSlots; i += B)
            head[i] = -1;
        __syncthreads();

        const int shift = P.hashShift;
        const int h1    = k / 2;
        auto chainOf = [&](uint32_t kk) { return int((kk * 0x9E3779B1u) >> shift); };
        auto put = [&](unsigned idx, uint32_t kk, int a, int b) {
            entry[idx] = (uint64_t(kk) << 26) | (uint64_t(a + 1) << 13) | uint64_t(b + 1);
            next[idx]  = atomicExch(head + chainOf(kk), int(idx));
        };
        // Номер комбинации: 0 — пусто, 1..h1 — одиночные, дальше пары по a.
        if (tid == 0)
            put(0u, 0u, -1, -1);
        for (int a = tid; a < h1; a += B)
            put(unsigned(1 + a), winKey[a], a, -1);
        if (p >= 2)
            for (int a = warp; a < h1; a += warps) {
                const unsigned base = unsigned(1 + h1) + unsigned(a) * unsigned(h1 - 1) - unsigned(a) * unsigned(a - 1) / 2u;
                for (int b = a + 1 + lane; b < h1; b += 32)
                    put(base + unsigned(b - a - 1), winKey[a] ^ winKey[b], a, b);
            }
        __syncthreads();

        auto sumPair = [&](uint64_t e, int b0, int b1) {
            const int a0 = int((e >> 13) & 0x1FFFu) - 1, a1 = int(e & 0x1FFFu) - 1;
            uint64_t acc[WORDS];
            #pragma unroll
            for (int w = 0; w < WORDS; ++w) acc[w] = 0ULL;
            const int rs[4] = { a0, a1, b0, b1 };
            for (int i = 0; i < 4; ++i) {
                if (rs[i] < 0) continue;
                const uint64_t* row = rowAt(rowOf[rs[i]]);
                #pragma unroll
                for (int w = 0; w < WORDS; ++w) acc[w] ^= row[w];
            }
            int wt = 0;
            #pragma unroll
            for (int w = 0; w < WORDS; ++w) wt += __popcll(acc[w]);
            if (light(wt, P))
                emitWord<WORDS>(P, acc, nullptr, wt);
        };
        auto probe = [&](uint32_t kk, int b0, int b1) {
            for (int i = __ldcg(head + chainOf(kk)); i >= 0; i = __ldcg(next + i)) {
                const uint64_t e = __ldcg(reinterpret_cast<const unsigned long long*>(entry + i));
                if (uint32_t((e >> 26) & 0xFFFFFu) != kk)
                    continue;
                if ((e & 0x3FFFFFFu) == 0ULL && b0 < 0)
                    continue;   // «пусто, пусто»
                const unsigned slot = atomicAdd(&s_pairs, 1u);
                if (slot < P.pairCapacity)
                    pairs[slot] = (uint64_t(i) << 26) | (uint64_t(b0 + 1) << 13) | uint64_t(b1 + 1);
                else
                    sumPair(e, b0, b1);
            }
        };
        if (tid == 0)
            probe(0u, -1, -1);
        for (int b = h1 + tid; b < k; b += B)
            probe(winKey[b], b, -1);
        if (p >= 2)
            for (int a = h1 + warp; a < k; a += warps)
                for (int b = a + 1 + lane; b < k; b += 32)
                    probe(winKey[a] ^ winKey[b], a, b);
        __syncthreads();

        const unsigned total = min(s_pairs, P.pairCapacity);
        for (unsigned i = tid; i < total; i += B) {
            const uint64_t rec = pairs[i];
            sumPair(entry[rec >> 26], int((rec >> 13) & 0x1FFFu) - 1, int(rec & 0x1FFFu) - 1);
        }
        return;
    }

    // 4. Перебор комбинаций до p строк. Единица работы — варп, а не нить:
    //    у 32 лэйнов либо общий префикс в регистрах и свои хвосты подряд
    //    (строки читаются соседние — без конфликтов банков при нечётном
    //    шаге), либо свои префиксы и общий хвост (строка читается
    //    широковещательно). В обоих случаях лэйны делают одинаковое число
    //    шагов: раньше у каждой нити был свой хвост своей длины, и в варпе
    //    работало в среднем 8 лэйнов из 32.
    uint64_t acc[WORDS], acc2[WORDS];

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

size_t sharedBytesFor(int rows, int cols, int words, bool global, int tableGroup, int window)
{
    size_t bytes = size_t(cols) * sizeof(uint16_t) + size_t(rows) * sizeof(uint16_t)   // порядок, опоры по порядку
                 + 2 * size_t(rows) + size_t(cols);                                     // флаги, ключи группы, столбцы
    if (window > 0)
        bytes += size_t((rows + 1) & ~1) * sizeof(uint32_t);                            // ключи окна
    if (tableGroup > 0)
        bytes += (size_t(1) << tableGroup) * rowStride(words) * sizeof(uint64_t);       // таблица четырёх русских
    if (!global)
        bytes += size_t(rows) * rowStride(words) * sizeof(uint64_t);
    return bytes;
}

// Как вести Гаусс: ширина группы, таблица, поиск опор одним варпом.
struct GaussMode
{
    int  group   = 1;
    bool table   = false;
    int  rowsPerLane = 8;
};

// Пределы разделяемой памяти устройства: обычный на блок и по опт-ину
// (на Ampere — 99 КБ против 48).
struct SharedLimits
{
    size_t perBlock = 48 * 1024;
    size_t optIn    = 48 * 1024;
};
SharedLimits sharedLimits()
{
    static SharedLimits limits;
    static bool known = false;
    if (!known) {
        known = true;
        int device = 0, value = 0;
        cudaGetDevice(&device);
        if (cudaDeviceGetAttribute(&value, cudaDevAttrMaxSharedMemoryPerBlock, device) == cudaSuccess && value > 0)
            limits.perBlock = size_t(value);
        if (cudaDeviceGetAttribute(&value, cudaDevAttrMaxSharedMemoryPerBlockOptin, device) == cudaSuccess && value > 0)
            limits.optIn = size_t(value);
    }
    return limits;
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
        return 0;
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
        const size_t bytes = sharedBytesFor(rows, cols, words, global, group, 1)
                           + staticSharedBytes(words) + size_t(reservedPerBlock);
        return int(size_t(sharedPerSm) / bytes);
    };
    int best = 0;
    for (int group = 1; group <= 8; ++group) {
        if (sharedBytesFor(rows, cols, words, global, group, 1) > size_t(Constants::MAX_SHARED_BYTES))
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

int leonSharedTier(int rows, int cols, int wordsPerRow, int window)
{
    const int words = leonPaddedWords(wordsPerRow);
    if (rows <= 0 || cols <= 0 || words == 0)
        return 0;
    const size_t bytes = sharedBytesFor(rows, cols, words, false, 0, window);
    if (bytes <= size_t(Constants::MAX_SHARED_BYTES))
        return 1;
    // Большая: по опт-ину, один блок на мультипроцессор. Статическая
    // память ядра — в тот же предел.
    const SharedLimits limits = sharedLimits();
    if (limits.optIn > limits.perBlock && bytes + staticSharedBytes(words) <= limits.optIn)
        return 2;
    return 0;
}

bool leonFitsShared(int rows, int cols, int wordsPerRow)
{
    return leonSharedTier(rows, cols, wordsPerRow, 0) > 0;
}

size_t leonSharedBytes(int rows, int cols, int wordsPerRow)
{
    const int words = leonPaddedWords(wordsPerRow);
    if (rows <= 0 || cols <= 0 || words == 0)
        return 0;
    const int  tier   = leonSharedTier(rows, cols, wordsPerRow, 0);
    const bool global = tier == 0;
    const int  group  = gaussGroupFor(rows, cols, words, global, LEON_THREADS);
    if (global && group == 0)
        return 0;
    return sharedBytesFor(rows, cols, words, global, global ? group : 0, 0);
}

// Гаусс по ярусу: в глобальной памяти — четыре русских с таблицей (проходы
// по строкам — латентность DRAM, таблица их сокращает); в разделяемой —
// без таблицы: в обычной разделяемой памяти (ярус 1) по столбцу — там
// сложение дёшево, а барьеров при 2–4 блоках на мультипроцессор хватает
// кому перекрыть; в большой (ярус 2) блок на мультипроцессоре один, ядро
// стоит у барьеров, и группа по восемь столбцов с четырьмя барьерами
// вместо шестнадцати выигрывает; опоры там ищет один варп.
GaussMode gaussModeFor(int rows, int cols, int words, int tier, int threadsPerBlock)
{
    GaussMode mode;
    if (tier == 0) {
        mode.group = gaussGroupFor(rows, cols, words, true, threadsPerBlock);
        mode.table = mode.group > 1;
    }
    if (const char* forced = getenv("LEON_GAUSS")) {   // EXPERIMENT: col | group | table
        const std::string f = forced;
        if (f == "col") { mode.group = 1; mode.table = false; }
        if (f == "group") { mode.group = 8; mode.table = false; mode.rowsPerLane = 32; }
        if (f == "table") { mode.group = tier == 0 ? mode.group : 6; mode.table = true; mode.rowsPerLane = 32; }
    }
    return mode;
}

size_t leonScratchWords(int rows, int cols, int wordsPerRow, int rowsPerTrial, int window,
                        unsigned pairCapacity, uint64_t* hashSlots, unsigned* listA)
{
    const int    words  = leonPaddedWords(wordsPerRow);
    const bool   global = leonSharedTier(rows, cols, wordsPerRow, window) == 0;
    size_t       total  = global ? size_t(rows) * size_t(rowStride(words)) : 0;
    uint64_t     slots  = 0;
    unsigned     list   = 0;
    if (window > 0) {
        const unsigned h1 = unsigned(rows / 2);
        list  = 1u + h1 + (rowsPerTrial >= 2 ? h1 * (h1 - 1) / 2u : 0u);
        slots = 2;
        while (double(slots) < 2.0 * list) slots <<= 1;
        total += size_t(slots) / 2                 // головы цепочек, int32
                + size_t(list)                     // записи
                + (size_t(list) + 1) / 2           // ссылки, int32
                + size_t(pairCapacity);            // список пар
    }
    if (hashSlots) *hashSlots = slots;
    if (listA) *listA = list;
    return total;
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
    const int  tier   = leonSharedTier(launch.rows, launch.cols, launch.wordsPerRow, launch.window);
    const bool global = tier == 0;
    if (words == 0)
        throw std::invalid_argument("строка матрицы длиннее, чем умеет ядро случайного поиска");
    if ((global || launch.window > 0) && launch.scratch == nullptr)
        throw std::invalid_argument("ядру случайного поиска нужен рабочий буфер, а он не задан");
    if (launch.window > 0 && (launch.window > 20 || launch.rows > 0x1FFF || launch.pairCapacity == 0))
        throw std::invalid_argument("окно Штерна–Дюмера на видеокарте: окно до 20 столбцов, строк до 8191, список пар не задан");
    if (launch.trials <= 0)
        return;

    const GaussMode mode = gaussModeFor(launch.rows, launch.cols, words, tier, threadsPerBlock);
    if (mode.group == 0)
        throw std::invalid_argument("порядок столбцов не помещается в разделяемую память");
    const size_t sharedBytes = sharedBytesFor(launch.rows, launch.cols, words, global,
                                              mode.table ? mode.group : 0, launch.window);
    LeonLaunch L = launch;
    L.gaussGroup        = mode.group;
    L.gaussTable        = mode.table;
    L.searchRowsPerLane = mode.rowsPerLane;
    L.scratchWords = leonScratchWords(L.rows, L.cols, L.wordsPerRow, L.rowsPerTrial, L.window,
                                      L.pairCapacity, &L.hashSlots, &L.listA);
    L.hashShift = 32;
    for (uint64_t v = L.hashSlots; v > 1; v >>= 1) --L.hashShift;

    // Большая разделяемая память — по опт-ину: ядру разрешается больше
    // обычных 48 КБ (один раз на вариант; статическая память ядра — в тот
    // же предел).
    #define LAUNCH_LEON(W, G)                                                                       \
        do {                                                                                        \
            if (tier == 2) {                                                                        \
                static bool optedIn = false;                                                        \
                if (!optedIn) {                                                                     \
                    optedIn = true;                                                                 \
                    cudaFuncAttributes fa{};                                                        \
                    cudaFuncGetAttributes(&fa, leonTrialsKernel<W, G>);                             \
                    cudaFuncSetAttribute(leonTrialsKernel<W, G>,                                    \
                                         cudaFuncAttributeMaxDynamicSharedMemorySize,               \
                                         int(sharedLimits().optIn - fa.sharedSizeBytes));           \
                    cudaFuncSetAttribute(leonTrialsKernel<W, G>,                                    \
                                         cudaFuncAttributePreferredSharedMemoryCarveout, 100);      \
                }                                                                                   \
            }                                                                                       \
            leonTrialsKernel<W, G><<<L.trials, threadsPerBlock, sharedBytes, stream>>>(L);          \
        } while (0)
    #define LAUNCH_LEON_WORDS(G)                                                                    \
        switch (words) {                                                                            \
            case  1: LAUNCH_LEON( 1, G); break;   case  2: LAUNCH_LEON( 2, G); break;               \
            case  3: LAUNCH_LEON( 3, G); break;   case  4: LAUNCH_LEON( 4, G); break;               \
            case  5: LAUNCH_LEON( 5, G); break;   case  6: LAUNCH_LEON( 6, G); break;               \
            case  7: LAUNCH_LEON( 7, G); break;   case  8: LAUNCH_LEON( 8, G); break;               \
            case 10: LAUNCH_LEON(10, G); break;   case 12: LAUNCH_LEON(12, G); break;               \
            case 14: LAUNCH_LEON(14, G); break;   case 16: LAUNCH_LEON(16, G); break;               \
            case 20: LAUNCH_LEON(20, G); break;   case 24: LAUNCH_LEON(24, G); break;               \
            case 28: LAUNCH_LEON(28, G); break;   default: LAUNCH_LEON(32, G); break;               \
        }
    if (!global)
        LAUNCH_LEON_WORDS(false)
    else
        LAUNCH_LEON_WORDS(true)
    #undef LAUNCH_LEON_WORDS
    #undef LAUNCH_LEON

    const cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess)
        throw std::invalid_argument(
            std::string("не удалось запустить ядро случайного поиска: ") + cudaGetErrorString(err)
            + " (блоков " + std::to_string(launch.trials)
            + ", нитей "  + std::to_string(threadsPerBlock)
            + ", разделяемой памяти " + std::to_string(sharedBytes) + " байт)");
}
