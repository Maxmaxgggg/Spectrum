#pragma once

// Случайный поиск лёгких слов по случайным информационным множествам (Леон,
// Канто–Шабо; в криптографии тот же приём зовут ISD).
//
// У Брауэра–Циммермана множества фиксированы, и перебор в них идёт глубоко.
// Здесь наоборот: множеств тысячи, случайных, а перебор в каждом мелкий —
// две-три строки. Слово веса w на случайных k столбцах из n несёт в среднем
// w·k/n единиц, но иногда — мало, и тогда его ловит мелкий перебор. Каждая
// попытка ловит слово веса w с вероятностью P_w (гипергеометрическое
// распределение), и после T попыток слово пропущено с вероятностью (1-P_w)^T.
//
// Гарантии это не даёт, но даёт число: «все слова до веса W найдены, если не
// случилось события вероятности 10^-9». Слова помнятся в таблице, чтобы не
// считать дважды; там же считается, сколько раз каждое поймано — по словам,
// пойманным один и два раза, оценивается, сколько ещё не найдено (Чао).
//
// Предел метода — память, а не время: всё найденное надо хранить.

#include "bitops.h"
#include "infosets.h"

#include <QStringList>
#include <QtGlobal>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Leon {

// Вероятность, что слово веса weight имеет не больше rows единиц на случайном
// информационном множестве кода [n, k] — гипергеометрический хвост.
double catchProbability(int n, int k, int weight, int rows);

// Окно Штерна–Дюмера. Информационное множество делится на две половины
// (⌊k/2⌋ и ⌈k/2⌉ столбцов), из проверочных столбцов берётся окно из window
// штук; попытка перебирает пары «до rows строк первой половины» × «до rows
// строк второй» с одинаковой суммой на окне. Ловится слово, у которого на
// каждой половине не больше rows единиц, а на окне — ни одной. Списков две
// по Σ C(k/2, i), совпадений ≈ (размер списка)² / 2^window — против C(k, 2·rows)
// сумм у перебора той же глубины. Вероятность поимки по той же
// гипергеометрической модели: раскладка носителя по половинам, окну и
// остатку. window == 0 — окна нет, это catchProbability(rows).
double catchProbabilityStern(int n, int k, int weight, int rows, int window);

// Размер списка одной половины: Σ C(h, i) по i = 0..rows (с пустой комбинацией).
double sternListSize(int half, int rows);

// Самое широкое окно: корзин сортировки 2^l у каждого потока, при l = 20
// это 4 МБ; шире и не нужно — списки до p = 2 короче 2^20 и при k = 2000.
constexpr int MAX_STERN_WINDOW = 20;

// Сколько пар списков совпадёт по ключу за попытку: pairs[p][l] для p строк
// на половину (1..2) и окна l (1..MAX_STERN_WINDOW), измерено на
// систематической матрице попытки 0. У случайной матрицы это L₁·L₂/2^l, у
// разреженной ключи скошены — многие строки на окне нулевые — и пар в
// сотни раз больше; по замеру планировщик такое окно и отвергает.
// window — ширина измеренного окна (меньше MAX_STERN_WINDOW, если
// проверочных столбцов не хватило); 0 — не измерено (строки зависимы).
struct SternProfile
{
    int    window = 0;
    double pairs[3][MAX_STERN_WINDOW + 1] = {};
};
SternProfile sternProfile(const QStringList& matrix);

// Сколько слов веса не выше weight ожидать у кода [n, k]: как у случайного
// кода, C(n, w) / 2^(n-k) на вес. У кода со структурой лёгких слов больше,
// чем у случайного (у произведения 336x96 их миллионы там, где случайному
// положено ноль), так что это оценка снизу — годится, чтобы отсечь веса,
// при которых таблица не поместилась бы даже у случайного кода.
double expectedWordsUpTo(int n, int k, int weight);

// Байт на слово в таблице найденных: само слово, поимки, вес, индекс, с
// запасом на рост векторов.
double tableBytesPerWord(int wordsPerRow);

// Наибольший вес, при котором таблица ожидаемо помещается в limitBytes;
// не меньше единицы и не больше n.
int maxWeightForMemory(int n, int k, quint64 limitBytes);

