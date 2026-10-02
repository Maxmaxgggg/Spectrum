#pragma once

// Прототип алгоритма Брауэра–Циммермана: низ спектра с гарантией.
//
// Идея. Приведём матрицу к систематическому виду на k столбцах — там у
// каждого кодового слова стоит ровно его информационный вектор, и вес слова
// не меньше числа сложенных строк. Значит, перебрав все комбинации <= r строк,
// мы нашли ВСЕ слова веса <= r: слово меньшего веса просто не может быть суммой
// большего числа строк такой матрицы.
//
// С одним множеством этого мало — гарантия растёт как r, а цена как C(k, r).
// Возьмём m информационных множеств, не пересекающихся по столбцам. Слово, не
// пойманное ни одним перебором до r строк, несёт >= r+1 единиц на каждом из
// них, а множества не пересекаются — итого вес >= m(r+1). Та же цена в m раз
// по слагаемым, а гарантия — в m раз по показателю.
//
// Нынешний частичный перебор программы складывает строки ВВЕДЁННОЙ матрицы.
// Если она не систематическая, гарантии нет вовсе: слово веса 6 может
// требовать десяти строк. Здесь это видно на случайных матрицах.
//
// CPU и без оптимизаций намеренно: это проверка идеи, а не боевой код.

#include "reference.h"

#include <QMap>
#include <QStringList>

#ifdef _MSC_VER
    #include <intrin.h>
#endif

#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>

namespace BZ {

struct BitMatrix
{
    int rows  = 0;
    int cols  = 0;
    int words = 0;
    std::vector<uint64_t> bits;

    uint64_t*       row(int r)       { return &bits[size_t(r) * words]; }
    const uint64_t* row(int r) const { return &bits[size_t(r) * words]; }

