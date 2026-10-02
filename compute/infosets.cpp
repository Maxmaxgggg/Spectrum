#include "infosets.h"
#include "mixing.h"

#include <algorithm>
#include <numeric>

namespace InfoSets {

namespace {

inline bool bitAt(const quint64* row, int c)
{
    return (row[c >> 6] >> (c & 63)) & 1ULL;
}

// Сумма C(rows, r) по r <= maxRows, в double: точность тут не нужна, а для
// длинных кодов сумма ни во что целое не помещается.
double combinationsUpTo(int rows, int maxRows)
{
    double total = 0.0;
    double term  = 1.0;
    for (int r = 0; r <= maxRows && r <= rows; ++r) {
        total += term;
        term = term * double(rows - r) / double(r + 1);
    }
    return total;
}

} // namespace

std::vector<quint64> packRows(const QStringList& matrix, int& wordsPerRow)
{
    const int rows = matrix.size();
    const int cols = rows > 0 ? matrix.first().length() : 0;
    wordsPerRow = (cols + 63) / 64;

    std::vector<quint64> packed(size_t(rows) * wordsPerRow, 0ULL);
    for (int r = 0; r < rows; ++r) {
        const QString& row = matrix.at(r);
        quint64* dst = packed.data() + size_t(r) * wordsPerRow;
        for (int c = 0; c < cols; ++c)
            if (row.at(c) == QLatin1Char('1'))
                dst[c >> 6] |= 1ULL << (c & 63);
    }
    return packed;
}

bool systematize(const quint64* matrix, int rows, int cols, int wordsPerRow,
                 const std::vector<int>& order, const std::vector<quint64>* avoid,
                 InfoSet& out)
{
    std::vector<quint64> m(matrix, matrix + size_t(rows) * wordsPerRow);
    auto row = [&](int r) { return m.data() + size_t(r) * wordsPerRow; };

    out = InfoSet();
    out.mask.assign(size_t(wordsPerRow), 0ULL);
    out.columns.reserve(size_t(rows));

    int pivotRow = 0;
    auto pass = [&](bool allowAvoided) {
        for (int c : order) {
            if (pivotRow >= rows)
                break;
            if (c < 0 || c >= cols)
                continue;
            const bool avoided = avoid && bitAt(avoid->data(), c);
            if (avoided != allowAvoided)
                continue;

            int src = -1;
            for (int r = pivotRow; r < rows; ++r)
                if (bitAt(row(r), c)) { src = r; break; }
            if (src < 0)
                continue;

            if (src != pivotRow)
                std::swap_ranges(row(src), row(src) + wordsPerRow, row(pivotRow));

            const quint64* pivot = row(pivotRow);
            for (int r = 0; r < rows; ++r) {
                if (r == pivotRow || !bitAt(row(r), c))
                    continue;
                quint64* dst = row(r);
                for (int w = 0; w < wordsPerRow; ++w)
                    dst[w] ^= pivot[w];
            }

            out.columns.push_back(c);
            out.mask[size_t(c >> 6)] |= 1ULL << (c & 63);
            if (avoided)
                ++out.overlap;
            ++pivotRow;
        }
    };

    pass(false);
    if (avoid)
        pass(true);

    if (pivotRow < rows)
        return false;
    out.rows = std::move(m);
    return true;
}

std::vector<InfoSet> find(const quint64* matrix, int rows, int cols, int wordsPerRow,
                          int maxSets)
{
    std::vector<InfoSet> sets;
    if (rows <= 0 || cols <= 0 || rows > cols)
        return sets;

    // Попыток на множество. Один проход Гаусса стоит около k*k*words операций:
    // на матрице 96x336 это ничто, а на 2048x2048 — десятая доля секунды, и
    // шестьдесят четыре попытки на каждое множество растянулись бы на минуту.
    const double gaussCost = double(rows) * double(rows) * double(wordsPerRow);
    const int attempts = int(std::min(64.0, std::max(2.0, 2.0e8 / std::max(gaussCost, 1.0))));

    // Свой генератор, а не std::mt19937: поиск обязан давать одни и те же
    // множества на любой сборке, а перемешивание стандартной библиотеки
    // этого не обещает.
    Mixing::Xorshift64 rng{ 0x9E3779B97F4A7C15ULL };
    std::vector<int>     order(static_cast<size_t>(cols));
    std::vector<quint64> used(size_t(wordsPerRow), 0ULL);

    for (int j = 0; j < maxSets; ++j) {
        InfoSet best;
        bool    found = false;

        for (int attempt = 0; attempt < attempts; ++attempt) {
            std::iota(order.begin(), order.end(), 0);
            if (attempt > 0)
                for (int c = cols - 1; c > 0; --c)
                    std::swap(order[size_t(c)], order[size_t(rng.next() % quint64(c + 1))]);

            InfoSet set;
            // Опорные столбцы выбираются среди всех, поэтому неудача означает
            // зависимые строки — и никакой порядок этого не исправит.
            if (!systematize(matrix, rows, cols, wordsPerRow, order, &used, set))
                return {};
            if (!found || set.overlap < best.overlap) {
                best  = std::move(set);
                found = true;
            }
            if (best.overlap == 0)
                break;
        }
        if (!found)
            break;
        // Множество целиком в занятых столбцах к гарантии не добавит ничего.
        if (best.overlap >= rows)
            break;

        for (int w = 0; w < wordsPerRow; ++w)
            used[size_t(w)] |= best.mask[size_t(w)];
        sets.push_back(std::move(best));
    }
    return sets;
}

bool rebuild(const quint64* matrix, int rows, int cols, int wordsPerRow,
             const QVector<QVector<int>>& columns, std::vector<InfoSet>& out)
{
    out.clear();
    if (columns.isEmpty())
        return false;

    const std::vector<int> overlaps = overlapsOf(columns);
    for (int j = 0; j < columns.size(); ++j) {
        if (columns[j].size() != rows)
            return false;
        const std::vector<int> order(columns[j].cbegin(), columns[j].cend());
        InfoSet set;
        if (!systematize(matrix, rows, cols, wordsPerRow, order, nullptr, set))
            return false;
        set.overlap = overlaps[size_t(j)];
        out.push_back(std::move(set));
    }
    return true;
}

std::vector<int> overlapsOf(const QVector<QVector<int>>& columns)
{
    int maxColumn = -1;
    for (const QVector<int>& set : columns)
        for (int c : set)
            maxColumn = std::max(maxColumn, c);

    std::vector<char> used(size_t(maxColumn + 1), 0);
    std::vector<int>  overlaps;
    overlaps.reserve(size_t(columns.size()));
    for (const QVector<int>& set : columns) {
        int overlap = 0;
        for (int c : set)
            if (c >= 0 && used[size_t(c)])
                ++overlap;
        overlaps.push_back(overlap);
        for (int c : set)
            if (c >= 0)
                used[size_t(c)] = 1;
    }
    return overlaps;
}

int guaranteedBelow(const std::vector<int>& overlaps, Depth depth, int rows, int cols)
{
    if (overlaps.empty())
        return 0;
    const int sets = int(overlaps.size());
    const int last = std::max(1, std::min(depth.lastLayerSets, sets));
    if (depth.maxRows >= rows)
        return cols + 1;

    // Множество j пройдено до r_j строк: слово, которое оно не нашло, несёт на
    // нём не меньше r_j + 1 единиц, из них на ещё не занятых столбцах — не
    // меньше r_j + 1 − overlap_j.
    int bound = 0;
    for (int j = 0; j < sets; ++j) {
        const int depthOfSet = j < last ? depth.maxRows : depth.maxRows - 1;
        bound += std::max(0, depthOfSet + 1 - overlaps[size_t(j)]);
    }
    return std::min(bound, cols + 1);
}

Depth depthForWeight(const std::vector<int>& overlaps, int weight, int rows, int cols)
{
    const int sets = std::max(1, int(overlaps.size()));
    for (int r = 0; r < rows; ++r)
        for (int last = 1; last <= sets; ++last)
            if (guaranteedBelow(overlaps, Depth{ r, last }, rows, cols) > weight)
                return Depth{ r, last };
    return Depth{ rows, 1 };
}

double combinationsFor(Depth depth, int sets, int rows)
{
    // Слой maxRows по всем множествам — это то же, что maxRows−1 по всем и
    // ещё один полный слой; считаем всё в double: точность тут не нужна.
    const double full = combinationsUpTo(rows, depth.maxRows - 1);
    double top = 1.0;
    for (int i = 0; i < depth.maxRows && i < rows; ++i)
        top = top * double(rows - i) / double(i + 1);
    if (depth.maxRows > rows)
        top = 0.0;
    return double(sets) * full + double(std::min(depth.lastLayerSets, sets)) * top;
}

int setsForWeight(const std::vector<int>& overlaps, int weight, int rows, int cols)
{
    int    bestSets = 0;
    double bestCost = 0.0;
    for (int m = 1; m <= int(overlaps.size()); ++m) {
        const std::vector<int> prefix(overlaps.begin(), overlaps.begin() + m);
        const Depth  depth = depthForWeight(prefix, weight, rows, cols);
        const double cost  = combinationsFor(depth, m, rows);
        if (bestSets == 0 || cost < bestCost) {
            bestSets = m;
            bestCost = cost;
        }
    }
    return bestSets;
}

} // namespace InfoSets