// Физическая память машины, байт; ноль — не узнать.
quint64 physicalMemoryBytes();

// Сколько слов перебирается за одну попытку: sum C(k, i) по i = 1..rows.
double wordsPerTrial(int k, int rows);

// Число попыток, после которого слово с такой вероятностью поимки пропущено
// не чаще, чем с вероятностью miss.
quint64 trialsFor(double catchProbability, double miss);

// Цена попытки помимо перебора — приведение матрицы к систематическому виду,
// в тех же единицах, что и слова перебора. Замерено: на 96x336 один Гаусс на
// процессоре стоит примерно столько же, сколько шесть тысяч слов, то есть
// k*k*words/8. На видеокарте Гаусс идёт блоком с барьером на каждый опорный
// столбец и относительно перебора обходится дороже.
double gaussCostInWords(int k, int wordsPerRow, bool gpu);

// Попыток, чтобы ни одно слово веса до weight не осталось непойманным с
// вероятностью больше miss — уже с учётом того, сколько слов каждого веса
// найдено: у веса с тысячей слов шансов на пропуск в тысячу раз больше, чем
// у одного слова. Пока слов не найдено, считается на одно слово.
quint64 trialsForAll(int n, int k, int weight, int rows, double miss,
                     const std::vector<quint64>& foundByWeight);
// То же для попытки с окном Штерна–Дюмера (window == 0 — без окна).
quint64 trialsForAll(int n, int k, int weight, int rows, int window, double miss,
                     const std::vector<quint64>& foundByWeight);

// План поиска: глубина перебора в попытке и число попыток, при которых слово
// веса weight пропускается с вероятностью не больше miss. Глубина выбирается
// по цене: попыток тем меньше, чем глубже перебор, но каждая дороже.
struct Plan
{
    int     rows          = 0;
    int     window        = 0;     // окно Штерна–Дюмера; 0 — обычный перебор
    quint64 trials        = 0;
    double  wordsPerTrial = 0.0;   // слов перебирается за попытку (у окна — списки и совпадения)
    double  costPerTrial  = 0.0;   // то же плюс цена Гаусса — для прогресса
};
// Где плану можно брать окно Штерна–Дюмера.
//
// По умолчанию — только на процессоре. Ядро видеокарты окно умеет
// (хеш-таблица блока в глобальной памяти), но цена попытки там ещё не
// откалибрована, а Гаусс относительно перебора дорог, и выигрыш окна под
// вопросом — пока по запросу. Выключить окно и на процессоре нужно тестам:
// они сравнивают CPU и GPU слово в слово.
//
// Раньше это были два глобальных флага, которые тесты переключали вокруг
// вызовов, — теперь параметр плана и настройка Worker.
struct WindowPolicy
{
    bool cpu = true;
    bool gpu = false;

    static WindowPolicy none() { return WindowPolicy{ false, false }; }
};

// Без профиля ключей (sternProfile) окно не рассматривается: цена попытки
// с окном зависит от матрицы, а не только от размеров.
Plan plan(int n, int k, int weight, double miss, bool gpu = false,
          const SternProfile* profile = nullptr, WindowPolicy window = WindowPolicy());

// Вероятность поимки по плану: с окном или без.
inline double catchProbabilityFor(int n, int k, int weight, int rows, int window)
{
    return window > 0 ? catchProbabilityStern(n, k, weight, rows, window)
                      : catchProbability(n, k, weight, rows);
}

// Таблица найденных слов: само слово, вес и сколько раз поймано.
// Не потокобезопасна — добавления серийные.
class WordTable
{
public:
    WordTable(int wordsPerRow, int maxWeight);

    // true — слово новое. countHit = false — поимка не засчитывается: её
    // считает таблица на видеокарте, а здесь слово только хранится (поимок
    // у него 0, и в оценку Чао оно отсюда не попадает).
    bool add(const quint64* word, int weight, bool countHit = true);
    // Есть ли слово в таблице; поимки не считает.
    bool contains(const quint64* word) const;

    quint64 size()  const { return m_count; }
    quint64 bytes() const;

    // Найдено слов каждого веса, индекс — вес.
    const std::vector<quint64>& countByWeight() const { return m_byWeight; }

