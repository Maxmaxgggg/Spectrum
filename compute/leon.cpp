#include "leon.h"
#include "mixing.h"

#ifdef Q_OS_WIN
    #ifndef NOMINMAX
        #define NOMINMAX   // иначе windows.h подменяет std::min/max макросами
    #endif
    #include <windows.h>
#else
    #include <unistd.h>
#endif

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

double catchProbabilityStern(int n, int k, int weight, int rows, int window)
{
    if (window <= 0)
        return catchProbability(n, k, weight, rows);
    if (n <= 0 || k <= 0 || weight <= 0 || rows <= 0 || weight > n || window > n - k)
        return 0.0;
    // Носитель веса w раскладывается по половинам (h1, h2), окну (l) и остатку
    // (n − k − l): ловится, если на половинах не больше rows и на окне ноль.
    const int h1 = k / 2, h2 = k - h1, rest = n - k - window;
    const double logTotal = logBinom(n, weight);
    double p = 0.0;
    for (int a = 0; a <= rows && a <= h1; ++a)
        for (int b = 0; b <= rows && b <= h2; ++b) {
            if (a + b == 0) continue;   // нулевое на множестве — нулевое слово
            const int r = weight - a - b;
            if (r < 0 || r > rest) continue;
            const double term = logBinom(h1, a) + logBinom(h2, b) + logBinom(rest, r) - logTotal;
            if (std::isfinite(term))
                p += std::exp(term);
        }
    return std::min(1.0, p);
}

namespace {

// log n! для n до cols: таблица на вызов, считать её дешевле, чем лог-гамму в
// шестикратном цикле.
std::vector<double> logFactorials(int n)
{
    std::vector<double> f(size_t(std::max(n, 0)) + 1, 0.0);
    for (int i = 1; i <= n; ++i)
        f[size_t(i)] = f[size_t(i - 1)] + std::log(double(i));
    return f;
}

} // namespace

double jointCatchProbability(int n, int k, int weight, int overlap, int rows, int window)
{
    if (n <= 0 || k <= 0 || weight <= 0 || rows <= 0 || overlap < 0 || overlap > weight
        || 2 * weight - overlap > n)
        return 0.0;
    const std::vector<double> lf = logFactorials(n);
    auto f = [&](int x) { return lf[size_t(x)]; };
    const int a = overlap, s = weight - overlap;   // общая часть и своя у каждого
    const int other = n - (2 * weight - overlap);  // столбцы вне обоих носителей
    double p = 0.0;

    if (window <= 0) {
        // Множество — k случайных столбцов. На нём i общих единиц, b своих у
        // первого слова и c — у второго; у каждого слова не больше rows.
        const double logTotal = f(n) - f(k) - f(n - k);
        for (int i = 0; i <= rows && i <= a; ++i)
            for (int b = 0; b + i <= rows && b <= s; ++b)
                for (int c = 0; c + i <= rows && c <= s; ++c) {
                    const int rest = k - i - b - c;
                    if (rest < 0 || rest > other) continue;
                    const double t = (f(a) - f(i) - f(a - i)) + (f(s) - f(b) - f(s - b))
                                   + (f(s) - f(c) - f(s - c)) + (f(other) - f(rest) - f(other - rest))
                                   - logTotal;
                    p += std::exp(t);
                }
        return std::min(1.0, p);
    }

    if (window > n - k)
        return 0.0;
    // Окно Штерна–Дюмера: половины h1, h2, окно l и остаток — случайное
    // разбиение столбцов. Слово ловится, если на каждой половине у него не
    // больше rows единиц, а на окне ни одной. Общие единицы раскладываются по
    // половинам (i1, i2), свои у первого слова — (b1, b2), у второго — (c1, c2).
    const int h1 = k / 2, h2 = k - h1, l = window, r = n - k - l;
    const double logTotal = f(n) - f(h1) - f(h2) - f(l) - f(r);
    auto split = [&](int total, int x, int y) {   // total! / (x! y! (total−x−y)!)
        return f(total) - f(x) - f(y) - f(total - x - y);
    };
    for (int i1 = 0; i1 <= rows && i1 <= a; ++i1)
        for (int i2 = 0; i2 <= rows && i1 + i2 <= a; ++i2)
            for (int b1 = 0; b1 + i1 <= rows && b1 <= s; ++b1)
                for (int b2 = 0; b2 + i2 <= rows && b1 + b2 <= s; ++b2)
                    for (int c1 = 0; c1 + i1 <= rows && c1 <= s; ++c1)
                        for (int c2 = 0; c2 + i2 <= rows && c1 + c2 <= s; ++c2) {
                            const int x1 = h1 - i1 - b1 - c1, x2 = h2 - i2 - b2 - c2;
                            const int y  = other - x1 - x2 - l;   // остаток вне носителей
                            if (x1 < 0 || x2 < 0 || y < 0) continue;
                            const double t = split(a, i1, i2) + split(s, b1, b2) + split(s, c1, c2)
                                           + (f(other) - f(x1) - f(x2) - f(l) - f(y))
                                           - logTotal;
                            p += std::exp(t);
                        }
    return std::min(1.0, p);
}

