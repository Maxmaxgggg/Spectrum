#include "hammingcode.h"

namespace Hamming {

namespace {

Code make(int r, bool extend, int shorten, bool withRows)
{
    Code code;
    if (r < MIN_R || r > MAX_R)
        return code;
    const int n = (1 << r) - 1;
    const int k = n - r;
    if (shorten < 0 || shorten >= k)
        return code;
    code.n = n - shorten + (extend ? 1 : 0);
    code.k = k - shorten;
    code.d = extend ? 4 : 3;
    if (!withRows)
        return code;

    // Строка i — единичный вектор и i-й по счёту r-разрядный вектор веса
    // не меньше двух (в двоичном порядке): столбец проверочной матрицы.
    int value = 0;
    for (int i = 0; i < k; ++i) {
        do { ++value; } while ((value & (value - 1)) == 0);   // пропуск степеней двойки
        if (i < shorten)
            continue;
        QString row(code.n, QLatin1Char('0'));
        row[i - shorten] = QLatin1Char('1');
        int ones = 1;
        for (int b = 0; b < r; ++b)
            if (value >> b & 1) { row[code.k + b] = QLatin1Char('1'); ++ones; }
        if (extend && (ones & 1))
            row[code.n - 1] = QLatin1Char('1');
        code.rows.append(row);
    }
    return code;
}

} // namespace

Code build(int r, bool extend, int shorten)    { return make(r, extend, shorten, true); }
Code describe(int r, bool extend, int shorten) { return make(r, extend, shorten, false); }

} // namespace Hamming