    // Оценка Чао: сколько слов каждого веса ещё не найдено. По числу слов,
    // пойманных ровно один (f1) и ровно два (f2) раза: f1^2 / (2 f2). Это
    // нижняя оценка — у слов разная вероятность поимки.
    std::vector<double> unseenByWeight() const;

    // Слова, пойманные ровно один (f1) и ровно два (f2) раза, по весам —
    // сырьё для оценки Чао, когда таблица разбита на части.
    void hitCounts(std::vector<quint64>& f1, std::vector<quint64>& f2) const;

    // Все слова подряд (по wordsPerRow слов) и их веса — дописываются в конец.
    void appendWords(std::vector<quint64>& words, std::vector<int>& weights) const;

private:
    bool    equalAt(uint32_t index, const quint64* word) const;
    void    grow();

    int m_words;
    int m_maxWeight;

    std::vector<quint64>  m_store;    // слова подряд, по m_words каждое
    std::vector<uint32_t> m_hits;     // поимок у слова
    std::vector<uint16_t> m_weight;   // вес слова
    std::vector<uint32_t> m_table;    // индекс слова + 1; 0 — пусто
    std::vector<quint64>  m_byWeight;
    quint64               m_count = 0;
};

// Хеш слова — общий для таблицы и для раскладки по частям.
quint64 hashWord(const quint64* word, int wordsPerRow);

// Оценка Чао по f1 и f2: f1^2 / (2 f2), при f2 = 0 — f1 (f1 - 1) / 2.
std::vector<double> chaoUnseen(const std::vector<quint64>& f1, const std::vector<quint64>& f2);

// Таблица из нескольких независимых частей, по хешу слова. Части наполняются
// параллельно, каждая своим потоком: так хост успевает разбирать находки
// видеокарты, а потоки CPU-пути не толкаются на одном замке.
class ShardedWordTable
{
public:
    ShardedWordTable(int wordsPerRow, int maxWeight, int shards);
    ~ShardedWordTable();
    ShardedWordTable(const ShardedWordTable&)            = delete;
    ShardedWordTable& operator=(const ShardedWordTable&) = delete;

    // Одно слово, под замком своей части. Можно звать из разных потоков.
    void add(const quint64* word, int weight);
    // Пачка слов подряд, по wordsPerRow каждое: разбирается всеми потоками
    // OpenMP сразу, вес считается здесь. countHits — см. WordTable::add.
    void addBatch(const quint64* words, size_t count, bool countHits = true);

    quint64 size()  const;
    quint64 bytes() const;
    std::vector<quint64> countByWeight()  const;
    std::vector<double>  unseenByWeight() const;
    // f1 и f2 по весам суммарно по частям — для оценки Чао, когда часть
    // поимок посчитана в другом месте.
    void hitCounts(std::vector<quint64>& f1, std::vector<quint64>& f2) const;
    // Все найденные слова и их веса — для тех, кому нужны сами слова, а не
    // только счёт: код произведения строит из них наборы.
    void exportWords(std::vector<quint64>& words, std::vector<int>& weights) const;

private:
    int shardOf(const quint64* word) const;

    int m_words;
    int m_maxWeight;
    std::vector<WordTable> m_shards;
    std::vector<void*>     m_locks;   // omp_lock_t, без заголовка OpenMP здесь
};

// Случайный порядок столбцов для попытки с этим номером. Детерминирован:
// один и тот же номер даёт один и тот же порядок на любой машине, поэтому
// поиск воспроизводим.
void shuffledColumns(int cols, quint64 trialIndex, std::vector<int>& order);

