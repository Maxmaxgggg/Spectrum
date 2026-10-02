#include "productcode.h"

#include "infosets.h"

#include "bitops.h"

#include <omp.h>

#include <algorithm>
#include <atomic>
#include <numeric>

namespace Product {

namespace {

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
            total += Bits::popcount64(acc);
        }
        // Дополнение за пределами n даёт единицы в старших битах, но хотя бы
        // один множитель у ненулевого u — само слово, там они нули.
        cells[size_t(u - 1)] = total;
    }
}

} // namespace

// --------------------------------------------------------------- перебор

Component bruteForce(const QStringList& rows, int wordsUpTo,
                     const std::function<bool()>& cancelled, const Progress& progress, int maxK)
{
    Component c;
    c.k = rows.size();
    c.n = c.k > 0 ? rows.first().length() : 0;
    c.wordsUpTo = wordsUpTo;
    if (c.k <= 0 || c.k > maxK || c.k > 62)
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
                weight += Bits::popcount64(word[size_t(w)]);
            ++spectrum[size_t(weight)];
            if (weight > 0 && weight <= wordsUpTo) {
                mine.insert(mine.end(), word.begin(), word.end());
                myW.push_back(weight);
            }
            if ((idx & 0xFFFF) == 0xFFFF) {
                if (cancelled && cancelled())
                    stop.store(true);
                // Ход показывает главный поток по своей доле: доли равные.
                if (tid == 0 && progress && (idx & 0xFFFFF) == 0xFFFFF)
                    progress(idx - start, end - start);
            }
            if (stop.load())
                break;
            // Следующий код Грея отличается одним битом — номером младшей
            // единицы idx + 1.
            const quint64 next = idx + 1;
            if (next >= total)
                break;
            const int flip = Bits::lowestSetBit(next);
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
//
// Отсечка — по весам, а не по носителям. Каждая позиция объединения
// носителей набора из t слов накрыта ровно 2^(t-1) ненулевыми комбинациями
// набора, поэтому сумма весов всех комбинаций равна 2^(t-1)·|объединение|.
// Значит, у набора из r слов с объединением не больше U сумма весов всех
// 2^r - 1 комбинаций не больше 2^(r-1)·U, а каждая комбинация весит не
// меньше d. Отсюда потолок веса следующего слова, и раз кандидаты
// отсортированы по весу, перебор обрывается, едва потолок пройден: из
// миллионов слов веса до 9 в пару к слову веса 8 годятся только слова
// веса 5, и их перебор кончается, не начавшись.
struct ProfileWalker
{
    const Component&            c;
    const std::vector<size_t>&  candidates;   // по возрастанию веса
    const std::vector<std::vector<int>>& perms;
    int r; int unionLimit; int words; int d;
    quint64 workLimit;
    std::atomic<quint64>& work;
    std::atomic<bool>&    over;
    const std::function<bool()>& cancelled;

    ProfileMap map;
    quint64    localWork = 0;
    std::vector<const quint64*> tuple, permuted;
    std::vector<int> cells;
    // Ненулевые комбинации префикса: слова подряд и их веса; combos — сколько
    // их сейчас (2^t - 1), spanSum — сумма их весов.
    std::vector<quint64> span;
    std::vector<int>     spanWeights;
    int                  combos  = 0;
    long long            spanSum = 0;

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

    void go(size_t from, int depth)
    {
        if (over.load(std::memory_order_relaxed))
            return;
        if (depth == r) {
            for (const std::vector<int>& p : perms) {
                for (int i = 0; i < r; ++i)
                    permuted[size_t(i)] = tuple[size_t(p[size_t(i)])];
                cellsOf(permuted, words, cells);
                ++map[packProfile(cells)];
            }
            return;
        }

        const long long budget         = (1LL << (r - 1)) * unionLimit;
        const long long pending        = (1LL << r) - (1LL << depth) - 1;   // комбинаций ещё не было
        const long long pendingAfter   = (1LL << r) - (1LL << (depth + 1)); // останется после этого слова
        const long long maxWeight      = budget - spanSum - pending * d;
        if (maxWeight < d)
            return;

        for (size_t ci = from; ci < candidates.size(); ++ci) {
            if (!tick())
                return;
            const size_t idx = candidates[ci];
            const int weight = c.weights[idx];
            if (weight > maxWeight)
                break;
            const quint64* a = c.word(idx);

            // Новые комбинации: само слово и его суммы со всеми прежними.
            // Нулевая сумма — слово уже в линейной оболочке префикса.
            long long added = weight;
            bool dependent = false;
            const int base = combos;
            for (int j = 0; j < base; ++j) {
                int wt = 0;
                const quint64* p = span.data() + size_t(j) * words;
                quint64* dst = span.data() + size_t(base + 1 + j) * words;
                for (int w = 0; w < words; ++w) {
                    dst[w] = a[w] ^ p[w];
                    wt += Bits::popcount64(dst[w]);
                }
                if (wt == 0) { dependent = true; break; }
                spanWeights[size_t(base + 1 + j)] = wt;
                added += wt;
            }
            if (dependent)
                continue;
            if (spanSum + added + pendingAfter * d > budget)
                continue;

            std::copy(a, a + words, span.data() + size_t(base) * words);
            spanWeights[size_t(base)] = weight;
            const long long savedSum = spanSum;
            spanSum += added;
            combos = 2 * base + 1;
            tuple.push_back(a);

            go(ci + 1, depth + 1);

            tuple.pop_back();
            combos  = base;
            spanSum = savedSum;
            if (over.load(std::memory_order_relaxed))
                return;
        }
    }
};

} // namespace

