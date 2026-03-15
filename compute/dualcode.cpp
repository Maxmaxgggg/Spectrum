#include "dualcode.h"
#include <iostream>


// Функция для генерации дуальной матрицы (писал GPT)
QStringList generatorToParity(const QStringList& gen)
{
    if (gen.isEmpty()) return {};

    int m = gen.size();
    int n = gen.first().size();
    const int W = 64;
    int words = (n + W - 1) / W;
    QVector<QVector<uint64_t>> rows(m, QVector<uint64_t>(words, 0));

    auto set_bit = [&](QVector<uint64_t>& v, int idx) {
        v[idx / W] |= (uint64_t(1) << (idx % W));
        };
    auto get_bit = [&](const QVector<uint64_t>& v, int idx)->int {
        return (v[idx / W] >> (idx % W)) & 1u;
        };
    for (int r = 0; r < m; ++r) {
        const QString& s = gen[r].trimmed();
        for (int c = 0; c < n && c < s.size(); ++c) {
            QChar ch = s.at(c);
            if (ch == QChar('1')) {
                set_bit(rows[r], c);
            }
        }
    }
    QVector<int> pivot_row(n, -1);
    int r = 0;
    for (int c = 0; c < n && r < m; ++c) {
        int sel = -1;
        for (int i = r; i < m; ++i) {
            if (get_bit(rows[i], c)) { sel = i; break; }
        }
        if (sel == -1) continue;
        if (sel != r) rows.swapItemsAt(sel, r);
        pivot_row[c] = r;
        for (int i = 0; i < m; ++i) {
            if (i == r) continue;
            if (get_bit(rows[i], c)) {
                for (int w = 0; w < words; ++w) rows[i][w] ^= rows[r][w];
            }
        }
        ++r;
    }
    int rank = r;
    QVector<int> free_cols;
    for (int c = 0; c < n; ++c) if (pivot_row[c] == -1) free_cols.append(c);
    QStringList parity;
    parity.reserve(free_cols.size());
    for (int fcol : free_cols) {
        QVector<uint64_t> vec(words, 0);
        vec[fcol / W] |= (uint64_t(1) << (fcol % W));
        for (int p = 0; p < n; ++p) {
            int prow = pivot_row[p];
            if (prow == -1) continue;
            int bit = get_bit(rows[prow], fcol);
            if (bit) vec[p / W] |= (uint64_t(1) << (p % W));
        }
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
// Биноминальная таблица для расчета дуального спектра
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

// Многочлены для расчета дуального спектра
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

// Функция, которая рассчитывает спектр из дуального и записывает его в QStringList
QStringList computeSpectrumFromDual(quint64* dualSpectrum, int numOfCols, int numOfRows) {

    // Переводим параметры дуального кода в обычный
    numOfRows = numOfCols - numOfRows;
    // Копируем входной массив в mpz-вектор
    std::vector<mpz_class> B(numOfCols + 1);
    mpz_class sumB = 0;
    for (int j = 0; j <= numOfCols; ++j) {
        quint64 t = dualSpectrum[j];
        B[j] = mpz_class(std::to_string(dualSpectrum[j]));
        sumB += B[j];
    }

    mpz_class expected = mpz_class(1) << (numOfCols - numOfRows);
    auto C = buildBinomTable(numOfCols);
    mpz_class scale = mpz_class(1) << (numOfCols - numOfRows);

    std::vector<mpz_class> A(numOfCols + 1);
    for (int i = 0; i <= numOfCols; ++i) {
        mpz_class s = 0;
        for (int j = 0; j <= numOfCols; ++j) {
            if (B[j] == 0) continue;
            mpz_class K = krawtchouk(C, numOfCols, j, i);
            s += B[j] * K;
        }
        mpz_class Ai = s / scale;

        A[i] = Ai;
    }

    QStringList result;
    for (int i = 0; i <= numOfCols; ++i) {
        QString Ai_str = QString::fromStdString(A[i].get_str(10));
        if (Ai_str != "0") {
            result.append(QString("%1 - %2").arg(i).arg(Ai_str));
        }
    }
    return result;
}