double orbitCatchProbability(int n, int k, int weight, int rows, int window,
                             const Cyclic::Symmetry& symmetry)
{
    const double single = catchProbabilityFor(n, k, weight, rows, window);
    if (!symmetry.active() || single <= 0.0 || weight <= 1)
        return single;

    // Совместные вероятности по числу общих единиц a = 0..weight−1 и их
    // верхняя вогнутая оболочка: средняя по парам сдвигов оценивается сверху
    // оболочкой в средней точке (неравенство Йенсена).
    std::vector<double> joint(static_cast<size_t>(weight));
    for (int a = 0; a < weight; ++a)
        joint[size_t(a)] = jointCatchProbability(n, k, weight, a, rows, window);
    std::vector<int> hull;   // вершины оболочки, по возрастанию a
    for (int a = 0; a < weight; ++a) {
        while (hull.size() >= 2) {
            const int a1 = hull[hull.size() - 2], a2 = hull.back();
            // a2 под отрезком (a1, a) — не вершина.
            const double cross = (joint[size_t(a2)] - joint[size_t(a1)]) * double(a - a1)
                               - (joint[size_t(a)] - joint[size_t(a1)]) * double(a2 - a1);
            if (cross <= 0.0) hull.pop_back();
            else break;
        }
        hull.push_back(a);
    }
    auto envelope = [&](double x) {
        x = std::max(0.0, std::min(x, double(weight - 1)));
        for (size_t i = 1; i < hull.size(); ++i)
            if (x <= double(hull[i])) {
                const int a1 = hull[i - 1], a2 = hull[i];
                const double t = (x - double(a1)) / double(a2 - a1);
                return joint[size_t(a1)] + t * (joint[size_t(a2)] - joint[size_t(a1)]);
            }
        return joint[size_t(hull.back())];
    };

    // Худший случай по всему, чего мы о слове не знаем: стоит ли единица на
    // столбцах вне круга (их не больше одного) и какой у слова период.
    const int L = symmetry.length, fixed = n - L;
    double worst = 1.0;
    for (int e = 0; e <= fixed && e <= weight; ++e) {
        const int onCircle = weight - e;
        for (int orbit : Cyclic::orbitSizes(symmetry, onCircle)) {
            if (orbit <= 1) {
                worst = std::min(worst, single);
                continue;
            }
            // Пересечения разных сдвигов: на круге их сумма по сдвигам 1..p−1
            // равна w(wp − L)/L (у слова периода p единиц на периоде поровну),
            // и к каждому прибавляются общие единицы вне круга.
            const double sumOnCircle = double(onCircle) * (double(onCircle) * orbit - L) / double(L);
            const double mean = sumOnCircle / double(orbit - 1) + double(e);
            const double pairs = envelope(mean);
            // Чжун–Эрдёш: P(хоть одно) >= (Σ P_i)^2 / (Σ P_i + Σ_{i≠j} P_ij).
            const double sum = double(orbit) * single;
            const double bound = sum * sum / (sum + double(orbit) * double(orbit - 1) * pairs);
            worst = std::min(worst, bound);
        }
    }
    return std::max(single, std::min(1.0, worst));
}

