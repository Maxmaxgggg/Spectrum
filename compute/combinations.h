#pragma once

// Сочетания: номер ↔ сочетание и шаги по ним — общие для хоста и ядер.
//
// Нумерация везде одна — лексикографическая по возрастанию позиций, — и на
// ней держится смысл chunkOffset в чекпоинтах. Раньше перевод номера в
// сочетание был написан трижды (unrankCombination и generateStartPositions
// на хосте, generateBitMaskGPU в ядре), а шаг к следующему сочетанию и
// разность позиций — дважды, на хосте и в ядре.
//
// C — биномиальные коэффициенты: C(n, m) для нужных n и m, в том числе
// C(n, 0) = 1. На хосте это BinomTable, в ядре — таблица в памяти устройства.

#include "bitops.h"

#include <cstdint>

namespace Combinations {

// Позиция очередной единицы при разборе номера: наименьшая j >= from, такая,
// что номер попадает в сочетания с единицей в j. left — сколько единиц ещё
// не расставлено (вместе с этой). Номер уменьшается на пропущенные сочетания.
template <class Binom>
SPECTRUM_HD unsigned nextRankedPosition(unsigned n, unsigned left, unsigned from,
                                        unsigned long long& rank, const Binom& C)
{
    unsigned j = from;
    while (j <= n - left) {
        const unsigned long long c = C(n - j - 1, left - 1);
        if (c > rank)
            break;
        rank -= c;
        ++j;
    }
    return j;
}

// Маска из r единиц среди n <= 64 позиций по её номеру.
template <class Binom>
SPECTRUM_HD unsigned long long unrankMask(unsigned n, unsigned r, unsigned long long rank, const Binom& C)
{
    if (r == 0 || r > n)
        return 0ULL;
    unsigned long long mask = 0ULL;
    unsigned from = 0;
    for (unsigned left = r; left > 0; --left) {
        const unsigned j = nextRankedPosition(n, left, from, rank, C);
        mask |= 1ULL << j;
        from = j + 1;
    }
    return mask;
}

// То же для длинных кодов: позиции единиц по возрастанию в positions[0..r),
// хвост до capacity обнуляется.
template <class Binom>
inline void unrankPositions(unsigned long long rank, int n, int r, int16_t* positions, int capacity,
                            const Binom& C)
{
    unsigned from = 0;
    for (int i = 0; i < r; ++i) {
        const unsigned j = nextRankedPosition(unsigned(n), unsigned(r - i), from, rank, C);
        positions[i] = int16_t(j);
        from = j + 1;
    }
    for (int i = r; i < capacity; ++i)
        positions[i] = 0;
}

// Следующее сочетание: a[0] < … < a[r−1] — позиции единиц среди n.
// false — сочетание было последним.
SPECTRUM_HD bool nextPositions(int16_t* a, int r, int n)
{
    int i = r - 1;
    // Самый правый элемент, который ещё можно увеличить
    while (i >= 0 && a[i] == n - r + i)
        --i;
    if (i < 0)
        return false;
    ++a[i];
    for (int j = i + 1; j < r; ++j)
        a[j] = int16_t(a[i] + (j - i));
    return true;
}

// Симметрическая разность двух сочетаний по r позиций: строки, которые
// вошли или вышли, — их и надо сложить с кодовым словом. Оба массива
// отсортированы; changed — до 2r позиций, их число — в numChanged.
SPECTRUM_HD void diffPositions(const int16_t* prev, const int16_t* curr, int r,
                               int16_t* changed, int& numChanged)
{
    int i = 0, j = 0;
    numChanged = 0;
    while (i < r || j < r) {
        if (j == r || (i < r && prev[i] < curr[j]))
            changed[numChanged++] = prev[i++];
        else if (i == r || curr[j] < prev[i])
            changed[numChanged++] = curr[j++];
        else {
            ++i;
            ++j;
        }
    }
}

} // namespace Combinations
