#pragma once

// Низ спектра кода произведения C1 ⊗ C2 по компонентам, без матрицы
// самого произведения: у него длина n1·n2 и размерность k1·k2, и при
// компонентах по две тысячи бит это миллионы — такое не перебирается.
//
// Кодовое слово произведения — матрица n1 x n2, столбцы которой лежат в C1,
// а строки в C2. Слово ранга r — сумма r произведений a_i ⊗ b_i с независимыми
// a_i из C1 и b_i из C2. Его вес зависит только от того, как наборы {a_i}
// и {b_i} накрывают позиции: если N_A(u) — число позиций x, где вектор
// (a_1(x), …, a_r(x)) равен u, а N_B(v) — то же для {b_i}, то
//
//     wt = Σ_{u,v ≠ 0} N_A(u) · N_B(v) · [⟨u, v⟩ = 1].
//
// Значит, достаточно знать распределение «профилей» N(·) по упорядоченным
// независимым r-наборам в каждой компоненте; одно слово ранга r получается
// из |GL(r, 2)| пар наборов, на это и делим.
//
// Ранг 1 проще: A_w = Σ_{i·j = w} A_i(C1) · A_j(C2) — нужны только спектры.
//
// Слово ранга r весит не меньше d_r(C1)·d2 и d1·d_r(C2), где d_r —
// обобщённый вес Хэмминга, а он по Грисмеру не меньше Σ_{i<r} ⌈d/2^i⌉.
// Поэтому, посчитав ранги до R, спектр знаем точно до веса, с которого
// начинаются слова ранга R+1. Для R = 1 это граница Толхёйзена.
//
// Наборы веса ≤ W у слова произведения ранга r: каждое a_i весит не больше
// W/d2, каждое b_i — не больше W/d1 (носитель набора не легче d_r, а
// вес слова — не меньше d2·|носитель|). Поэтому хватает списков лёгких
// слов компонент.

#include <QStringList>
#include <QtGlobal>

#include <functional>
#include <unordered_map>
#include <vector>

namespace Product {

// Компонента: спектр (точный до exactUpTo включительно) и список ненулевых
// слов веса не больше wordsUpTo — все такие слова, если hasWords.
struct Component
{
    int n = 0;
    int k = 0;
    int wordsPerRow = 0;
    // Минимальный вес; 0 — не найден (тяжелее exactUpTo).
    int d = 0;
    int exactUpTo = -1;
    std::vector<quint64> spectrum;     // индекс — вес, размер n + 1

    // Спектр и список получены случайным поиском: полны с вероятностью
    // пропуска не больше missProbability, а не по сертификату.
    bool                 probabilistic   = false;
    double               missProbability = 0.0;

    bool                 hasWords  = false;
    int                  wordsUpTo = -1;
    std::vector<quint64> words;        // подряд, по wordsPerRow слов
    std::vector<int>     weights;      // вес каждого слова из words

    quint64 wordCount() const { return weights.size(); }
    const quint64* word(size_t i) const { return words.data() + i * size_t(wordsPerRow); }
};

// Ход работы: сделано/всего в единицах текущего шага. Зовётся из потока,
// вызвавшего функцию, не чаще нескольких раз в секунду.
using Progress = std::function<void(quint64 done, quint64 total)>;

// До какой размерности компоненту выгодно перебирать целиком.
constexpr int kBruteForceMaxK = 28;

// Полный перебор компоненты кодом Грея: точный спектр и все ненулевые слова
// веса не больше wordsUpTo. Годится при k до maxK (и не больше 62).
// cancelled — опрос отмены; при отмене возвращает hasWords = false.
Component bruteForce(const QStringList& rows, int wordsUpTo,
                     const std::function<bool()>& cancelled = {},
                     const Progress& progress = {},
                     int maxK = kBruteForceMaxK);

// Спектр компоненты в объект без списка слов (для больших k, где спектр
// посчитан другим способом).
Component fromSpectrum(int n, int k, const std::vector<quint64>& spectrum, int exactUpTo);

// Слова ранга 1 веса до maxWeight по спектрам компонент.
std::vector<quint64> rankOne(const Component& c1, const Component& c2, quint64 maxWeight);

// Профиль набора: 2^r - 1 счётчиков ячеек, по одному на ненулевой u.
// Ключ — упакованные счётчики; в каждом до 12 бит (n <= 4095).
struct ProfileKey
{
    quint64 lo = 0, hi = 0, top = 0;
    bool operator==(const ProfileKey& o) const { return lo == o.lo && hi == o.hi && top == o.top; }
};
struct ProfileKeyHash
{
    size_t operator()(const ProfileKey& key) const
    {
        quint64 h = key.lo * 0x9E3779B97F4A7C15ULL;
        h ^= (key.hi + 0x632BE59BD9B4E019ULL) * 0xBF58476D1CE4E5B9ULL;
        h ^= (key.top + 0x94D049BB133111EBULL) * 0xFF51AFD7ED558CCDULL;
        return size_t(h ^ (h >> 29));
    }
};
using ProfileMap = std::unordered_map<ProfileKey, quint64, ProfileKeyHash>;

ProfileKey packProfile(const std::vector<int>& cells);
std::vector<int> unpackProfile(const ProfileKey& key, int cellCount);

// Профили независимых r-наборов слов компоненты, у которых объединение
// носителей не больше unionLimit: ordered — по всем упорядоченным наборам,
// иначе по одному на множество. Перебор с отсечкой по объединению;
// workLimit — потолок числа рассмотренных кандидатов, при превышении
// возвращает false (профили неполные — использовать нельзя).
// Сколько кандидатов просмотрит profiles() при r = 2 (для r >= 3 — оценка
// снизу): по каждому первому слову — все слова веса не выше потолка, который
// оставляет ему отсечка. Считается по спектру компоненты мгновенно — ещё до
// того, как собирать списки слов: чтобы не начинать перебор, которому не
// хватит и недели.
double estimatedProfileWork(const Component& c, int r, int unionLimit);

bool profiles(const Component& c, int r, int unionLimit, quint64 workLimit,
              ProfileMap& out, bool ordered, const std::function<bool()>& cancelled = {},
              const Progress& progress = {});

// Слова ранга r веса до maxWeight: свёртка профилей двух компонент. p1 —
// по множествам, p2 — по упорядоченным наборам; тогда каждое слово получено
// |GL(r, 2)| / r! раз, на это и делится.
std::vector<quint64> rankR(const ProfileMap& p1, const ProfileMap& p2, int r, quint64 maxWeight,
                           const Progress& progress = {});

// |GL(r, 2)| = Π_{i<r} (2^r - 2^i).
quint64 generalLinearOrder(int r);

// Нижняя граница веса слов ранга r по Грисмеру для обобщённых весов:
// max(d2 · Σ_{i<r} ⌈d1/2^i⌉, d1 · Σ_{i<r} ⌈d2/2^i⌉).
quint64 rankWeightBound(int d1, int d2, int r);

} // namespace Product