// Одна попытка: случайное информационное множество и перебор комбинаций до
// rowsPerTrial строк его систематической матрицы. Для каждого ненулевого
// слова веса не больше maxWeight зовётся visit(word, weight).
// false — строки матрицы зависимы.
template <class Visit>
bool trial(const quint64* matrix, int rows, int cols, int wordsPerRow,
           int rowsPerTrial, int maxWeight, quint64 trialIndex, Visit&& visit)
{
    std::vector<int> order;
    shuffledColumns(cols, trialIndex, order);

    InfoSets::InfoSet set;
    if (!InfoSets::systematize(matrix, rows, cols, wordsPerRow, order, nullptr, set))
        return false;

    const quint64* g = set.rows.data();
    std::vector<quint64> word(size_t(wordsPerRow), 0ULL);

    // Рекурсия по глубине: каждая промежуточная сумма — тоже кодовое слово,
    // так что комбинации из 1..rowsPerTrial строк обходятся одним проходом.
    struct Walker
    {
        const quint64* g; int rows; int words; int depthMax; int maxWeight;
        std::vector<quint64>& word; Visit& visit;

        void go(int from, int depth)
        {
            for (int i = from; i < rows; ++i) {
                const quint64* row = g + size_t(i) * words;
                int weight = 0;
                for (int w = 0; w < words; ++w) {
                    word[size_t(w)] ^= row[w];
                    weight += BitOps::popcount64(word[size_t(w)]);
                }
                if (weight > 0 && weight <= maxWeight)
                    visit(word.data(), weight);
                if (depth + 1 < depthMax)
                    go(i + 1, depth + 1);
                for (int w = 0; w < words; ++w)
                    word[size_t(w)] ^= row[w];
            }
        }
    };
    Walker walker{ g, rows, wordsPerRow, rowsPerTrial, maxWeight, word, visit };
    walker.go(0, 0);
    return true;
}

// Окно попытки: последние l неопорных столбцов случайного порядка. Не
// первые: у разреженной матрицы столбец пропускается при выборе опорных
// тогда, когда в ещё не приведённых строках он пуст, так что первые
// пропущенные столбцы почти нулевые и во всех строках, ключи по ним
// совпадают у всех, и списки сравниваются попарно целиком.
inline void sternWindow(const std::vector<int>& order, const InfoSets::InfoSet& set, int cols,
                        int window, std::vector<char>& pivot, std::vector<int>& win)
{
    pivot.assign(static_cast<size_t>(cols), 0);
    for (int c : set.columns) pivot[size_t(c)] = 1;
    win.clear();
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        if (int(win.size()) >= window) break;
        if (!pivot[size_t(*it)]) win.push_back(*it);
    }
}

// Ключ строки: её биты на столбцах окна, младший — первый столбец.
inline uint32_t sternKey(const quint64* row, const std::vector<int>& win)
{
    uint32_t key = 0;
    for (size_t b = 0; b < win.size(); ++b)
        key |= uint32_t((row[win[b] >> 6] >> (win[b] & 63)) & 1ULL) << b;
    return key;
}

// Попытка с окном Штерна–Дюмера (см. catchProbabilityStern). Множество и
// окно — по тому же случайному порядку столбцов, что и у trial(): опорные
// столбцы — первые k независимых, окно — sternWindow. Половины — строки
// [0, k/2) и [k/2, k) приведённой матрицы. Список первой половины
// сортируется подсчётом по ключу на окне, комбинации второй ищут в нём
// совпадения; пара «пусто, пусто» — нулевое слово, пропускается.
// false — строки матрицы зависимы.
// Рабочие буферы попытки с окном — свои у каждого потока, чтобы не
// выделять память на каждой попытке: списки при p = 2 — десятки тысяч
// записей, корзин 2^l.
struct SternScratch
{
    std::vector<int>      order;
    InfoSets::InfoSet     set;
    std::vector<char>     pivot;
    std::vector<int>      win;
    std::vector<uint32_t> key;
    std::vector<uint32_t> aKey;      // ключ записи списка первой половины
    std::vector<uint32_t> aRows;     // её строки: две по 16 бит, 0xFFFF — пусто
    std::vector<uint32_t> start;     // начало корзины ключа в sortedRows
    std::vector<uint32_t> fill;
    std::vector<uint32_t> sortedRows;
    std::vector<quint64>  zero;
    std::vector<quint64>  word;
};

