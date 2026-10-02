#include "cyclic.h"
#include "bitops.h"
#include "infosets.h"

#include <algorithm>
#include <cstring>
#include <numeric>

namespace Cyclic {

namespace {

// Круг длиной до 2048 столбцов и сдвиг начала на один — 33 слова с запасом.
constexpr int MAX_WORDS = 34;

inline bool bitAt(const quint64* v, int i)
{
    return (v[i >> 6] >> (i & 63)) & 1ULL;
}

// dst = src >> bits (по всем words словам), старшие биты — нули.
void shiftRight(const quint64* src, int words, int bits, quint64* dst)
{
    const int whole = bits >> 6, part = bits & 63;
    for (int i = 0; i < words; ++i) {
        const int j = i + whole;
        quint64 v = j < words ? src[j] >> part : 0ULL;
        if (part != 0 && j + 1 < words)
            v |= src[j + 1] << (64 - part);
        dst[i] = v;
    }
}

// dst = src << bits (по всем words словам), то, что ушло за край, теряется.
void shiftLeft(const quint64* src, int words, int bits, quint64* dst)
{
    const int whole = bits >> 6, part = bits & 63;
    for (int i = words - 1; i >= 0; --i) {
        const int j = i - whole;
        quint64 v = j >= 0 ? src[j] << part : 0ULL;
        if (part != 0 && j - 1 >= 0)
            v |= src[j - 1] >> (64 - part);
        dst[i] = v;
    }
}

// Оставить младшие bits бит.
void keepLow(quint64* v, int words, int bits)
{
    for (int i = 0; i < words; ++i) {
        const int lo = i * 64;
        if (lo >= bits)
            v[i] = 0ULL;
        else if (bits - lo < 64)
            v[i] &= (1ULL << (bits - lo)) - 1ULL;
    }
}

struct Circle
{
    // words — слов под length бит; span — сколько слов трогают сдвиги: круг
    // и слово целиком с запасом на сдвиг начала. Остальные слова массивов —
    // нули, их не обходим.
    int start, length, words, span;

    Circle(const Symmetry& s, int wordsPerRow)
        : start(s.start), length(s.length), words((s.length + 63) / 64)
        , span(std::min(MAX_WORDS, std::max(words, wordsPerRow) + 1)) {}

    // Биты круга, прижатые к нулю.
    void extract(const quint64* word, int wordsPerRow, quint64* out) const
    {
        quint64 tmp[MAX_WORDS] = {};
        std::memcpy(tmp, word, size_t(wordsPerRow) * sizeof(quint64));
        shiftRight(tmp, span, start, out);
        keepLow(out, span, length);
    }

    // Сдвиг по кругу на shift: бит j переходит в (j + shift) mod length.
    // Что ушло за span слов при сдвиге влево, лежало выше length и всё равно
    // отрезается.
    void rotate(const quint64* in, int shift, quint64* out) const
    {
        shift %= length;
        if (shift < 0) shift += length;
        quint64 a[MAX_WORDS], b[MAX_WORDS];
        shiftLeft(in, span, shift, a);
        shiftRight(in, span, length - shift, b);
        for (int i = 0; i < span; ++i)
            out[i] = a[i] | b[i];
        keepLow(out, span, length);
    }

