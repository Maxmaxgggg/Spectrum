#pragma once

// Брауэр–Циммерман у циклического кода по сдвигам (подход Чена) — правило
// подсчёта, общее для хоста и ядер.
//
// У циклического кода k подряд идущих столбцов круга — информационное
// множество, и все его сдвиги — тоже. Перебор до r строк на одном таком
// множестве S₀ находит слова, у которых на S₀ не больше r единиц. Слово, не
// найденное ни на одном сдвиге S₀, несёт не меньше r + 1 единиц в каждом окне
// из k столбцов круга, а каждый столбец лежит ровно в k окнах из L, — значит,
// единиц на круге у него не меньше L(r + 1)/k. Поэтому, перебрав одно S₀,
// мы знаем точно все орбиты слов веса меньше ⌈L(r + 1)/k⌉ — если каждую
// засчитать один раз и с её размером.
//
// Какое слово орбиты засчитывается. Сдвиги S₀ на t, где у слова не больше r
// единиц, — множество T(c), и у сдвинутого слова оно сдвигается тоже:
// T(σᵗc) = T(c) + t. Представитель орбиты c* — наименьший из сдвигов,
// ставящих единицу на начало круга (как в cyclic.h), t₀ — наименьший номер
// в T(c*). Засчитывается слово m = σ^(−t₀) c*: у него ноль лежит в T(m),
// значит, перебор S₀ его находит, и оно у орбиты одно. К спектру добавляется
// размер орбиты. Счёт остаётся целым в любой момент расчёта, поэтому
// чекпоинты и продолжение работают как обычно.
//
// Заголовок не тянет ни Qt, ни CUDA: его подключают и .cu, и обычные .cpp.

#include "bitops.h"

// Функция для обеих сторон, но на устройстве — не встраиваемая: она длинная
// и вызывается только для лёгких слов, а встроенная раздула бы горячий цикл
// перебора и отняла бы у него регистры.
#ifdef __CUDACC__
    #define SPECTRUM_HD_RARE __host__ __device__ __noinline__
#else
    #define SPECTRUM_HD_RARE inline
#endif

namespace CyclicOrbit {

// Круг до 2048 столбцов — 32 слова; MAXW у вызывающего не меньше.
SPECTRUM_HD int bitAt(const unsigned long long* x, int i)
{
    return int((x[i >> 6] >> (i & 63)) & 1ULL);
}

// x — биты круга: слово, сдвинутое на start вправо, младшие length бит.
SPECTRUM_HD void extract(const unsigned long long* word, int words, int start, int length,
                         unsigned long long* x)
{
    const int lw = (length + 63) >> 6;
    for (int i = 0; i < lw; ++i) {
        const int bit = start + (i << 6);
        const int wi = bit >> 6, sh = bit & 63;
        unsigned long long v = wi < words ? word[wi] >> sh : 0ULL;
        if (sh != 0 && wi + 1 < words)
            v |= word[wi + 1] << (64 - sh);
        x[i] = v;
    }
    if (length & 63)
        x[lw - 1] &= (1ULL << (length & 63)) - 1ULL;
}

// y — x, повёрнутый по кругу длины length на shift (0 <= shift < length):
// бит j переходит в (j + shift) mod length.
SPECTRUM_HD void rotate(const unsigned long long* x, int length, int shift, unsigned long long* y)
{
    const int lw = (length + 63) >> 6;
    if (shift == 0) {
        for (int i = 0; i < lw; ++i) y[i] = x[i];
        return;
    }
    // (x << shift) | (x >> (length − shift)), обрезанное до length бит. Что
    // ушло влево за lw слов, лежало выше length и всё равно отрезается.
    const int up = shift, down = length - shift;
    for (int i = 0; i < lw; ++i) {
        const int ju = i - (up >> 6), pu = up & 63;
        unsigned long long a = ju >= 0 ? x[ju] << pu : 0ULL;
        if (pu != 0 && ju - 1 >= 0) a |= x[ju - 1] >> (64 - pu);
        const int jd = i + (down >> 6), pd = down & 63;
        unsigned long long b = jd < lw ? x[jd] >> pd : 0ULL;
        if (pd != 0 && jd + 1 < lw) b |= x[jd + 1] << (64 - pd);
        y[i] = a | b;
    }
    if (length & 63)
        y[lw - 1] &= (1ULL << (length & 63)) - 1ULL;
}

SPECTRUM_HD bool equal(const unsigned long long* x, const unsigned long long* y, int lw)
{
    for (int i = 0; i < lw; ++i)
        if (x[i] != y[i]) return false;
    return true;
}

SPECTRUM_HD bool less(const unsigned long long* x, const unsigned long long* y, int lw)
{
    for (int i = 0; i < lw; ++i)
        if (x[i] != y[i]) return x[i] < y[i];
    return false;
}

// Размер орбиты, если слово засчитывается (см. в начале), иначе ноль.
// Круг — столбцы [start, start + length) слова из words 64-битных слов, S₀ —
// первые window его столбцов, depth — до скольких строк шёл перебор S₀.
// MAXW — слов под круг, не меньше (length + 63) / 64.
template <int MAXW>
SPECTRUM_HD_RARE int designatedOrbit(const unsigned long long* word, int words, int start, int length,
                                     int window, int depth)
{
    const int lw = (length + 63) >> 6;
    unsigned long long x[MAXW], best[MAXW], cand[MAXW];
    extract(word, words, start, length, x);

    // Сдвиг a до представителя: наименьший из сдвигов, ставящих единицу на
    // начало круга. Без единиц на круге слово неподвижно, a = 0.
    int  a     = 0;
    bool found = false;
    for (int i = 0; i < lw; ++i) {
        for (unsigned long long rest = x[i]; rest; rest &= rest - 1ULL) {
            const int j = (i << 6) + BitOps::lowestSetBit(rest);
            const int s = j == 0 ? 0 : length - j;
            rotate(x, length, s, cand);
            if (!found || less(cand, best, lw)) {
                for (int q = 0; q < lw; ++q) best[q] = cand[q];
                a     = s;
                found = true;
            }
        }
    }

    // T(c*) = T(c) + a. Окна идут скользя: окно t — столбцы [t, t + window)
    // по кругу; t₀ — наименьший (t + a) mod length среди окон с не больше
    // depth единицами.
    int count = 0;
    for (int t = 0; t < window; ++t)
        count += bitAt(x, t);
    int t0 = length;
    for (int t = 0; t < length; ++t) {
        if (count <= depth) {
            int u = t + a;
            if (u >= length) u -= length;
            if (u < t0) t0 = u;
        }
        int e = t + window;
        if (e >= length) e -= length;
        count += bitAt(x, e) - bitAt(x, t);
    }
    if (t0 == length)
        return 0;   // ни одного окна — перебор S₀ это слово не находил

    // Засчитывается m = σ^(a − t₀) c; само слово c — если поворот на a − t₀
    // оставляет его на месте.
    int delta = a - t0;
    if (delta < 0) delta += length;
    if (delta != 0) {
        rotate(x, length, delta, cand);
        if (!equal(cand, x, lw))
            return 0;
    }

    // Размер орбиты — наименьший период: делитель length, поворот на который
    // слово не меняет.
    for (int d = 1; d < length; ++d) {
        if (length % d != 0)
            continue;
        rotate(x, length, d, cand);
        if (equal(cand, x, lw))
            return d;
    }
    return length;
}

} // namespace CyclicOrbit
