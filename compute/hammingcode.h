#pragma once

// Двоичный код Хэмминга с r проверочными символами: [2^r − 1, 2^r − 1 − r, 3].
//
// Проверочная матрица H = (Pᵀ | E_r), где столбцы Pᵀ — все ненулевые
// r-разрядные векторы веса не меньше двух, по возрастанию как двоичных
// чисел; порождающая — G = (E_k | P). Расширение приписывает бит чётности
// (n + 1, d = 4), укорочение на s убирает первые s информационных символов.

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
