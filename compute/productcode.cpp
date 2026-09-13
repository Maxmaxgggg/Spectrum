#include "productcode.h"

#include "infosets.h"

#include <omp.h>

#ifdef _MSC_VER
    #include <intrin.h>
#endif

#include <algorithm>
#include <atomic>
#include <numeric>

namespace Product {

int bruteForceMaxK = 28;

namespace {

inline int popcount64(quint64 v)
{
#ifdef _MSC_VER
    return int(__popcnt64(v));
#else
    return __builtin_popcountll(v);
#endif
}

inline int trailingZeros(quint64 v)
{
#ifdef _MSC_VER
    unsigned long index = 0;
    _BitScanForward64(&index, v);
    return int(index);
#else
    return __builtin_ctzll(v);
#endif
}

// Ранг набора слов — Гаусс на r строках. r маленькое, слов немного.
bool independent(const std::vector<const quint64*>& tuple, int words)
{
    const int r = int(tuple.size());
    std::vector<quint64> m;
    m.reserve(size_t(r) * words);
    for (const quint64* w : tuple)
        m.insert(m.end(), w, w + words);

    int rank = 0;
    for (int bit = 0; bit < words * 64 && rank < r; ++bit) {
        const int wi = bit >> 6;
        const quint64 mask = 1ULL << (bit & 63);
        int src = -1;
        for (int row = rank; row < r; ++row)
            if (m[size_t(row) * words + wi] & mask) { src = row; break; }
        if (src < 0)
            continue;
        if (src != rank)
            std::swap_ranges(m.begin() + src * words, m.begin() + (src + 1) * words,
                             m.begin() + rank * words);
        for (int row = 0; row < r; ++row)
            if (row != rank && (m[size_t(row) * words + wi] & mask))
                for (int w = 0; w < words; ++w)
                    m[size_t(row) * words + w] ^= m[size_t(rank) * words + w];
        ++rank;
    }
    return rank == r;
}

// Ячейки профиля набора: для каждого ненулевого u — сколько позиций x, где
// (a_1(x), …, a_r(x)) = u.
void cellsOf(const std::vector<const quint64*>& tuple, int words, std::vector<int>& cells)
{
    const int r = int(tuple.size());
    const int count = (1 << r) - 1;
    cells.assign(size_t(count), 0);
    for (int u = 1; u <= count; ++u) {
        int total = 0;
        for (int w = 0; w < words; ++w) {
            quint64 acc = ~0ULL;
            for (int i = 0; i < r; ++i) {
                const quint64 a = tuple[size_t(i)][w];
                acc &= (u >> i) & 1 ? a : ~a;
            }
            total += popcount64(acc);
        }
        // Дополнение за пределами n даёт единицы в старших битах, но хотя бы
        // один множитель у ненулевого u — само слово, там они нули.
        cells[size_t(u - 1)] = total;
    }
}

} // namespace

// --------------------------------------------------------------- перебор

Component bruteForce(const QStringList& rows, int wordsUpTo,
                     const std::function<bool()>& cancelled)
{
    Component c;
    c.k = rows.size();
    c.n = c.k > 0 ? rows.first().length() : 0;
    c.wordsUpTo = wordsUpTo;
    if (c.k <= 0 || c.k > bruteForceMaxK || c.k > 62)
        return c;

    const std::vector<quint64> packed = InfoSets::packRows(rows, c.wordsPerRow);
    const int words = c.wordsPerRow;
    const quint64 total = 1ULL << c.k;

    const int threads = std::max(1, omp_get_max_threads());
    std::vector<std::vector<quint64>> spectra(size_t(threads), std::vector<quint64>(size_t(c.n) + 1, 0ULL));
    std::vector<std::vector<quint64>> found(static_cast<size_t>(threads));
    std::vector<std::vector<int>>     foundWeights(static_cast<size_t>(threads));
    std::atomic<bool> stop{ false };

    #pragma omp parallel num_threads(threads)
    {
        const int tid = omp_get_thread_num();
        const quint64 start = total * quint64(tid) / quint64(threads);
        const quint64 end   = total * quint64(tid + 1) / quint64(threads);
        std::vector<quint64>& spectrum = spectra[size_t(tid)];
        std::vector<quint64>& mine     = found[size_t(tid)];
        std::vector<int>&     myW      = foundWeights[size_t(tid)];

        std::vector<quint64> word(size_t(words), 0ULL);
        // Стартовая маска — код Грея номера start.
        quint64 mask = start ^ (start >> 1);
        for (int i = 0; i < c.k; ++i)
            if (mask & (1ULL << i))
                for (int w = 0; w < words; ++w)
                    word[size_t(w)] ^= packed[size_t(i) * words + w];

        for (quint64 idx = start; idx < end; ++idx) {
            int weight = 0;
            for (int w = 0; w < words; ++w)
                weight += popcount64(word[size_t(w)]);
            ++spectrum[size_t(weight)];
            if (weight > 0 && weight <= wordsUpTo) {
                mine.insert(mine.end(), word.begin(), word.end());
                myW.push_back(weight);
            }
            if ((idx & 0xFFFF) == 0xFFFF && cancelled && cancelled())
                stop.store(true);
            if (stop.load())
                break;
            // Следующий код Грея отличается одним битом — номером младшей
            // единицы idx + 1.
            const quint64 next = idx + 1;
            if (next >= total)
                break;
            const int flip = trailingZeros(next);
            for (int w = 0; w < words; ++w)
                word[size_t(w)] ^= packed[size_t(flip) * words + w];
        }
    }
    if (stop.load())
        return c;

    c.spectrum.assign(size_t(c.n) + 1, 0ULL);
    for (const std::vector<quint64>& s : spectra)
        for (size_t w = 0; w < s.size(); ++w)
            c.spectrum[w] += s[w];
    for (int t = 0; t < threads; ++t) {
        c.words.insert(c.words.end(), found[size_t(t)].begin(), found[size_t(t)].end());
        c.weights.insert(c.weights.end(), foundWeights[size_t(t)].begin(), foundWeights[size_t(t)].end());
    }
    c.exactUpTo = c.n;
    c.hasWords  = true;
    for (int w = 1; w <= c.n; ++w)
        if (c.spectrum[size_t(w)] > 0) { c.d = w; break; }
    return c;
}

Component fromSpectrum(int n, int k, const std::vector<quint64>& spectrum, int exactUpTo)
{
    Component c;
    c.n = n;
    c.k = k;
    c.wordsPerRow = (n + 63) / 64;
    c.spectrum.assign(size_t(n) + 1, 0ULL);
    for (size_t w = 0; w < spectrum.size() && w <= size_t(n); ++w)
        c.spectrum[w] = spectrum[w];
    c.exactUpTo = std::min(exactUpTo, n);
    for (int w = 1; w <= c.exactUpTo; ++w)
        if (c.spectrum[size_t(w)] > 0) { c.d = w; break; }
    return c;
}

// --------------------------------------------------------------- ранг 1

std::vector<quint64> rankOne(const Component& c1, const Component& c2, quint64 maxWeight)
{
    std::vector<quint64> result(size_t(maxWeight) + 1, 0ULL);
    for (int i = 1; i <= c1.exactUpTo && i <= c1.n; ++i) {
        const quint64 a = c1.spectrum[size_t(i)];
        if (a == 0)
            continue;
        for (int j = 1; j <= c2.exactUpTo && j <= c2.n; ++j) {
            const quint64 b = c2.spectrum[size_t(j)];
            if (b == 0)
                continue;
            const quint64 w = quint64(i) * quint64(j);
            if (w > maxWeight)
                break;
            result[size_t(w)] += a * b;
        }
    }
    return result;
}

// --------------------------------------------------------------- профили

ProfileKey packProfile(const std::vector<int>& cells)
{
    ProfileKey key;
    for (size_t i = 0; i < cells.size(); ++i) {
        const quint64 v = quint64(cells[i]) & 0xFFFULL;
        if (i < 5)       key.lo  |= v << (12 * i);
        else if (i < 10) key.hi  |= v << (12 * (i - 5));
        else             key.top |= v << (12 * (i - 10));
    }
    return key;
}

std::vector<int> unpackProfile(const ProfileKey& key, int cellCount)
{
    std::vector<int> cells(size_t(cellCount), 0);
    for (int i = 0; i < cellCount; ++i) {
        const quint64 src = i < 5 ? key.lo : i < 10 ? key.hi : key.top;
        const int     at  = i < 5 ? i : i < 10 ? i - 5 : i - 10;
        cells[size_t(i)] = int((src >> (12 * at)) & 0xFFFULL);
    }
    return cells;
}

namespace {

// Обход наборов одним потоком: своя карта профилей, свои буферы. Потоки
// делят между собой первое слово набора, остальное — рекурсия.
struct ProfileWalker
{
    const Component&            c;
    const std::vector<size_t>&  candidates;
    const std::vector<std::vector<int>>& perms;
    int r; int unionLimit; int words;
    quint64 workLimit;
    std::atomic<quint64>& work;
    std::atomic<bool>&    over;
    const std::function<bool()>& cancelled;