std::vector<double> catchProbabilities(int n, int k, int maxWeight, int rows, int window,
                                       const Cyclic::Symmetry& symmetry)
{
    std::vector<double> p(size_t(std::max(maxWeight, 0)) + 1, 0.0);
    for (int w = 1; w <= maxWeight && w <= n; ++w)
        p[size_t(w)] = orbitCatchProbability(n, k, w, rows, window, symmetry);
    return p;
}

double sternListSize(int half, int rows)
{
    double total = 1.0, term = 1.0;
    for (int i = 1; i <= rows && i <= half; ++i) {
        term = term * double(half - i + 1) / double(i);
        total += term;
    }
    return total;
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

double expectedWordsUpTo(int n, int k, int weight)
{
    if (n <= 0 || k <= 0 || weight <= 0)
        return 0.0;
    const double redundancy = double(n - k) * std::log(2.0);
    double total = 0.0;
    for (int w = 1; w <= weight && w <= n; ++w) {
        const double t = logBinom(n, w) - redundancy;
        if (std::isfinite(t))
            total += std::exp(t);
    }
    return total;
}

double tableBytesPerWord(int wordsPerRow)
{
    // Слово, поимки (4), вес (2) — векторы растут с запасом; индекс — от
    // двух до четырёх ячеек по 4 байта на слово (заполнение не выше 0,5).
    return 1.5 * (8.0 * std::max(1, wordsPerRow) + 6.0) + 12.0;
}

int maxWeightForMemory(int n, int k, quint64 limitBytes)
{
    if (n <= 0)
        return 1;
    const double perWord = tableBytesPerWord((n + 63) / 64);
    int best = 1;
    for (int w = 1; w <= n; ++w) {
        if (expectedWordsUpTo(n, k, w) * perWord > double(limitBytes))
            break;
        best = w;
    }
    return best;
}

quint64 physicalMemoryBytes()
{
#ifdef Q_OS_WIN
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status))
        return quint64(status.ullTotalPhys);
    return 0;
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long size  = sysconf(_SC_PAGE_SIZE);
    return pages > 0 && size > 0 ? quint64(pages) * quint64(size) : 0;
#endif
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

quint64 trialsForAll(int n, int k, int weight, int rows, int window, double miss,
                     const std::vector<quint64>& foundByWeight)
{
    return trialsForAll(catchProbabilities(n, k, weight, rows, window, Cyclic::Symmetry()),
                        miss, foundByWeight);
}

quint64 trialsForAll(const std::vector<double>& catchByWeight, double miss,
                     const std::vector<quint64>& foundByWeight)
{
    quint64 needed = 1;
    for (size_t w = 1; w < catchByWeight.size(); ++w) {
        const quint64 found = w < foundByWeight.size() ? foundByWeight[w] : 0;
        const double  each  = miss / double(std::max<quint64>(1, found));
        needed = std::max(needed, trialsFor(catchByWeight[w], each));
    }
    return needed;
}

quint64 trialsForAll(int n, int k, int weight, int rows, double miss,
                     const std::vector<quint64>& foundByWeight)
{
    return trialsForAll(n, k, weight, rows, 0, miss, foundByWeight);
}

