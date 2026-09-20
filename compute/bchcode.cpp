#include "bchcode.h"

#include <algorithm>
#include <numeric>

namespace Bch {

namespace {

// Восьмеричные примитивные многочлены из таблицы, m = 2..11.
const uint32_t kPrimitive[MAX_M + 1] = { 0, 0, 07, 013, 023, 045, 0103, 0211, 0435, 01021, 02011, 04005 };

struct Field
{
    int m = 0, n = 0;
    std::vector<int> exp, log;   // α^i и обратно

    explicit Field(int m_) : m(m_), n((1 << m_) - 1), exp(size_t(2 * n), 0), log(size_t(n) + 1, 0)
    {
        int x = 1;
        for (int i = 0; i < n; ++i) {
            exp[size_t(i)] = x;
            log[size_t(x)] = i;
            x <<= 1;
            if (x >> m & 1)
                x ^= int(kPrimitive[m]);
        }
        for (int i = n; i < 2 * n; ++i)
            exp[size_t(i)] = exp[size_t(i - n)];
    }
    int mul(int a, int b) const
    {
        return (a == 0 || b == 0) ? 0 : exp[size_t(log[size_t(a)] + log[size_t(b)])];
    }
};

// Циклотомический класс i по модулю n.
std::vector<int> coset(int i, int n)
{
    std::vector<int> out;
    int c = i % n;
    while (std::find(out.begin(), out.end(), c) == out.end()) {
        out.push_back(c);
        c = (c * 2) % n;
    }
    return out;
}

// Произведение (x − α^c) по классу — многочлен над GF(2), биты.
uint32_t minimalPoly(const Field& f, const std::vector<int>& cls)
{
    std::vector<int> poly = { 1 };   // коэффициенты над GF(2^m), младшая степень первой
    for (int c : cls) {
        std::vector<int> r(poly.size() + 1, 0);
        for (size_t d = 0; d < poly.size(); ++d) {
            r[d + 1] ^= poly[d];
            r[d]     ^= f.mul(poly[d], f.exp[size_t(c)]);
        }
        poly.swap(r);
    }
    uint32_t bits = 0;
    for (size_t d = 0; d < poly.size(); ++d)
        if (poly[d]) bits |= 1u << d;   // коэффициенты обязаны быть 0 или 1
    return bits;
}

// Многочлен над GF(2) произвольной степени: бит d — коэффициент при x^d.
struct Poly
{
    std::vector<uint64_t> w;
    int  deg() const
    {
        for (int i = int(w.size()) - 1; i >= 0; --i)
            if (w[size_t(i)]) return i * 64 + topBit(w[size_t(i)]);
        return -1;
    }
    static int topBit(uint64_t v)
    {
        int b = 63;
        while (!(v >> b & 1ULL)) --b;
        return b;
    }
    bool bit(int i) const { return size_t(i >> 6) < w.size() && (w[size_t(i >> 6)] >> (i & 63)) & 1ULL; }
    void set(int i)
    {
        if (size_t(i >> 6) >= w.size()) w.resize(size_t(i >> 6) + 1, 0ULL);
        w[size_t(i >> 6)] |= 1ULL << (i & 63);
    }
    void xorShifted(const Poly& b, int s)   // this ^= b · x^s
    {
        const int db = b.deg();
        for (int i = 0; i <= db; ++i)
            if (b.bit(i)) {
                const int j = i + s;
                if (size_t(j >> 6) >= w.size()) w.resize(size_t(j >> 6) + 1, 0ULL);
                w[size_t(j >> 6)] ^= 1ULL << (j & 63);
            }
    }
    Poly times(const Poly& b) const
    {
        Poly r;
        const int da = deg();
        for (int i = 0; i <= da; ++i)
            if (bit(i)) r.xorShifted(b, i);
        return r;
    }
    void reduce(const Poly& g)   // this = this mod g
    {
        const int dg = g.deg();
        for (int d = deg(); d >= dg; d = deg())
            xorShifted(g, d - dg);
    }
};

Poly fromBits(uint32_t bits)
{
    Poly p;
    for (int i = 0; i < 32; ++i)
        if (bits >> i & 1) p.set(i);
    return p;
}

// Конструктивное расстояние: сколько подряд α^1, α^2, … среди корней.
int designedDistance(const std::vector<bool>& isRoot, int n)
{
    int L = 0;
    while (L + 1 < n && isRoot[size_t(L + 1)]) ++L;
    return L + 1;
}

// Строки систематической матрицы: проверочная часть строки i — x^{n−k+i}
// mod g, младшая степень слева. Считается подряд: следующая — предыдущая,
// умноженная на x и приведённая. code.n и code.k уже с учётом расширения
// и укорочения.
void fillRows(Code& code, const Poly& g, int n, int k, bool extend, int shorten)
{
    const int r = n - k;
    Poly cur;
    cur.set(r);
    cur.reduce(g);
    for (int i = 0; i < k; ++i) {
        if (i >= shorten) {
            QString row(code.n, QLatin1Char('0'));
            row[i - shorten] = QLatin1Char('1');
            int ones = 1;
            for (int d = 0; d < r; ++d)
                if (cur.bit(d)) { row[k - shorten + d] = QLatin1Char('1'); ++ones; }
            if (extend && (ones & 1))
                row[code.n - 1] = QLatin1Char('1');
            code.rows.append(row);
        }
        Poly next;
        next.xorShifted(cur, 1);
        next.reduce(g);
        cur = next;
    }
}

Code make(int m, int reps, bool extend, int shorten, bool withRows)
{
    Code code;
    if (m < MIN_M || m > MAX_M)
        return code;
    const std::vector<MinimalPolynomial> table = minimalPolynomials(m);
    if (reps < 1 || reps > int(table.size()))
        return code;
    const int n = (1 << m) - 1;

    // g(x) и множество корней.
    Poly g = fromBits(1);
    std::vector<bool> isRoot(size_t(n), false);
    int degree = 0;
    for (int i = 0; i < reps; ++i) {
        g = g.times(fromBits(table[size_t(i)].poly));
        degree += table[size_t(i)].degree;
        for (int c : coset(table[size_t(i)].exponent, n))
            isRoot[size_t(c)] = true;
    }
    const int k = n - degree;
    if (k <= 0 || shorten < 0 || shorten >= k)
        return code;

    const int delta = designedDistance(isRoot, n);
    code.n                = n - shorten + (extend ? 1 : 0);
    code.k                = k - shorten;
    code.designedDistance = delta + (extend ? 1 : 0);
    code.corrects         = (code.designedDistance - 1) / 2;
    for (int d = degree; d >= 0; --d)
        code.generator += g.bit(d) ? QLatin1Char('1') : QLatin1Char('0');
    if (withRows)
        fillRows(code, g, n, k, extend, shorten);
    return code;
}

} // namespace

uint32_t primitivePolynomial(int m)
{
    return (m >= MIN_M && m <= MAX_M) ? kPrimitive[m] : 0;
}

std::vector<MinimalPolynomial> minimalPolynomials(int m)
{
    std::vector<MinimalPolynomial> out;
    if (m < MIN_M || m > MAX_M)
        return out;
    const Field f(m);
    std::vector<bool> seen(size_t(f.n), false);
    for (int i = 1; i < f.n; i += 2) {
        if (seen[size_t(i)])
            continue;
        const std::vector<int> cls = coset(i, f.n);
        for (int c : cls) seen[size_t(c)] = true;
        MinimalPolynomial p;
        p.exponent = i;
        p.degree   = int(cls.size());
        p.poly     = minimalPoly(f, cls);
        p.octal    = QString::number(p.poly, 8);
        out.push_back(p);
    }
    return out;
}

Code build(int m, int reps, bool extend, int shorten)
{
    return make(m, reps, extend, shorten, true);
}

Code describe(int m, int reps, bool extend, int shorten)
{
    return make(m, reps, extend, shorten, false);
}

Code cyclic(int m, uint32_t generator, bool extend, int shorten)
{
    Code code;
    if (m < MIN_M || m > MAX_M || generator < 2)
        return code;
    const Poly g  = fromBits(generator);
    const int  n  = (1 << m) - 1;
    const int  dg = g.deg();
    const int  k  = n - dg;
    if (k <= 0 || shorten < 0 || shorten >= k)
        return code;
    code.n = n - shorten + (extend ? 1 : 0);
    code.k = k - shorten;
    for (int d = dg; d >= 0; --d)
        code.generator += g.bit(d) ? QLatin1Char('1') : QLatin1Char('0');
    fillRows(code, g, n, k, extend, shorten);
    return code;
}

uint32_t reciprocal(uint32_t poly)
{
    if (poly == 0)
        return 0;
    int deg = 31;
    while (!(poly >> deg & 1u)) --deg;
    uint32_t out = 0;
    for (int d = 0; d <= deg; ++d)
        if (poly >> d & 1u) out |= 1u << (deg - d);
    return out;
}

} // namespace Bch