double estimatedProfileWork(const Component& c, int r, int unionLimit)
{
    if (r < 2 || c.d <= 0 || c.spectrum.empty())
        return 0.0;
    const int limit = std::min({ unionLimit, c.n, c.exactUpTo });
    if (limit < c.d)
        return 0.0;
    // Гистограмма весов кандидатов (по спектру) и её накопленная сумма.
    std::vector<double> count(size_t(limit) + 1, 0.0);
    for (int w = 1; w <= limit; ++w)
        count[size_t(w)] = double(c.spectrum[size_t(w)]);
    std::vector<double> upTo(size_t(limit) + 1, 0.0);
    for (int w = 0; w <= limit; ++w)
        upTo[size_t(w)] = (w > 0 ? upTo[size_t(w) - 1] : 0.0) + count[size_t(w)];
    // Как в ProfileWalker::go на глубине 1: после первого слова веса w1
    // потолок второго — budget − w1 − (комбинаций впереди)·d.
    const long long budget  = (1LL << (r - 1)) * limit;
    const long long pending = (1LL << r) - 2 - 1;
    double work = 0.0;
    for (int w1 = 1; w1 <= limit; ++w1) {
        if (count[size_t(w1)] == 0.0) continue;
        const long long maxWeight = budget - w1 - pending * c.d;
        if (maxWeight < c.d) continue;
        work += count[size_t(w1)] * upTo[size_t(std::min<long long>(maxWeight, limit))];
    }
    return work;
}

bool profiles(const Component& c, int r, int unionLimit, quint64 workLimit,
              ProfileMap& out, bool ordered, const std::function<bool()>& cancelled,
              const Progress& progress)
{
    out.clear();
    // Слов тяжелее n не бывает: предел выше длины кода — это «все слова».
    if (!c.hasWords || c.wordsUpTo < std::min(unionLimit, c.n) || r < 1 || r > 4)
        return false;
    if (c.d <= 0)
        return true;

    // Кандидаты — слова веса не больше предела, по возрастанию веса: на
    // порядке держится отсечка.
    std::vector<size_t> candidates;
    for (size_t i = 0; i < c.wordCount(); ++i)
        if (c.weights[i] <= unionLimit)
            candidates.push_back(i);
    std::stable_sort(candidates.begin(), candidates.end(),
                     [&c](size_t a, size_t b) { return c.weights[a] < c.weights[b]; });
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
    std::atomic<quint64> firstDone{ 0 };   // первых слов разобрано — для хода
    const int threads = std::max(1, omp_get_max_threads());
    std::vector<ProfileMap> maps(static_cast<size_t>(threads));

    #pragma omp parallel num_threads(threads)
    {
        ProfileWalker walker{ c, candidates, perms, r, unionLimit, words, c.d, workLimit,
                              work, over, cancelled, ProfileMap(), 0, {}, {}, {}, {}, {}, 0, 0 };
        walker.permuted.resize(static_cast<size_t>(r));
        walker.span.assign(size_t((1 << r) - 1) * size_t(words), 0ULL);
        walker.spanWeights.assign(size_t((1 << r) - 1), 0);

        // Первое слово набора раздаётся потокам; длинные хвосты у первых
        // слов, поэтому раздача динамическая. Потолок веса первого слова —
        // тот же, что и в рекурсии на нулевой глубине.
        const long long budget    = (1LL << (r - 1)) * unionLimit;
        const long long maxWeight = budget - ((1LL << r) - 2) * c.d;

        const int tid = omp_get_thread_num();
        quint64 mine = 0;

        #pragma omp for schedule(dynamic, 1)
        for (long long ci = 0; ci < (long long)candidates.size(); ++ci) {
            // Ход: главный поток раз в несколько своих слов смотрит общий счёт.
            if (tid == 0 && progress && (++mine & 15) == 0)
                progress(firstDone.load(std::memory_order_relaxed), candidates.size());
            if (over.load(std::memory_order_relaxed) || !walker.tick())
                continue;
            const size_t idx = candidates[size_t(ci)];
            const int weight = c.weights[idx];
            if (weight > maxWeight) {
                firstDone.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const quint64* a = c.word(idx);
            std::copy(a, a + words, walker.span.data());
            walker.spanWeights[0] = weight;
            walker.combos  = 1;
            walker.spanSum = weight;
            walker.tuple.assign(1, a);
            walker.go(size_t(ci) + 1, 1);
            firstDone.fetch_add(1, std::memory_order_relaxed);
        }
        maps[size_t(tid)] = std::move(walker.map);
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

std::vector<quint64> rankR(const ProfileMap& p1, const ProfileMap& p2, int r, quint64 maxWeight,
                           const Progress& progress)
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
                if (Bits::popcount64(quint64(u & v)) & 1)
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

    std::atomic<quint64> done{ 0 };
    #pragma omp parallel for schedule(dynamic, 64) num_threads(threads)
    for (long long li = 0; li < (long long)left.size(); ++li) {
        std::vector<quint64>& mine = partial[size_t(omp_get_thread_num())];
        const quint64 seen = done.fetch_add(1, std::memory_order_relaxed);
        if (omp_get_thread_num() == 0 && progress && (seen & 63) == 0)
            progress(seen, left.size());
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
