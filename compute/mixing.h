#pragma once

// Перемешивание: генераторы, тасование и хеш слова — общие для хоста и ядер.
//
// Здесь важна побитовая одинаковость, а не только смысл. Попытка случайного
// поиска с одним номером обязана давать один и тот же порядок столбцов на CPU
// и на GPU — так ядро и проверяется, сличением с процессором. Раньше это
// держалось на двух копиях одних и тех же констант, в leonsearch.cpp и в
// leonkernel.cu, и на комментарии «обязан совпадать бит в бит».

#include "bitops.h"

namespace Mix {

// Перемешивание номера в затравку (splitmix64): соседние номера дают
// несвязанные последовательности.
SPECTRUM_HD unsigned long long splitmix64(unsigned long long index)
{
    unsigned long long z = index + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// Свой генератор, а не std::mt19937: последовательность обязана быть одной
// и той же на любой сборке, а на видеокарте стандартной библиотеки нет.
// Нулевое состояние генератор не покидает — затравку берут нечётной.
struct Xorshift64
{
    unsigned long long state;

    SPECTRUM_HD unsigned long long next()
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
};

// Тасование Фишера–Йетса для попытки с этим номером: порядок столбцов
// случайного поиска. Индекс берётся старшими битами произведения, без
// деления (см. Bits::mulHigh64).
template <class T>
SPECTRUM_HD void shuffleForTrial(T* order, int count, unsigned long long trialIndex)
{
    Xorshift64 rng{ splitmix64(trialIndex) | 1ULL };
    for (int c = count - 1; c > 0; --c) {
        const int j = int(Bits::mulHigh64(rng.next(), static_cast<unsigned long long>(c + 1)));
        const T t = order[c];
        order[c] = order[j];
        order[j] = t;
    }
}

// Хеш слова по 64-битным частям: начальное значение и шаг на каждую часть.
SPECTRUM_HD unsigned long long wordHashSeed()
{
    return 0x9E3779B97F4A7C15ULL;
}

SPECTRUM_HD unsigned long long wordHashStep(unsigned long long h, unsigned long long part)
{
    h ^= part;
    h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33;
    return h;
}

} // namespace Mix
