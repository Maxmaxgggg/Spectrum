#pragma once

// Варианты ядер по числу 64-битных слов в строке матрицы.
//
// Ядра шаблонные по этому числу: при известном на этапе компиляции размере
// кодовое слово живёт в регистрах, а не в локальной памяти. Заготовлены не
// все размеры, а ряд ниже; число слов округляется вверх до ближайшего, а
// лишние слова строки — нули, с которыми XOR и popcount ничего не меняют.
//
// Раньше ряд был записан дважды (pickWordCount и leonPaddedWords), а выбор
// варианта — четырьмя одинаковыми макросами со switch на шестнадцать
// случаев. Теперь ряд один, и выбор тоже.

#include <type_traits>

// Ближайший сверху заготовленный размер; ноль — строка длиннее любого.
inline int paddedWordCount(int words)
{
    static const int SIZES[] = { 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 28, 32 };
    for (int candidate : SIZES)
        if (candidate >= words) return candidate;
    return 0;
}

// Зовёт launch(std::integral_constant<int, W>()) для заготовленного размера
// padded (результат paddedWordCount). false — такого варианта нет, и launch
// не вызывался: что делать тогда, решает вызывающий.
//
//     dispatchWords(words, [&](auto w) {
//         constexpr int W = decltype(w)::value;
//         kernel<W><<<blocks, threads, shared, stream>>>(...);
//     });
template <class Launch>
bool dispatchWords(int padded, Launch&& launch)
{
    switch (padded) {
        case  1: launch(std::integral_constant<int,  1>()); return true;
        case  2: launch(std::integral_constant<int,  2>()); return true;
        case  3: launch(std::integral_constant<int,  3>()); return true;
        case  4: launch(std::integral_constant<int,  4>()); return true;
        case  5: launch(std::integral_constant<int,  5>()); return true;
        case  6: launch(std::integral_constant<int,  6>()); return true;
        case  7: launch(std::integral_constant<int,  7>()); return true;
        case  8: launch(std::integral_constant<int,  8>()); return true;
        case 10: launch(std::integral_constant<int, 10>()); return true;
        case 12: launch(std::integral_constant<int, 12>()); return true;
        case 14: launch(std::integral_constant<int, 14>()); return true;
        case 16: launch(std::integral_constant<int, 16>()); return true;
        case 20: launch(std::integral_constant<int, 20>()); return true;
        case 24: launch(std::integral_constant<int, 24>()); return true;
        case 28: launch(std::integral_constant<int, 28>()); return true;
        case 32: launch(std::integral_constant<int, 32>()); return true;
        default: return false;
    }
}
