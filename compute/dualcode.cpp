#include "dualcode.h"
#include <iostream>
QStringList generatorToParity(const QStringList& gen)
{
    if (gen.isEmpty()) return {};

    int m = gen.size();
    int n = gen.first().size();

    // Представим каждую строку как массив uint64_t слов (LSB = столбец 0).
    const int W = 64;
    int words = (n + W - 1) / W;
    QVector<QVector<uint64_t>> rows(m, QVector<uint64_t>(words, 0));

    auto set_bit = [&](QVector<uint64_t>& v, int idx) {
        v[idx / W] |= (uint64_t(1) << (idx % W));
        };
    auto get_bit = [&](const QVector<uint64_t>& v, int idx)->int {
        return (v[idx / W] >> (idx % W)) & 1u;
        };

    // Заполним rows: внешний формат предполагает строка с MSB слева.
    for (int r = 0; r < m; ++r) {
        const QString& s = gen[r].trimmed();
        for (int c = 0; c < n && c < s.size(); ++c) {
            QChar ch = s.at(c);
            if (ch == QChar('1')) {
                // позиция: c (0..n-1) — сохраняем так, чтобы индекс 0 — первый символ (левый)
                // но для простоты используем c как индекс столбца
                set_bit(rows[r], c);
            }
        }
    }

    // Гаусс по столбцам: найдем опорные столбцы и приведём матрицу к RREF (XOR над всеми строками).
    QVector<int> pivot_row(n, -1); // для каждого столбца - индекс опорной строки или -1
    int r = 0;
    for (int c = 0; c < n && r < m; ++c) {
        // найти строку с единицей в столбце c среди r..m-1
        int sel = -1;
        for (int i = r; i < m; ++i) {
            if (get_bit(rows[i], c)) { sel = i; break; }
        }
        if (sel == -1) continue;
        // swap sel и r
        if (sel != r) rows.swapItemsAt(sel, r);
        pivot_row[c] = r;
        // занулить бит c во всех остальных строках
        for (int i = 0; i < m; ++i) {
            if (i == r) continue;
            if (get_bit(rows[i], c)) {
                // rows[i] ^= rows[r]
                for (int w = 0; w < words; ++w) rows[i][w] ^= rows[r][w];
            }
        }
        ++r;
    }
    int rank = r;
    // свободные столбцы:
    QVector<int> free_cols;
    for (int c = 0; c < n; ++c) if (pivot_row[c] == -1) free_cols.append(c);

    // Для каждого свободного столбца f создаём вектор решения х (разм. n):
    // х[f]=1, а для опорных столбцов p: х[p] = parity(rows[pivot_row[p]] & mask_free),
    // где mask_free имеет 1 только в позиции f (потому что мы формируем базис, где только этот free=1).
    QStringList parity;
    parity.reserve(free_cols.size());
    for (int fcol : free_cols) {
        QVector<uint64_t> vec(words, 0);
        // установить свободную переменную
        vec[fcol / W] |= (uint64_t(1) << (fcol % W));
        // для каждого опорного столбца p посчитать значение
        for (int p = 0; p < n; ++p) {
            int prow = pivot_row[p];
            if (prow == -1) continue;
            // parity of (rows[prow] & mask where mask only fcol set) == bit at fcol in rows[prow]
            int bit = get_bit(rows[prow], fcol);
            if (bit) vec[p / W] |= (uint64_t(1) << (p % W));
        }
        // превратим vec в строку '0'/'1' длины n
        QString out;
        out.reserve(n);
        for (int c = 0; c < n; ++c) {
            int b = ((vec[c / W] >> (c % W)) & 1ull) ? 1 : 0;
            out.append(b ? QChar('1') : QChar('0'));
        }
        parity.append(out);
    }

    return parity;
}

static std::vector<std::vector<mpz_class>> buildBinomTable(int n) {
    std::vector<std::vector<mpz_class>> C(n + 1, std::vector<mpz_class>(n + 1));
    for (int i = 0; i <= n; ++i) {
        C[i][0] = 1;
        for (int j = 1; j <= i; ++j) {
            C[i][j] = C[i - 1][j - 1] + C[i - 1][j];
        }
    }
    return C;
}