template <class Visit>
bool trialStern(const quint64* matrix, int rows, int cols, int wordsPerRow,
                int rowsPerTrial, int window, int maxWeight, quint64 trialIndex, Visit&& visit)
{
    static thread_local SternScratch sc;
    std::vector<int>& order = sc.order;
    shuffledColumns(cols, trialIndex, order);

    InfoSets::InfoSet& set = sc.set;
    if (!InfoSets::systematize(matrix, rows, cols, wordsPerRow, order, nullptr, set))
        return false;
    const quint64* g = set.rows.data();
    const int words  = wordsPerRow;
    const int k      = rows;
    const int p      = std::min(rowsPerTrial, 2);
    const int l      = std::min(window, MAX_STERN_WINDOW);
    if (k > 0xFFFF)   // строки в записи списка — по 16 бит; sternProfile такое окно не даёт
        return trial(matrix, rows, cols, wordsPerRow, rowsPerTrial, maxWeight, trialIndex, visit);

    std::vector<int>& win = sc.win;
    sternWindow(order, set, cols, l, sc.pivot, win);
    const int lw = int(win.size());
    std::vector<uint32_t>& key = sc.key;
    key.assign(static_cast<size_t>(k), 0u);
    for (int i = 0; i < k; ++i) key[size_t(i)] = sternKey(g + size_t(i) * words, win);

    // Список первой половины: комбинации до p строк.
    const uint32_t none = 0xFFFFu;
    const int h1 = k / 2;
    std::vector<uint32_t>& aKey  = sc.aKey;
    std::vector<uint32_t>& aRows = sc.aRows;
    aKey.clear();
    aRows.clear();
    aKey.push_back(0u);
    aRows.push_back(none | (none << 16));
    for (int a = 0; a < h1; ++a) {
        aKey.push_back(key[size_t(a)]);
        aRows.push_back(uint32_t(a) | (none << 16));
        if (p < 2) continue;
        for (int b = a + 1; b < h1; ++b) {
            aKey.push_back(key[size_t(a)] ^ key[size_t(b)]);
            aRows.push_back(uint32_t(a) | (uint32_t(b) << 16));
        }
    }
    // Сортировка подсчётом по ключу: строки записей переписываются подряд по
    // корзинам, чтобы поиск читал корзину сплошь.
    const size_t buckets = size_t(1) << lw;
    std::vector<uint32_t>& start = sc.start;
    start.assign(buckets + 1, 0u);
    for (uint32_t kk : aKey) ++start[size_t(kk) + 1];
    for (size_t b = 0; b < buckets; ++b) start[b + 1] += start[b];
    std::vector<uint32_t>& sortedRows = sc.sortedRows;
    sortedRows.resize(aKey.size());
    {
        std::vector<uint32_t>& fill = sc.fill;
        fill.assign(start.begin(), start.end() - 1);
        for (size_t i = 0; i < aKey.size(); ++i)
            sortedRows[size_t(fill[size_t(aKey[i])]++)] = aRows[i];
    }

    // Сумма пары: до четырёх строк, пустая — нулевая строка, чтобы обойтись
    // без ветвлений в цикле по словам.
    std::vector<quint64>& zero = sc.zero;
    zero.assign(static_cast<size_t>(words), 0ULL);
    std::vector<quint64>& word = sc.word;
    word.resize(static_cast<size_t>(words));
    auto rowOf = [&](uint32_t r) { return r == none ? zero.data() : g + size_t(r) * words; };
    auto probe = [&](uint32_t kk, uint32_t b0, uint32_t b1) {
        const quint64* r2 = rowOf(b0);
        const quint64* r3 = rowOf(b1);
        for (uint32_t i = start[size_t(kk)]; i < start[size_t(kk) + 1]; ++i) {
            const uint32_t packed = sortedRows[size_t(i)];
            const uint32_t a0 = packed & 0xFFFFu, a1 = packed >> 16;
            if (a0 == none && b0 == none)
                continue;   // «пусто, пусто» — нулевое слово
            const quint64* r0 = rowOf(a0);
            const quint64* r1 = rowOf(a1);
            int weight = 0;
            for (int w = 0; w < words; ++w) {
                const quint64 x = r0[w] ^ r1[w] ^ r2[w] ^ r3[w];
                word[size_t(w)] = x;
                weight += BitOps::popcount64(x);
            }
            if (weight > 0 && weight <= maxWeight)
                visit(word.data(), weight);
        }
    };
    probe(0u, none, none);
    for (int a = h1; a < k; ++a) {
        probe(key[size_t(a)], uint32_t(a), none);
        if (p < 2) continue;
        for (int b = a + 1; b < k; ++b)
            probe(key[size_t(a)] ^ key[size_t(b)], uint32_t(a), uint32_t(b));
    }
    return true;
}

} // namespace Leon