SternProfile sternProfile(const QStringList& matrix)
{
    SternProfile profile;
    const int k = matrix.size();
    // Строки в записи списка — по 16 бит (см. trialStern).
    if (k == 0 || k > 0xFFFF)
        return profile;
    const int n = matrix.first().length();
    int words = 0;
    const std::vector<quint64> packed = InfoSets::packRows(matrix, words);
    std::vector<int> order;
    shuffledColumns(n, 0, order);
    InfoSets::InfoSet set;
    if (!InfoSets::systematize(packed.data(), k, n, words, order, nullptr, set))
        return profile;
    std::vector<char> pivot;
    std::vector<int>  win;
    sternWindow(order, set, n, MAX_STERN_WINDOW, pivot, win);
    const int lw = int(win.size());
    if (lw == 0)
        return profile;
    profile.window = lw;

    // Ключи комбинаций каждой половины на самом широком окне; окно уже —
    // те же ключи без старших битов. Как в trialStern: пустая комбинация в
    // списке есть, и пара «пусто, пусто» из счёта вычитается.
    std::vector<uint32_t> key(static_cast<size_t>(k), 0u);
    for (int i = 0; i < k; ++i)
        key[size_t(i)] = sternKey(set.rows.data() + size_t(i) * words, win);
    const int h1 = k / 2;
    auto list = [&](int from, int to, int rows) {
        std::vector<uint32_t> keys;
        keys.push_back(0u);
        for (int a = from; a < to; ++a) {
            keys.push_back(key[size_t(a)]);
            if (rows < 2) continue;
            for (int b = a + 1; b < to; ++b)
                keys.push_back(key[size_t(a)] ^ key[size_t(b)]);
        }
        return keys;
    };
    std::vector<uint32_t> hist;
    for (int rows = 1; rows <= 2; ++rows) {
        const std::vector<uint32_t> a = list(0, h1, rows), b = list(h1, k, rows);
        for (int l = 1; l <= lw; ++l) {
            const uint32_t mask = (1u << l) - 1u;
            hist.assign(size_t(1) << l, 0u);
            for (uint32_t x : a) ++hist[x & mask];
            double pairs = -1.0;
            for (uint32_t x : b) pairs += double(hist[x & mask]);
            profile.pairs[rows][l] = std::max(0.0, pairs);
        }
    }
    return profile;
}

Plan plan(int n, int k, int weight, double miss, bool gpu, const SternProfile* profile,
          WindowPolicy window, const Cyclic::Symmetry& symmetry)
{
    Plan best;
    double bestCost = 0.0;
    const double gauss = gaussCostInWords(k, (n + 63) / 64, gpu);
    // Глубже четырёх строк за попытку не имеет смысла: столько уже дешевле
    // отдать Брауэру–Циммерману. Глубже k не бывает.
    for (int rows = 1; rows <= 4 && rows <= k; ++rows) {
        const double  p      = orbitCatchProbability(n, k, weight, rows, 0, symmetry);
        const quint64 trials = trialsFor(p, miss);
        const double  words  = wordsPerTrial(k, rows);
        const double  cost   = double(trials) * (words + gauss);
        if (best.rows == 0 || cost < bestCost) {
            best.rows          = rows;
            best.window        = 0;
            best.trials        = trials;
            best.wordsPerTrial = words;
            best.costPerTrial  = words + gauss;
            bestCost           = cost;
        }
    }
    if (!(gpu ? window.gpu : window.cpu) || !profile || profile->window <= 0)
        return best;

    // Окно Штерна–Дюмера (только процессор). Замер одного потока (мкс на
    // попытку, --stern-bench, строки по 16 слов): сумма перебора стоит
    // words операций над словами (≈1,75 нс каждая); запись списка — от 10
    // до 35 нс, смотря помещаются ли списки в кэш ядра; пара — words + 10
    // (сумма четырёх строк читается сплошь, 15–50 нс); корзина сортировки —
    // 0,5–1,5 нс. Модель окну не льстит: на [961,676] она даёт попытке
    // 9 мс против измеренных 6, на случайной [1000,500] — 2,5 против 0,8.
    // Списки только до p = 2: при p = 3 они в миллионы записей и в
    // несколько потоков попытка дорожает в разы против одного.
    const int    words   = std::max(1, (n + 63) / 64);
    const double h1      = k / 2, h2 = k - k / 2;
    // Окно берётся, только если по модели выигрывает хотя бы в cutover раз.
    // На деле выигрыш меньше модельного: рабочий набор попытки с окном —
    // мегабайты на поток против десятков килобайт у перебора, и в
    // несколько потоков она дорожает сильнее; а у кода с тьмой лёгких слов
    // (разреженная [1000,500], 120 тысяч слов веса до 12) почти каждая
    // пара — лёгкое слово, и цена попытки уходит в таблицу найденных, чего
    // модель не знает. Замеры в 16 потоков: [961,676] (Хэмминг [31,26]²)
    // до веса 9 — модель 7,4 раза, на деле 4,6; до веса 12 — 11 и 7,5;
    // разреженная [1000,500] до веса 12 — модель 2, на деле проигрыш 2,6.
    const double cutover = 2.5;
    const double gaussOps = gauss * words;
    const double bestOps  = bestCost * words;   // перебор: слова × длина строки
    double bestWindowOps  = 0.0;
    for (int rows = 1; rows <= 2 && rows <= int(h1); ++rows) {
        const double listA = sternListSize(int(h1), rows), listB = sternListSize(int(h2), rows);
        for (int window = 1; window <= profile->window && window < n - k; ++window) {
            const double p = orbitCatchProbability(n, k, weight, rows, window, symmetry);
            if (p <= 0.0) continue;
            const quint64 trials = trialsFor(p, miss);
            const double  pairs  = profile->pairs[rows][window];
            const double  ops    = 16.0 * (listA + listB) + pairs * (words + 10.0)
                                 + 0.8 * std::ldexp(1.0, window);
            const double  cost   = double(trials) * (ops + gaussOps);
            if (cost * cutover < bestOps && (best.window == 0 || cost < bestWindowOps)) {
                best.rows          = rows;
                best.window        = window;
                best.trials        = trials;
                // В «словах» перебора, чтобы скорость и ETA считались как обычно.
                best.wordsPerTrial = ops / words;
                best.costPerTrial  = (ops + gaussOps) / words;
                bestWindowOps      = cost;
            }
        }
    }
    return best;
}