// Krawtchouk K_j(i) = sum_t (-1)^t * C(i,t) * C(n-i, j-t)
// Возвращает mpz_class (целое)
static mpz_class krawtchouk(const std::vector<std::vector<mpz_class>>& C, int n, int j, int i) {
    int tmin = std::max(0, j - (n - i));
    int tmax = std::min(j, i);
    mpz_class s = 0;
    for (int t = tmin; t <= tmax; ++t) {
        mpz_class term = C[j][t] * C[n - j][i - t];
        if ((t & 1) != 0) s -= term; else s += term;
    }
    return s;
}


bool computeSpectrumFromDual(quint64* dualSpectrum, int n, int k) {
    if (!dualSpectrum) return false;
    if (n < 0 || k < 0 || k > n) return false;

    // Копируем входной массив в mpz_class-вектор (чтобы работать в big-int)
    std::vector<mpz_class> B(n + 1);
    mpz_class sumB = 0;
    for (int j = 0; j <= n; ++j) {;
        B[j] = mpz_class(std::to_string(dualSpectrum[j])); 
        sumB += B[j];
    }

    // Проверка: sumB == 2^(n-k) (в идеале). Если не так — предупреждаем, но продолжаем.
    mpz_class expected = mpz_class(1) << (n - k);
    if (sumB != expected) {
        return false;
    }
    // Построим таблицу биномиалов C[a][b] для 0..n
    auto C = buildBinomTable(n);
    // scale = 2^(n-k)
    mpz_class scale = mpz_class(1) << (n - k);

    // Вычисляем A_i по формуле MacWilliams: A_i = (1/|C^perp|) * sum_j B_j * K_j(i)
    std::vector<mpz_class> A(n + 1);
    for (int i = 0; i <= n; ++i) {
        mpz_class s = 0;
        for (int j = 0; j <= n; ++j) {
            if (B[j] == 0) continue;
            mpz_class K = krawtchouk(C, n, j, i);
            s += B[j] * K;          // mpz_class * mpz_class
        }
        // Делим на scale. Ожидаем целочисленного деления.
        if (scale == 0) return false; // защита
        // Здесь предполагается, что s делится на scale.
        mpz_class Ai = s / scale;
        
        A[i] = Ai;
    }
    // Наконец, перезаписываем исходный массив (конвертация mpz_class -> quint64)
    for (int i = 0; i <= n; ++i) {
        // безопасная конвертация: предполагаем, что A[i] помещается в 64-бит
        try {
            quint64 outVal = 0;
            mpz_export(&outVal, nullptr, -1, sizeof(outVal), 0, 0, A[i].get_mpz_t());
            dualSpectrum[i] = outVal;
        }
        catch (const std::exception& ex) {
            return false;
        }
    }

    return true;
}


bool computeSpectrumFromDual(const quint64* dualSpectrum, int n, int k, std::vector<mpz_class>& spectrumOut) {
    if (!dualSpectrum) return false;
    if (n < 0 || k < 0 || k > n) return false;

    spectrumOut.clear();
    spectrumOut.resize(n + 1);

    // Копируем входной массив в mpz_class-вектор (big-int)
    std::vector<mpz_class> B(n + 1);
    mpz_class sumB = 0;
    for (int j = 0; j <= n; ++j) {
        // mpz_class умеет инициализироваться из целочисленного значения
        B[j] =  mpz_class(std::to_string(dualSpectrum[j]));
        sumB += B[j];
    }

    // Проверка: sumB == 2^(n-k)
    mpz_class expected = mpz_class(1) << (n - k);
    if (sumB != expected) {
        // Поведение сохранено: если сумма не соответствует ожидаемой — вернуть false.
        return false;
    }

    // Построим таблицу биномиалов C[a][b] для 0..n
    auto C = buildBinomTable(n);

    // scale = |C^perp| = 2^(n-k)
    mpz_class scale = expected;
    if (scale == 0) return false; // на всякий случай

    // Вычисляем A_i по формуле MacWilliams: A_i = (1/|C^perp|) * sum_j B_j * K_j(i)
    for (int i = 0; i <= n; ++i) {
        mpz_class s = 0;
        for (int j = 0; j <= n; ++j) {
            if (B[j] == 0) continue;
            mpz_class K = krawtchouk(C, n, j, i); // ожидаем mpz_class или совместимый тип
            s += B[j] * K;
        }
        // Ожидаем целочисленного деления
        if (s % scale != 0) {
            // Если деление нецелое — считаем это ошибкой (по исходной логике)
            return false;
        }
        spectrumOut[i] = s / scale;
    }

    return true;
}