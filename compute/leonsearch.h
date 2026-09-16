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

#include "infosets.h"

#include <QtGlobal>

#ifdef _MSC_VER
    #include <intrin.h>
#endif

#include <cstdint>
#include <vector>

namespace Leon {

// Вероятность, что слово веса weight имеет не больше rows единиц на случайном
// информационном множестве кода [n, k] — гипергеометрический хвост.
double catchProbability(int n, int k, int weight, int rows);

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

// План поиска: глубина перебора в попытке и число попыток, при которых слово
// веса weight пропускается с вероятностью не больше miss. Глубина выбирается
// по цене: попыток тем меньше, чем глубже перебор, но каждая дороже.
struct Plan
{
    int     rows          = 0;
    quint64 trials        = 0;
    double  wordsPerTrial = 0.0;   // слов перебирается за попытку
    double  costPerTrial  = 0.0;   // то же плюс цена Гаусса — для прогресса
};
Plan plan(int n, int k, int weight, double miss, bool gpu = false);

// Таблица найденных слов: само слово, вес и сколько раз поймано.
// Не потокобезопасна — добавления серийные.
class WordTable
{
public:
    WordTable(int wordsPerRow, int maxWeight);

    // true — слово новое.
    bool add(const quint64* word, int weight);
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
    // OpenMP сразу, вес считается здесь.
    void addBatch(const quint64* words, size_t count);

    quint64 size()  const;
    quint64 bytes() const;
    std::vector<quint64> countByWeight()  const;
    std::vector<double>  unseenByWeight() const;
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
                    weight += int(popcount64(word[size_t(w)]));
                }
                if (weight > 0 && weight <= maxWeight)
                    visit(word.data(), weight);
                if (depth + 1 < depthMax)
                    go(i + 1, depth + 1);
                for (int w = 0; w < words; ++w)
                    word[size_t(w)] ^= row[w];
            }
        }
        static int popcount64(quint64 v)
        {
#ifdef _MSC_VER
            return int(__popcnt64(v));
#else
            return __builtin_popcountll(v);
#endif
        }
    };
    Walker walker{ g, rows, wordsPerRow, rowsPerTrial, maxWeight, word, visit };
    walker.go(0, 0);
    return true;
}

} // namespace Leon
