#pragma once

// Двоичный код Хэмминга с r проверочными символами: [2^r − 1, 2^r − 1 − r, 3],
// циклический, в систематическом виде.
//
// Порождающий многочлен — примитивный степени r, обратный к табличному
// (для r = 3 это x³ + x² + 1, и матрица (7,4) получается та, что в
// учебниках: 1000101, 0100111, 0010110, 0001011); строки — как у БЧХ:
// (E | x^{n−k+i} mod g). Расширение приписывает бит чётности (n + 1, d = 4),
// укорочение на s убирает первые s информационных символов.

#include <QString>
#include <QStringList>

namespace Hamming {

constexpr int MIN_R = 2;
constexpr int MAX_R = 11;   // n = 2047: длиннее матрицу программа не берёт

struct Code
{
    int         n = 0;
    int         k = 0;
    int         d = 0;
    QStringList rows;
};

// Пустой Code (n == 0), если r вне диапазона или shorten >= k.
Code build(int r, bool extend, int shorten);
Code describe(int r, bool extend, int shorten);

} // namespace Hamming
