#include "leonsearch.h"

#include <omp.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace Leon {

namespace {

// log C(a, b) через лог-гамму: биномы длинного кода в double не помещаются,
// а их отношения — вполне.
double logBinom(int a, int b)
{
    if (b < 0 || b > a)
        return -HUGE_VAL;
    return std::lgamma(double(a) + 1.0) - std::lgamma(double(b) + 1.0)
         - std::lgamma(double(a - b) + 1.0);
}

// Перемешивание номера попытки в затравку: соседние номера дают несвязанные
// последовательности (splitmix64).
quint64 seedFor(quint64 index)
{
    quint64 z = index + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

struct Xorshift
{
    quint64 state;
    quint64 next()
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
};

} // namespace

double catchProbability(int n, int k, int weight, int rows)
{
    if (n <= 0 || k <= 0 || weight <= 0 || rows <= 0)
        return 0.0;
    if (weight > n)
        return 0.0;

    // P(X <= rows), X — число единиц слова на k случайных столбцах из n.
    const double logTotal = logBinom(n, k);
    double p = 0.0;
    for (int x = 1; x <= rows && x <= weight; ++x) {
        const double term = logBinom(weight, x) + logBinom(n - weight, k - x) - logTotal;
        if (std::isfinite(term))
            p += std::exp(term);
    }
    // Нулевого числа единиц у ненулевого кодового слова на информационном
    // множестве не бывает, поэтому x начинается с единицы.
    return std::min(1.0, p);
}

double wordsPerTrial(int k, int rows)
{
    double total = 0.0;
    double term  = 1.0;   // C(k, 0)
    for (int i = 1; i <= rows && i <= k; ++i) {
        term = term * double(k - i + 1) / double(i);
        total += term;
    }
    return total;
}

quint64 trialsFor(double catchProbability, double miss)
{
    if (catchProbability <= 0.0)
        return std::numeric_limits<quint64>::max();
    if (miss <= 0.0 || miss >= 1.0)
        return 1;
    // (1 - p)^T <= miss  =>  T >= ln(miss) / ln(1 - p)
    const double t = std::log(miss) / std::log1p(-catchProbability);
    if (!std::isfinite(t) || t >= 1.8e19)
        return std::numeric_limits<quint64>::max();
    return quint64(std::ceil(std::max(1.0, t)));
}

double gaussCostInWords(int k, int wordsPerRow, bool gpu)
{
    const double cpu = double(k) * double(k) * double(wordsPerRow) / 8.0;
    // Замер на 96x336: попытка с двумя строками (почти один Гаусс) — 7 мкс,
    // с тремя — 34 мкс на 147 тысяч слов; Гаусс блока стоит как 38 тысяч
    // слов, в пять с половиной раз дороже, чем на процессоре относительно
    // перебора.
    return gpu ? cpu * 5.5 : cpu;
}

quint64 trialsForAll(int n, int k, int weight, int rows, double miss,
                     const std::vector<quint64>& foundByWeight)
{
    quint64 needed = 1;
    for (int w = 1; w <= weight && w <= n; ++w) {
        const quint64 found = size_t(w) < foundByWeight.size() ? foundByWeight[size_t(w)] : 0;
        const double  each  = miss / double(std::max<quint64>(1, found));
        needed = std::max(needed, trialsFor(catchProbability(n, k, w, rows), each));
    }
    return needed;
}

Plan plan(int n, int k, int weight, double miss, bool gpu)
{
    Plan best;
    double bestCost = 0.0;
    const double gauss = gaussCostInWords(k, (n + 63) / 64, gpu);
    // Глубже четырёх строк за попытку не имеет смысла: столько уже дешевле
    // отдать Брауэру–Циммерману. Глубже k не бывает.
    for (int rows = 1; rows <= 4 && rows <= k; ++rows) {
        const double  p      = catchProbability(n, k, weight, rows);
        const quint64 trials = trialsFor(p, miss);
        const double  words  = wordsPerTrial(k, rows);
        const double  cost   = double(trials) * (words + gauss);
        if (best.rows == 0 || cost < bestCost) {
            best.rows          = rows;
            best.trials        = trials;
            best.wordsPerTrial = words;
            best.costPerTrial  = words + gauss;
            bestCost           = cost;
        }
    }
    return best;
}

// ------------------------------------------------------------- таблица

WordTable::WordTable(int wordsPerRow, int maxWeight)
    : m_words(wordsPerRow)
    , m_maxWeight(maxWeight)
    , m_table(1u << 16, 0u)
    , m_byWeight(size_t(maxWeight) + 1, 0ULL)
{
}

quint64 hashWord(const quint64* word, int wordsPerRow)
{
    quint64 h = 0x9E3779B97F4A7C15ULL;
    for (int w = 0; w < wordsPerRow; ++w) {
        h ^= word[w];
        h *= 0xFF51AFD7ED558CCDULL;
        h ^= h >> 33;
    }
    return h;
}

bool WordTable::equalAt(uint32_t index, const quint64* word) const
{
    return std::memcmp(m_store.data() + size_t(index) * m_words, word,
                       size_t(m_words) * sizeof(quint64)) == 0;
}

void WordTable::grow()
{
    std::vector<uint32_t> table(m_table.size() * 2, 0u);
    const quint64 mask = table.size() - 1;
    for (uint32_t slot : m_table) {
        if (slot == 0)
            continue;
        quint64 pos = hashWord(m_store.data() + size_t(slot - 1) * m_words, m_words) & mask;
        while (table[size_t(pos)] != 0)
            pos = (pos + 1) & mask;
        table[size_t(pos)] = slot;
    }
    m_table.swap(table);
}

bool WordTable::contains(const quint64* word) const
{
    const quint64 mask = m_table.size() - 1;
    quint64 pos = hashWord(word, m_words) & mask;
    for (;;) {
        const uint32_t slot = m_table[size_t(pos)];
        if (slot == 0)
            return false;
        if (equalAt(slot - 1, word))
            return true;
        pos = (pos + 1) & mask;
    }
}

bool WordTable::add(const quint64* word, int weight, bool countHit)
{
    const quint64 mask = m_table.size() - 1;
    quint64 pos = hashWord(word, m_words) & mask;
    for (;;) {
        const uint32_t slot = m_table[size_t(pos)];
        if (slot == 0)
            break;
        if (equalAt(slot - 1, word)) {
            if (countHit)
                ++m_hits[size_t(slot - 1)];
            return false;
        }
        pos = (pos + 1) & mask;
    }

    // Новое слово. Заполнение держится не выше половины: линейное
    // пробирование при большем начинает ходить кругами.
    if ((m_count + 1) * 2 > m_table.size()) {
        grow();
        return add(word, weight, countHit);
    }

    m_store.insert(m_store.end(), word, word + m_words);
    m_hits.push_back(countHit ? 1u : 0u);
    m_weight.push_back(uint16_t(weight));
    m_table[size_t(pos)] = uint32_t(m_count + 1);
    ++m_count;
    if (weight >= 0 && weight <= m_maxWeight)
        ++m_byWeight[size_t(weight)];
    return true;
}

quint64 WordTable::bytes() const
{
    return quint64(m_store.capacity()) * sizeof(quint64)
         + quint64(m_hits.capacity())  * sizeof(uint32_t)
         + quint64(m_weight.capacity()) * sizeof(uint16_t)
         + quint64(m_table.capacity()) * sizeof(uint32_t);
}

void WordTable::hitCounts(std::vector<quint64>& f1, std::vector<quint64>& f2) const
{
    f1.assign(size_t(m_maxWeight) + 1, 0ULL);
    f2.assign(size_t(m_maxWeight) + 1, 0ULL);
    for (quint64 i = 0; i < m_count; ++i) {
        const uint32_t hits   = m_hits[size_t(i)];
        const uint16_t weight = m_weight[size_t(i)];
        if (weight > m_maxWeight)
            continue;
        if (hits == 1u)      ++f1[weight];
        else if (hits == 2u) ++f2[weight];
    }
}

void WordTable::appendWords(std::vector<quint64>& words, std::vector<int>& weights) const
{
    words.insert(words.end(), m_store.begin(), m_store.begin() + ptrdiff_t(m_count * quint64(m_words)));
    weights.insert(weights.end(), m_weight.begin(), m_weight.begin() + ptrdiff_t(m_count));
}

std::vector<double> chaoUnseen(const std::vector<quint64>& f1, const std::vector<quint64>& f2)
{
    std::vector<double> unseen(f1.size(), 0.0);
    for (size_t w = 0; w < unseen.size(); ++w) {
        // Чао-1; при f2 = 0 — его же вариант со смещением.
        if (f2[w] > 0)
            unseen[w] = double(f1[w]) * double(f1[w]) / (2.0 * double(f2[w]));
        else if (f1[w] > 1)
            unseen[w] = double(f1[w]) * double(f1[w] - 1) / 2.0;
        else
            unseen[w] = 0.0;
    }
    return unseen;
}

std::vector<double> WordTable::unseenByWeight() const
{
    std::vector<quint64> f1, f2;
    hitCounts(f1, f2);
    return chaoUnseen(f1, f2);
}

// ---------------------------------------------------------- части

ShardedWordTable::ShardedWordTable(int wordsPerRow, int maxWeight, int shards)
    : m_words(wordsPerRow)
    , m_maxWeight(maxWeight)
{
    shards = std::max(1, shards);
    m_shards.reserve(size_t(shards));
    for (int i = 0; i < shards; ++i) {
        m_shards.emplace_back(wordsPerRow, maxWeight);
        omp_lock_t* lock = new omp_lock_t;
        omp_init_lock(lock);
        m_locks.push_back(lock);
    }
}

ShardedWordTable::~ShardedWordTable()
{
    for (void* lock : m_locks) {
        omp_destroy_lock(static_cast<omp_lock_t*>(lock));
        delete static_cast<omp_lock_t*>(lock);
    }
}

int ShardedWordTable::shardOf(const quint64* word) const
{
    // Старшие биты хеша: младшие раскладывают слова внутри части, и часть
    // не должна от них зависеть — иначе в каждой части занята лишь доля ячеек.
    return int((hashWord(word, m_words) >> 40) % quint64(m_shards.size()));
}

void ShardedWordTable::add(const quint64* word, int weight)
{
    const int j = shardOf(word);
    omp_lock_t* lock = static_cast<omp_lock_t*>(m_locks[size_t(j)]);
    omp_set_lock(lock);
    m_shards[size_t(j)].add(word, weight);
    omp_unset_lock(lock);
}

void ShardedWordTable::addBatch(const quint64* words, size_t count, bool countHits)
{
    if (count == 0)
        return;
    // Сначала номер части каждого слова, потом каждая часть проходит по
    // пачке и берёт своё: хеш считается один раз, а не по разу на часть.
    std::vector<uint8_t> shard(count);
    const int shards = int(m_shards.size());

    #pragma omp parallel
    {
        #pragma omp for schedule(static)
        for (long long i = 0; i < (long long)count; ++i)
            shard[size_t(i)] = uint8_t(shardOf(words + size_t(i) * m_words));

        #pragma omp for schedule(dynamic, 1)
        for (int j = 0; j < shards; ++j) {
            WordTable& table = m_shards[size_t(j)];
            for (size_t i = 0; i < count; ++i) {
                if (shard[i] != j)
                    continue;
                const quint64* word = words + i * m_words;
                int weight = 0;
                for (int w = 0; w < m_words; ++w) {
#ifdef _MSC_VER
                    weight += int(__popcnt64(word[w]));
#else
                    weight += __builtin_popcountll(word[w]);
#endif
                }
                table.add(word, weight, countHits);
            }
        }
    }
}

void ShardedWordTable::hitCounts(std::vector<quint64>& f1, std::vector<quint64>& f2) const
{
    f1.assign(size_t(m_maxWeight) + 1, 0ULL);
    f2.assign(size_t(m_maxWeight) + 1, 0ULL);
    std::vector<quint64> p1, p2;
    for (const WordTable& t : m_shards) {
        t.hitCounts(p1, p2);
        for (size_t w = 0; w < f1.size() && w < p1.size(); ++w) {
            f1[w] += p1[w];
            f2[w] += p2[w];
        }
    }
}

quint64 ShardedWordTable::size() const
{
    quint64 total = 0;
    for (const WordTable& t : m_shards) total += t.size();
    return total;
}

quint64 ShardedWordTable::bytes() const
{
    quint64 total = 0;
    for (const WordTable& t : m_shards) total += t.bytes();
    return total;
}

std::vector<quint64> ShardedWordTable::countByWeight() const
{
    std::vector<quint64> total(size_t(m_maxWeight) + 1, 0ULL);
    for (const WordTable& t : m_shards) {
        const std::vector<quint64>& part = t.countByWeight();
        for (size_t w = 0; w < total.size() && w < part.size(); ++w)
            total[w] += part[w];
    }
    return total;
}

void ShardedWordTable::exportWords(std::vector<quint64>& words, std::vector<int>& weights) const
{
    for (const WordTable& t : m_shards)
        t.appendWords(words, weights);
}

std::vector<double> ShardedWordTable::unseenByWeight() const
{
    std::vector<quint64> f1(size_t(m_maxWeight) + 1, 0ULL), f2(size_t(m_maxWeight) + 1, 0ULL);
    std::vector<quint64> p1, p2;
    for (const WordTable& t : m_shards) {
        t.hitCounts(p1, p2);
        for (size_t w = 0; w < f1.size(); ++w) { f1[w] += p1[w]; f2[w] += p2[w]; }
    }
    return chaoUnseen(f1, f2);
}

// Случайный индекс из [0, range): старшие 64 бита произведения — без
// деления. На видеокарте 64-битный остаток — сотни инструкций, и тасование
// одной нитью стоило как весь Гаусс блока; здесь та же формула, чтобы
// порядок столбцов совпадал с ядром бит в бит.
static inline quint64 belowRange(quint64 random, quint64 range)
{
#ifdef _MSC_VER
    return __umulh(random, range);
#else
    return quint64((unsigned __int128(random) * range) >> 64);
#endif
}

void shuffledColumns(int cols, quint64 trialIndex, std::vector<int>& order)
{
    order.resize(size_t(cols));
    std::iota(order.begin(), order.end(), 0);
    Xorshift rng{ seedFor(trialIndex) | 1ULL };
    for (int c = cols - 1; c > 0; --c)
        std::swap(order[size_t(c)], order[size_t(belowRange(rng.next(), quint64(c + 1)))]);
}

} // namespace Leon
