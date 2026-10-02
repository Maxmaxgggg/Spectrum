#pragma once

// Эталонные данные для тестов расчёта спектра.
//
// Здесь намеренно НЕТ ничего из compute/: наивный перебор написан отдельно и
// максимально прямолинейно, чтобы его можно было проверить глазами. Спектры
// классических кодов дополнительно сверены с аналитическими весовыми
// перечислителями (см. tests/README.md).

#include <QMap>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace Reference {

using Spectrum = QMap<int, quint64>;   // вес -> количество, только ненулевые

// Наивный перебор всех 2^k комбинаций строк. Годится только для k <= ~24.
inline Spectrum bruteForce(const QStringList& rows)
{
    const int k = rows.size();
    const int n = rows.first().length();
    Q_ASSERT(k <= 24);

    const int wordsPerRow = (n + 63) / 64;
    QVector<quint64> words(k * wordsPerRow, 0ULL);
    for (int r = 0; r < k; ++r)
        for (int c = 0; c < n; ++c)
            if (rows[r].at(c) == QLatin1Char('1'))
                words[r * wordsPerRow + c / 64] |= (1ULL << (c % 64));

    Spectrum spec;
    QVector<quint64> cw(wordsPerRow);
    for (quint64 mask = 0; mask < (1ULL << k); ++mask) {
        cw.fill(0ULL);
        for (int r = 0; r < k; ++r)
            if (mask & (1ULL << r))
                for (int w = 0; w < wordsPerRow; ++w)
                    cw[w] ^= words[r * wordsPerRow + w];

        int weight = 0;
        for (int w = 0; w < wordsPerRow; ++w) {
            quint64 v = cw[w];
            while (v) { weight += int(v & 1ULL); v >>= 1; }
        }
        spec[weight] += 1;
    }
    return spec;
}

// Биномиальный коэффициент C(n, r) без переполнения для наших размеров.
inline quint64 binom(quint64 n, quint64 r)
{
    if (r > n) return 0;
    if (r > n - r) r = n - r;
    quint64 result = 1;
    for (quint64 i = 1; i <= r; ++i) {
        result = result / i * (n - r + i) + result % i * (n - r + i) / i;
    }
    return result;
}

// --- Порождающие матрицы ---

// Хэмминг (7,4): весовой перечислитель 1 + 7z^3 + 7z^4 + z^7
inline QStringList hamming7_4()
{
    return { "1000110", "0100101", "0010011", "0001111" };
}

// Расширенный Хэмминг (8,4): 1 + 14z^4 + z^8
inline QStringList extHamming8_4()
{
    return { "10001110", "01001101", "00101011", "00010111" };
}

// Голей (24,12): 1 + 759z^8 + 2576z^12 + 759z^16 + z^24
inline QStringList golay24_12()
{
    static const char* B[12] = {
        "011111111111", "111011100010", "110111000101", "101110001011",
        "111100010110", "111000101101", "110001011011", "100010110111",
        "100101101110", "101011011100", "110110111000", "101101110001",
    };
    QStringList rows;
    for (int i = 0; i < 12; ++i) {
        QString r(12, QLatin1Char('0'));
        r[i] = QLatin1Char('1');
        rows << r + QLatin1String(B[i]);
    }
    return rows;
}

// Единичная матрица I(n): порождает вообще все слова, спектр равен C(n, w).
// При частичном переборе с maxRows = m спектр равен C(n, w) для w <= m и 0 дальше,
// что даёт точный эталон для длинных кодов, где полный перебор невозможен.
inline QStringList identity(int n)
{
    QStringList rows;
    for (int i = 0; i < n; ++i) {
        QString r(n, QLatin1Char('0'));
        r[i] = QLatin1Char('1');
        rows << r;
    }
    return rows;
}

// Псевдослучайная матрица с фиксированным зерном: на единичной матрице спектр
// биномиальный и слишком «гладкий», а тут распределение весов настоящее.
// Генератор свой, а не из <random>, чтобы результат не зависел от реализации
// стандартной библиотеки и совпадал от машины к машине.
inline QStringList randomMatrix(int rows, int cols, quint64 seed)
{
    quint64 state = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    auto next = [&state]() -> quint64 {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };

    QStringList m;
    for (int r = 0; r < rows; ++r) {
        QString row(cols, QLatin1Char('0'));
        for (int c = 0; c < cols; ++c)
            if (next() & 1ULL) row[c] = QLatin1Char('1');
        // Строка из одних нулей вырождает код — ставим бит по диагонали.
        if (!row.contains(QLatin1Char('1'))) row[r % cols] = QLatin1Char('1');
        m << row;
    }
    return m;
}

inline Spectrum identityPartialSpectrum(int n, int maxRows)
{
    Spectrum spec;
    for (int w = 0; w <= maxRows; ++w)
        spec[w] = binom(quint64(n), quint64(w));
    return spec;
}

// --- Аналитические спектры (третий независимый источник) ---

inline Spectrum analyticHamming7_4()    { return { {0,1}, {3,7}, {4,7}, {7,1} }; }
inline Spectrum analyticExtHamming8_4() { return { {0,1}, {4,14}, {8,1} }; }
inline Spectrum analyticGolay24_12()    { return { {0,1}, {8,759}, {12,2576}, {16,759}, {24,1} }; }

} // namespace Reference