    bool get(int r, int c) const { return (row(r)[c / 64] >> (c % 64)) & 1ULL; }
};

inline BitMatrix fromRows(const QStringList& rows)
{
    BitMatrix m;
    m.rows  = rows.size();
    m.cols  = rows.first().length();
    m.words = (m.cols + 63) / 64;
    m.bits.assign(size_t(m.rows) * m.words, 0ULL);
    for (int r = 0; r < m.rows; ++r)
        for (int c = 0; c < m.cols; ++c)
            if (rows[r].at(c) == QLatin1Char('1'))
                m.row(r)[c / 64] |= 1ULL << (c % 64);
    return m;
}

inline int popcount64(uint64_t v)
{
#ifdef _MSC_VER
    return int(__popcnt64(v));
#else
    return __builtin_popcountll(v);
#endif
}

inline int popcount(const uint64_t* a, int words)
{
    int n = 0;
    for (int i = 0; i < words; ++i) n += popcount64(a[i]);
    return n;
}

inline int popcountMasked(const uint64_t* a, const std::vector<uint64_t>& mask)
{
    int n = 0;
    for (size_t i = 0; i < mask.size(); ++i) n += popcount64(a[i] & mask[i]);
    return n;
}

// Информационное множество и матрица, систематическая на нём.
struct InfoSet
{
    std::vector<int>      columns;
    std::vector<uint64_t> mask;        // те же столбцы битовой маской
    int                   overlap = 0; // сколько столбцов уже заняты прежними множествами
    BitMatrix             systematic;
};

// Гаусс с выбором опорных столбцов. Сначала берутся столбцы вне avoid — так
// множество получается непересекающимся с прежними; если их не хватило,
// добираются занятые, и каждый такой уменьшает вклад множества в границу.
// false — матрица не полного ранга.
inline bool systematize(const BitMatrix& g, const std::vector<uint64_t>& avoid,
                        const std::vector<int>& order, InfoSet& out)
{
    BitMatrix m = g;
    out = InfoSet();
    out.mask.assign(size_t(m.words), 0ULL);

    int pivotRow = 0;
    auto tryColumns = [&](bool allowAvoided) {
        for (int c : order) {
            if (pivotRow >= m.rows) break;
            const bool avoided = (avoid[c / 64] >> (c % 64)) & 1ULL;
            if (avoided != allowAvoided)
                continue;
            int src = -1;
            for (int r = pivotRow; r < m.rows; ++r)
                if (m.get(r, c)) { src = r; break; }
            if (src < 0)
                continue;
            if (src != pivotRow)
                for (int w = 0; w < m.words; ++w)
                    std::swap(m.row(src)[w], m.row(pivotRow)[w]);
            for (int r = 0; r < m.rows; ++r)
                if (r != pivotRow && m.get(r, c))
                    for (int w = 0; w < m.words; ++w)
                        m.row(r)[w] ^= m.row(pivotRow)[w];
            out.columns.push_back(c);
            out.mask[c / 64] |= 1ULL << (c % 64);
            if (avoided) ++out.overlap;
            ++pivotRow;
        }
    };
    tryColumns(false);
    tryColumns(true);

    if (pivotRow < m.rows)
        return false;
    out.systematic = std::move(m);
    return true;
}

struct Result
{
    Reference::Spectrum spectrum;      // найденные слова, каждое один раз
    int      guaranteedBelow = 0;      // все слова веса < этого найдены
    int      sets            = 0;
    quint64  enumerated      = 0;
    std::vector<int> overlaps;
    // Лучшее перекрытие у множества, которое уже не взяли: видно, насколько
    // не хватило до ещё одного.
    int      rejectedOverlap = -1;
};

// Перебор комбинаций <= r строк матрицы; на каждое слово зовётся visit.
template <typename Visit>
inline quint64 enumerateCombinations(const BitMatrix& m, int r, Visit visit)
{
    std::vector<uint64_t> word(size_t(m.words), 0ULL);
    std::vector<int> chosen;
    quint64 count = 0;

    // Рекурсия по глубине r — для прототипа этого достаточно.
    std::function<void(int, int)> go = [&](int from, int depth) {
        if (depth > 0) {
            visit(word.data());
            ++count;
        }
        if (depth == r)
            return;
        for (int i = from; i < m.rows; ++i) {
            for (int w = 0; w < m.words; ++w) word[w] ^= m.row(i)[w];
            go(i + 1, depth + 1);
            for (int w = 0; w < m.words; ++w) word[w] ^= m.row(i)[w];
        }
    };
    go(0, 0);
    return count;
}

// Собственно алгоритм: до maxSets информационных множеств, перебор до r строк
// в каждом, дедупликация по маскам прежних множеств.
inline Result run(const QStringList& rows, int r, int maxSets)
{
    const BitMatrix g = fromRows(rows);
    Result result;
    result.spectrum[0] = 1;

    std::vector<uint64_t> used(size_t(g.words), 0ULL);
    std::vector<InfoSet>  sets;

    // Порядок столбцов при выборе опорных решает, сколько непересекающихся
    // множеств найдётся. Жадный проход слева направо на структурной матрице
    // выбирает столбцы так, что на остатке ранга не хватает. Поэтому на каждое
    // множество пробуется несколько случайных порядков, берётся тот, что
    // перекрывает прежние меньше всего.
    uint64_t state = 0x9E3779B97F4A7C15ULL;
    auto next = [&state]() { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; };

    for (int j = 0; j < maxSets; ++j) {
        InfoSet best;
        bool found = false;
        for (int attempt = 0; attempt < 64; ++attempt) {
            std::vector<int> order(size_t(g.cols));
            for (int c = 0; c < g.cols; ++c) order[c] = c;
            if (attempt > 0)
                for (int c = g.cols - 1; c > 0; --c)
                    std::swap(order[c], order[int(next() % uint64_t(c + 1))]);

            InfoSet set;
            if (!systematize(g, used, order, set))
                continue;
            if (!found || set.overlap < best.overlap) { best = std::move(set); found = true; }
            if (best.overlap == 0) break;
        }
        if (!found)
            break;
        InfoSet set = std::move(best);
        // Множество, перекрывающее прежние на r+1 столбцов и больше, к границе
        // ничего не добавляет, а перебор стоит столько же — дальше незачем.
        if (set.overlap >= r + 1) {
            result.rejectedOverlap = set.overlap;
            break;
        }
        for (int w = 0; w < g.words; ++w) used[w] |= set.mask[w];
        sets.push_back(std::move(set));
    }

    result.sets = int(sets.size());
    for (size_t j = 0; j < sets.size(); ++j) {
        result.overlaps.push_back(sets[j].overlap);
        result.guaranteedBelow += std::max(0, r + 1 - sets[j].overlap);

        result.enumerated += enumerateCombinations(sets[j].systematic, r, [&](const uint64_t* word) {
            // Слово, у которого на каком-то из прежних множеств <= r единиц,
            // уже поймано перебором того множества — второй раз не считаем.
            for (size_t i = 0; i < j; ++i)
                if (popcountMasked(word, sets[i].mask) <= r)
                    return;
            result.spectrum[popcount(word, g.words)] += 1;
        });
    }
    return result;
}

// Как перебирает программа сейчас: комбинации <= r строк введённой матрицы.
inline Reference::Spectrum naivePartial(const QStringList& rows, int r)
{
    const BitMatrix g = fromRows(rows);
    Reference::Spectrum spectrum;
    spectrum[0] = 1;
    enumerateCombinations(g, r, [&](const uint64_t* word) {
        spectrum[popcount(word, g.words)] += 1;
    });
    return spectrum;
}

// Портит систематический вид, не меняя кода: случайные сложения строк.
inline QStringList scramble(const QStringList& rows, quint64 seed)
{
    BitMatrix m = fromRows(rows);
    quint64 state = seed | 1ULL;
    auto next = [&state]() { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; };

    for (int t = 0; t < 4 * m.rows; ++t) {
        const int i = int(next() % quint64(m.rows));
        const int j = int(next() % quint64(m.rows));
        if (i == j) continue;
        for (int w = 0; w < m.words; ++w) m.row(i)[w] ^= m.row(j)[w];
    }

    QStringList out;
    for (int r = 0; r < m.rows; ++r) {
        QString row(m.cols, QLatin1Char('0'));
        for (int c = 0; c < m.cols; ++c)
            if (m.get(r, c)) row[c] = QLatin1Char('1');
        out << row;
    }
    return out;
}

// Систематическая [I | A] со случайной A — полного ранга по построению.
inline QStringList systematicRandom(int rows, int cols, quint64 seed)
{
    QStringList a = Reference::randomMatrix(rows, cols - rows, seed);
    QStringList out;
    for (int r = 0; r < rows; ++r) {
        QString row(rows, QLatin1Char('0'));
        row[r] = QLatin1Char('1');
        out << row + a[r];
    }
    return out;
}

} // namespace BZ