    // Слово, у которого биты круга заменены на bits.
    void insert(const quint64* word, int wordsPerRow, const quint64* bits, quint64* out) const
    {
        quint64 placed[MAX_WORDS], mask[MAX_WORDS], ones[MAX_WORDS];
        shiftLeft(bits, span, start, placed);
        for (int i = 0; i < span; ++i) ones[i] = ~0ULL;
        keepLow(ones, span, length);
        shiftLeft(ones, span, start, mask);
        for (int w = 0; w < wordsPerRow; ++w)
            out[w] = (word[w] & ~mask[w]) | placed[w];
    }
};

// Сравнение двух кругов как массивов слов, с младшего.
inline bool less(const quint64* a, const quint64* b, int words)
{
    for (int i = 0; i < words; ++i)
        if (a[i] != b[i])
            return a[i] < b[i];
    return false;
}

} // namespace

Symmetry find(const quint64* matrix, int rows, int cols, int wordsPerRow)
{
    if (rows <= 0 || cols < 2 || wordsPerRow * 64 + 64 > MAX_WORDS * 64)
        return Symmetry();

    // Код в систематическом виде на первых независимых столбцах: вектор лежит
    // в коде, если после вычёркивания опорных единиц ничего не остаётся.
    std::vector<int> order(static_cast<size_t>(cols));
    std::iota(order.begin(), order.end(), 0);
    InfoSets::InfoSet set;
    if (!InfoSets::systematize(matrix, rows, cols, wordsPerRow, order, nullptr, set))
        return Symmetry();
    auto inCode = [&](std::vector<quint64> v) {
        for (int i = 0; i < rows; ++i) {
            if (!bitAt(v.data(), set.columns[size_t(i)]))
                continue;
            const quint64* row = set.rows.data() + size_t(i) * wordsPerRow;
            for (int w = 0; w < wordsPerRow; ++w)
                v[size_t(w)] ^= row[w];
        }
        return std::all_of(v.begin(), v.end(), [](quint64 x) { return x == 0ULL; });
    };

    const Symmetry candidates[] = { { 0, cols }, { 0, cols - 1 }, { 1, cols - 1 } };
    std::vector<quint64> shifted(static_cast<size_t>(wordsPerRow));
    for (const Symmetry& s : candidates) {
        if (!s.active())
            continue;
        bool ok = true;
        for (int i = 0; i < rows && ok; ++i) {
            rotate(matrix + size_t(i) * wordsPerRow, wordsPerRow, s, 1, shifted.data());
            ok = inCode(shifted);
        }
        if (ok)
            return s;
    }
    return Symmetry();
}

void rotate(const quint64* word, int wordsPerRow, const Symmetry& symmetry, int shift, quint64* out)
{
    if (!symmetry.active()) {
        std::copy(word, word + wordsPerRow, out);
        return;
    }
    const Circle circle(symmetry, wordsPerRow);
    quint64 bits[MAX_WORDS], turned[MAX_WORDS];
    circle.extract(word, wordsPerRow, bits);
    circle.rotate(bits, shift, turned);
    circle.insert(word, wordsPerRow, turned, out);
}

int canonical(const quint64* word, int wordsPerRow, const Symmetry& symmetry, quint64* out)
{
    if (!symmetry.active()) {
        std::copy(word, word + wordsPerRow, out);
        return 1;
    }
    const Circle circle(symmetry, wordsPerRow);
    const int L = circle.length;
    quint64 bits[MAX_WORDS];
    circle.extract(word, wordsPerRow, bits);

    // Кандидаты — сдвиги, ставящие одну из единиц круга на его начало.
    quint64 best[MAX_WORDS], candidate[MAX_WORDS];
    bool found = false;
    for (int i = 0; i < circle.words; ++i) {
        for (quint64 rest = bits[i]; rest; rest &= rest - 1) {
            const int j = i * 64 + BitOps::lowestSetBit(rest);
            circle.rotate(bits, L - j, candidate);
            if (!found || less(candidate, best, circle.words)) {
                std::copy(candidate, candidate + circle.span, best);
                found = true;
            }
        }
    }
    if (!found) {
        std::copy(word, word + wordsPerRow, out);
        return 1;
    }

    // Размер орбиты — наименьший период: делитель L, сдвиг на который
    // оставляет круг на месте.
    int orbit = L;
    for (int p = 1; p < L; ++p) {
        if (L % p != 0)
            continue;
        circle.rotate(best, p, candidate);
        if (std::equal(candidate, candidate + circle.words, best)) {
            orbit = p;
            break;
        }
    }
    circle.insert(word, wordsPerRow, best, out);
    return orbit;
}

std::vector<int> orbitSizes(const Symmetry& symmetry, int cyclicWeight)
{
    if (!symmetry.active() || cyclicWeight <= 0)
        return { 1 };
    const int L = symmetry.length;
    std::vector<int> sizes;
    for (int p = 1; p <= L; ++p)
        if (L % p == 0 && cyclicWeight % (L / p) == 0)
            sizes.push_back(p);
    return sizes;
}

} // namespace Cyclic