// ------------------------------------------------------------- таблица

WordTable::WordTable(int wordsPerRow, int maxWeight, const Cyclic::Symmetry& symmetry)
    : m_words(wordsPerRow)
    , m_maxWeight(maxWeight)
    , m_symmetry(symmetry)
    , m_table(1u << 16, 0u)
    , m_byWeight(size_t(maxWeight) + 1, 0ULL)
    , m_entriesByWeight(size_t(maxWeight) + 1, 0ULL)
{
}

quint64 hashWord(const quint64* word, int wordsPerRow)
{
    quint64 h = Mixing::wordHashSeed();
    for (int w = 0; w < wordsPerRow; ++w)
        h = Mixing::wordHashStep(h, word[w]);
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
    std::vector<quint64> canonical(static_cast<size_t>(m_words));
    if (m_symmetry.active()) {
        Cyclic::canonical(word, m_words, m_symmetry, canonical.data());
        word = canonical.data();
    }
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
    if (!m_symmetry.active())
        return addCanonical(word, weight, 1, countHit);
    std::vector<quint64> canonical(static_cast<size_t>(m_words));
    const int orbit = Cyclic::canonical(word, m_words, m_symmetry, canonical.data());
    return addCanonical(canonical.data(), weight, orbit, countHit);
}

bool WordTable::addCanonical(const quint64* word, int weight, int orbit, bool countHit)
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
        return addCanonical(word, weight, orbit, countHit);
    }

    m_store.insert(m_store.end(), word, word + m_words);
    m_hits.push_back(countHit ? 1u : 0u);
    m_weight.push_back(uint16_t(weight));
    if (m_symmetry.active())
        m_orbit.push_back(uint16_t(orbit));
    m_table[size_t(pos)] = uint32_t(m_count + 1);
    ++m_count;
    if (weight >= 0 && weight <= m_maxWeight) {
        m_byWeight[size_t(weight)] += quint64(orbit);
        ++m_entriesByWeight[size_t(weight)];
    }
    return true;
}

