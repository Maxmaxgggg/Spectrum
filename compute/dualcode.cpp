#include "dualcode.h"

#include <limits>

// Проверочная матрица: порождающая приводится к ступенчатому виду
// Гаусса–Жордана, и на каждый свободный (неопорный) столбец f строится строка
// проверочной матрицы — единица в f и единицы в опорных столбцах тех строк,
// где в столбце f стоит единица.
Matrix generatorToParity(const Matrix& gen)
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
    QVector<int> free_cols;
    for (int c = 0; c < n; ++c) if (pivot_row[c] == -1) free_cols.append(c);
    Matrix parity;
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
namespace {

mpz_class fromU64(quint64 value)
{
    // Не mpz_class(unsigned long): на Windows unsigned long 32-битный.
    mpz_class out;
    mpz_import(out.get_mpz_t(), 1, -1, sizeof(value), 0, 0, &value);
    return out;
}

} // namespace

// Раньше A_i считалось по определению: K_i(x) = Σ_t (−1)^t C(x, t) C(n − x, i − t)
// для каждой пары (i, x) по таблице биномов из больших чисел — O(n^3)
// больших умножений и сотни мегабайт на таблицу; на длине 2047 это полминуты,
// и дважды за расчёт. Теперь по каждому весу x дуального кода многочлены
// идут трёхчленной рекуррентностью
//
//     K_0(x) = 1,  K_1(x) = n − 2x,
//     (i + 1)·K_{i+1}(x) = (n − 2x)·K_i(x) − (n − i + 1)·K_{i−1}(x),
//
// — O(n) операций на вес, без таблицы (сотые доли секунды на той же длине).
// Деление на i + 1 в рекуррентности точное.
std::vector<mpz_class> macWilliams(const quint64* dualSpectrum, int length, int dualDimension)
{
    const int n = length;
    std::vector<mpz_class> sum(size_t(n) + 1, 0);
    mpz_class prev, cur, next, b;
    for (int x = 0; x <= n; ++x) {
        if (dualSpectrum[x] == 0)
            continue;
        b    = fromU64(dualSpectrum[x]);
        prev = 1;            // K_0(x)
        cur  = n - 2 * x;    // K_1(x)
        sum[0] += b * prev;
        if (n >= 1)
            sum[1] += b * cur;
        for (int i = 1; i < n; ++i) {
            next = mpz_class(n - 2 * x) * cur - mpz_class(n - i + 1) * prev;
            mpz_divexact_ui(next.get_mpz_t(), next.get_mpz_t(), static_cast<unsigned long>(i + 1));
            sum[size_t(i) + 1] += b * next;
            mpz_swap(prev.get_mpz_t(), cur.get_mpz_t());
            mpz_swap(cur.get_mpz_t(), next.get_mpz_t());
        }
    }
    // Деление на число слов дуального кода. У настоящего спектра оно точное;
    // усечение к нулю — как у прежнего деления mpz_class.
    for (mpz_class& a : sum)
        mpz_tdiv_q_2exp(a.get_mpz_t(), a.get_mpz_t(), static_cast<mp_bitcnt_t>(dualDimension));
    return sum;
}

QVector<quint64> saturatedCounts(const std::vector<mpz_class>& spectrum)
{
    QVector<quint64> out(int(spectrum.size()), 0ULL);
    for (int i = 0; i < out.size(); ++i) {
        const mpz_class& a = spectrum[size_t(i)];
        if (sgn(a) <= 0)
            continue;
        if (mpz_sizeinbase(a.get_mpz_t(), 2) > 64) {
            out[i] = std::numeric_limits<quint64>::max();
            continue;
        }
        // Не mpz_get_ui: на Windows unsigned long 32-битный.
        quint64 v = 0;
        mpz_export(&v, nullptr, -1, sizeof(v), 0, 0, a.get_mpz_t());
        out[i] = v;
    }
    return out;
}

SpectrumCounts spectrumCounts(const std::vector<mpz_class>& spectrum)
{
    SpectrumCounts out;
    out.counts = saturatedCounts(spectrum);
    for (size_t i = 0; i < spectrum.size(); ++i) {
        if (mpz_sizeinbase(spectrum[i].get_mpz_t(), 2) <= 64)
            continue;
        out.exact.resize(out.size());
        out.exact[int(i)] = QString::fromStdString(spectrum[i].get_str(10));
    }
    return out;
}
