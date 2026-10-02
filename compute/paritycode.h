#pragma once

// Код с проверкой на чётность [k + 1, k, 2]: (E_k | столбец единиц).

#include <QString>
#include <QStringList>

namespace Parity {

constexpr int MIN_K = 1;
constexpr int MAX_K = 2047;   // n = 2048 — потолок ширины матрицы в программе

inline QStringList build(int k)
{
    QStringList rows;
    if (k < MIN_K || k > MAX_K)
        return rows;
    for (int i = 0; i < k; ++i) {
        QString row(k + 1, QLatin1Char('0'));
        row[i] = QLatin1Char('1');
        row[k] = QLatin1Char('1');
        rows.append(row);
    }
    return rows;
}

} // namespace Parity