quint64 WordTable::bytes() const
{
    return quint64(m_store.capacity()) * sizeof(quint64)
         + quint64(m_hits.capacity())  * sizeof(uint32_t)
         + quint64(m_weight.capacity()) * sizeof(uint16_t)
         + quint64(m_orbit.capacity()) * sizeof(uint16_t)
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
    if (!m_symmetry.active()) {
        words.insert(words.end(), m_store.begin(), m_store.begin() + ptrdiff_t(m_count * quint64(m_words)));
        weights.insert(weights.end(), m_weight.begin(), m_weight.begin() + ptrdiff_t(m_count));
        return;
    }
    // Орбита разворачивается в слова: сдвиги на 0..p−1, где p — её размер,
    // все различны и других нет.
    std::vector<quint64> turned(static_cast<size_t>(m_words));
    for (quint64 i = 0; i < m_count; ++i) {
        const quint64* word = m_store.data() + size_t(i) * m_words;
        for (int shift = 0; shift < int(m_orbit[size_t(i)]); ++shift) {
            Cyclic::rotate(word, m_words, m_symmetry, shift, turned.data());
            words.insert(words.end(), turned.begin(), turned.end());
            weights.push_back(int(m_weight[size_t(i)]));
        }
    }
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

ShardedWordTable::ShardedWordTable(int wordsPerRow, int maxWeight, int shards,
                                   const Cyclic::Symmetry& symmetry)
    : m_words(wordsPerRow)
    , m_maxWeight(maxWeight)
    , m_symmetry(symmetry)
{
    shards = std::max(1, shards);
    m_shards.reserve(size_t(shards));
    for (int i = 0; i < shards; ++i) {
        m_shards.emplace_back(wordsPerRow, maxWeight, symmetry);
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
    // Часть выбирается по представителю орбиты: все её слова — в одну часть.
    // Приводится до замка — это самая дорогая часть добавления.
    quint64 canonical[64];
    const quint64* key = word;
    int orbit = 1;
    if (m_symmetry.active()) {
        orbit = Cyclic::canonical(word, m_words, m_symmetry, canonical);
        key   = canonical;
    }
    const int j = shardOf(key);
    omp_lock_t* lock = static_cast<omp_lock_t*>(m_locks[size_t(j)]);
    omp_set_lock(lock);
    m_shards[size_t(j)].addCanonical(key, weight, orbit);
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
        // У циклического кода часть — по представителю орбиты. Он считается
        // дважды (здесь и при добавлении), зато без копии пачки: она бывает
        // в миллионы слов.
        quint64 canonical[64];
        const bool cyclic = m_symmetry.active();
        #pragma omp for schedule(static)
        for (long long i = 0; i < (long long)count; ++i) {
            const quint64* word = words + size_t(i) * m_words;
            if (cyclic) {
                Cyclic::canonical(word, m_words, m_symmetry, canonical);
                word = canonical;
            }
            shard[size_t(i)] = uint8_t(shardOf(word));
        }

        #pragma omp for schedule(dynamic, 1)
        for (int j = 0; j < shards; ++j) {
            WordTable& table = m_shards[size_t(j)];
            for (size_t i = 0; i < count; ++i) {
                if (shard[i] != j)
                    continue;
                const quint64* word = words + i * m_words;
                int weight = 0;
                for (int w = 0; w < m_words; ++w)
                    weight += BitOps::popcount64(word[w]);
                if (cyclic) {
                    const int orbit = Cyclic::canonical(word, m_words, m_symmetry, canonical);
                    table.addCanonical(canonical, weight, orbit, countHits);
                }
                else
                    table.addCanonical(word, weight, 1, countHits);
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

std::vector<quint64> ShardedWordTable::entriesByWeight() const
{
    std::vector<quint64> total(size_t(m_maxWeight) + 1, 0ULL);
    for (const WordTable& t : m_shards) {
        const std::vector<quint64>& part = t.entriesByWeight();
        for (size_t w = 0; w < total.size() && w < part.size(); ++w)
            total[w] += part[w];
    }
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

// Порядок столбцов — тот же, что у ядра (Mixing::shuffleForTrial): попытка с
// одним номером даёт одно и то же множество на CPU и на GPU.
void shuffledColumns(int cols, quint64 trialIndex, std::vector<int>& order)
{
    order.resize(size_t(cols));
    std::iota(order.begin(), order.end(), 0);
    Mixing::shuffleForTrial(order.data(), cols, trialIndex);
}

} // namespace Leon