    ProfileMap map;
    quint64    localWork = 0;
    std::vector<const quint64*> tuple, permuted;
    std::vector<int> cells;

    // Счётчик работы общий, но пополняется пачками: атомарный инкремент на
    // каждого кандидата съел бы весь выигрыш от потоков.
    bool tick()
    {
        if (++localWork < 4096)
            return true;
        const quint64 total = work.fetch_add(localWork) + localWork;
        localWork = 0;
        if (total > workLimit || (cancelled && cancelled())) {
            over.store(true);
            return false;
        }
        return true;
    }

    void go(size_t from, int depth, const std::vector<quint64>& unionSoFar)
    {
        if (over.load(std::memory_order_relaxed))
            return;
        if (depth == r) {
            if (!independent(tuple, words))
                return;
            for (const std::vector<int>& p : perms) {
                for (int i = 0; i < r; ++i)
                    permuted[size_t(i)] = tuple[size_t(p[size_t(i)])];
                cellsOf(permuted, words, cells);
                ++map[packProfile(cells)];
            }
            return;
        }
        std::vector<quint64> u(static_cast<size_t>(words));
        for (size_t ci = from; ci < candidates.size(); ++ci) {
            if (!tick())
                return;
            const quint64* a = c.word(candidates[ci]);
            int size = 0;
            for (int w = 0; w < words; ++w) {
                u[size_t(w)] = unionSoFar[size_t(w)] | a[w];
                size += popcount64(u[size_t(w)]);
            }
            if (size > unionLimit)
                continue;
            tuple.push_back(a);
            go(ci + 1, depth + 1, u);
            tuple.pop_back();
            if (over.load(std::memory_order_relaxed))
                return;
        }
    }
};

} // namespace

bool profiles(const Component& c, int r, int unionLimit, quint64 workLimit,
              ProfileMap& out, bool ordered, const std::function<bool()>& cancelled)
{
    out.clear();
    // Слов тяжелее n не бывает: предел выше длины кода — это «все слова».
    if (!c.hasWords || c.wordsUpTo < std::min(unionLimit, c.n) || r < 1 || r > 4)
        return false;

    // Кандидаты — слова веса не больше предела; тяжелее в набор не попадут.
    std::vector<size_t> candidates;
    for (size_t i = 0; i < c.wordCount(); ++i)
        if (c.weights[i] <= unionLimit)
            candidates.push_back(i);
    const int words = c.wordsPerRow;

    // Наборы перебираются по возрастанию индексов — по одному на множество,
    // а упорядоченные получаются перестановками: их профили — те же ячейки
    // в другом порядке.
    std::vector<int> perm(static_cast<size_t>(r));
    std::iota(perm.begin(), perm.end(), 0);
    std::vector<std::vector<int>> perms;
    if (ordered) {
        do perms.push_back(perm); while (std::next_permutation(perm.begin(), perm.end()));
    } else {
        perms.push_back(perm);
    }

    std::atomic<quint64> work{ 0 };
    std::atomic<bool>    over{ false };
    const int threads = std::max(1, omp_get_max_threads());
    std::vector<ProfileMap> maps(static_cast<size_t>(threads));

    #pragma omp parallel num_threads(threads)
    {
        ProfileWalker walker{ c, candidates, perms, r, unionLimit, words, workLimit,
                              work, over, cancelled, ProfileMap(), 0, {}, {}, {} };
        walker.permuted.resize(static_cast<size_t>(r));
        std::vector<quint64> empty(static_cast<size_t>(words), 0ULL);
        std::vector<quint64> u(static_cast<size_t>(words));

        // Первое слово набора раздаётся потокам; длинные хвосты у первых
        // слов, поэтому раздача динамическая.
        #pragma omp for schedule(dynamic, 1)
        for (long long ci = 0; ci < (long long)candidates.size(); ++ci) {
            if (over.load(std::memory_order_relaxed) || !walker.tick())
                continue;
            const quint64* a = c.word(candidates[size_t(ci)]);
            int size = 0;
            for (int w = 0; w < words; ++w) {
                u[size_t(w)] = a[w];
                size += popcount64(u[size_t(w)]);
            }
            if (size > unionLimit)
                continue;
            walker.tuple.assign(1, a);
            walker.go(size_t(ci) + 1, 1, u);
        }
        maps[size_t(omp_get_thread_num())] = std::move(walker.map);
    }
    if (over.load()) {
        out.clear();
        return false;
    }
    for (ProfileMap& m : maps)
        for (const auto& entry : m)
            out[entry.first] += entry.second;
    return true;
}

// --------------------------------------------------------------- ранг r

quint64 generalLinearOrder(int r)
{
    quint64 order = 1;
    for (int i = 0; i < r; ++i)
        order *= (1ULL << r) - (1ULL << i);
    return order;
}

quint64 rankWeightBound(int d1, int d2, int r)
{
    quint64 s1 = 0, s2 = 0;
    for (int i = 0; i < r; ++i) {
        s1 += quint64((d1 + (1 << i) - 1) >> i);
        s2 += quint64((d2 + (1 << i) - 1) >> i);
    }
    return std::max(quint64(d2) * s1, quint64(d1) * s2);
}

std::vector<quint64> rankR(const ProfileMap& p1, const ProfileMap& p2, int r, quint64 maxWeight)
{
    std::vector<quint64> result(size_t(maxWeight) + 1, 0ULL);
    if (p1.empty() || p2.empty())
        return result;
    const int cellCount = (1 << r) - 1;

    // Для профиля q второй компоненты — веса всех комбинаций набора:
    // wq(u) = Σ_{v : <u,v> = 1} q_v. Тогда вес слова = Σ_u p_u · wq(u).
    struct Right { std::vector<int> wq; quint64 count; };
    std::vector<Right> right;
    right.reserve(p2.size());
    for (const auto& entry : p2) {
        const std::vector<int> q = unpackProfile(entry.first, cellCount);
        Right rt;
        rt.wq.assign(size_t(cellCount), 0);
        rt.count = entry.second;
        for (int u = 1; u <= cellCount; ++u) {
            int sum = 0;
            for (int v = 1; v <= cellCount; ++v)
                if (popcount64(quint64(u & v)) & 1)
                    sum += q[size_t(v - 1)];
            rt.wq[size_t(u - 1)] = sum;
        }
        right.push_back(std::move(rt));
    }

    std::vector<std::pair<std::vector<int>, quint64>> left;
    left.reserve(p1.size());
    for (const auto& entry : p1)
        left.push_back({ unpackProfile(entry.first, cellCount), entry.second });

    const int threads = std::max(1, omp_get_max_threads());
    std::vector<std::vector<quint64>> partial(size_t(threads), std::vector<quint64>(size_t(maxWeight) + 1, 0ULL));

    #pragma omp parallel for schedule(dynamic, 64) num_threads(threads)
    for (long long li = 0; li < (long long)left.size(); ++li) {
        std::vector<quint64>& mine = partial[size_t(omp_get_thread_num())];
        const std::vector<int>& p = left[size_t(li)].first;
        const quint64 m1 = left[size_t(li)].second;
        for (const Right& rt : right) {
            quint64 weight = 0;
            for (int u = 0; u < cellCount; ++u)
                weight += quint64(p[size_t(u)]) * quint64(rt.wq[size_t(u)]);
            if (weight <= maxWeight)
                mine[size_t(weight)] += m1 * rt.count;
        }
    }
    for (const std::vector<quint64>& part : partial)
        for (size_t w = 0; w < result.size(); ++w)
            result[w] += part[w];

    // Первая компонента перебиралась по множествам, вторая — по упорядоченным
    // наборам: каждое слово ранга r получено |GL(r,2)| / r! раз.
    quint64 divisor = generalLinearOrder(r);
    for (int i = 2; i <= r; ++i)
        divisor /= quint64(i);
    for (quint64& v : result)
        v /= divisor;
    return result;
}

} // namespace Product
