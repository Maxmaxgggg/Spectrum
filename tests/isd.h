#pragma once

// Прототип: улучшения случайного поиска и сравнение с базовым вариантом.
//
// Базовый вариант — тот, что в приложении (Леон без окна, он же Ли–Брикелл):
// каждая попытка — новое случайное информационное множество полным Гауссом и
// перебор сумм до p строк. Улучшения:
//
//  * Канто–Шабо (CC): множество не строится заново, а меняется на один столбец
//    за итерацию — одно опорное преобразование k строк вместо k² операций.
//    Итерации зависимы, поэтому число итераций до полноты — только по замеру.
//
//  * Окно Штерна–Дюмера (St): строки делятся на две половины, на l случайных
//    проверочных столбцах (окне) считаются частичные суммы до p строк каждой
//    половины, и полная сумма считается только для пар, совпавших на окне.
//    Ловятся слова, у которых на каждой половине не больше p единиц и ноль на
//    окне. Пустая комбинация тоже в списках — слова, лёгкие лишь на одной
//    половине, не теряются.
//
//  * St+CC — окно на инкрементальном множестве.
//
// Сравнение — на одной задаче: найти все слова веса до W. Эталон — базовый
// вариант с числом попыток по аддитивной границе (пропуск 10^-N); остальным
// засекается время до полноты эталона, плюс прогноз по модели: P_W варианта,
// попыток до пропуска 10^-N и время при замеренной скорости итераций.
//
// Один поток у всех. Запуск: SpectrumTests.exe --isd <файл|random:n,k[,seed]>
// <W> [степень пропуска=9] [прогонов=3] [p Штерна] [l Штерна]

#include "infosets.h"
#include "leon.h"

#include <QStringList>
#include <QTextStream>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <random>
#include <vector>

namespace Isd {

inline int popcount64(quint64 v)
{
#ifdef _MSC_VER
    return int(__popcnt64(v));
#else
    return __builtin_popcountll(v);
#endif
}

inline double logBinom(int n, int k)
{
    if (k < 0 || k > n) return -INFINITY;
    return std::lgamma(n + 1.0) - std::lgamma(k + 1.0) - std::lgamma(n - k + 1.0);
}

struct Code
{
    int n = 0, k = 0, words = 0;
    std::vector<quint64> rows;   // k строк по words слов
};

inline Code fromRows(const QStringList& matrix)
{
    Code c;
    c.k    = matrix.size();
    c.n    = c.k > 0 ? matrix.first().length() : 0;
    c.rows = InfoSets::packRows(matrix, c.words);
    return c;
}

inline Code randomCode(int n, int k, quint64 seed)
{
    std::mt19937_64 rng(seed);
    for (;;) {
        QStringList rows;
        for (int i = 0; i < k; ++i) {
            QString row(n, QLatin1Char('0'));
            for (int j = 0; j < n; ++j)
                if (rng() & 1) row[j] = QLatin1Char('1');
            rows.append(row);
        }
        Code c = fromRows(rows);
        std::vector<int> order(static_cast<size_t>(n), 0);
        std::iota(order.begin(), order.end(), 0);
        InfoSets::InfoSet set;
        if (InfoSets::systematize(c.rows.data(), c.k, c.n, c.words, order, nullptr, set))
            return c;
    }
}

// Систематическая матрица и учёт столбцов.
struct Sys
{
    int n = 0, k = 0, words = 0;
    std::vector<quint64> g;        // k строк
    std::vector<int>     pivot;    // pivot[i] — столбец с единичным вектором в строке i
    std::vector<int>     other;    // остальные столбцы
    std::vector<int>     rowOf;    // столбец -> строка опоры или -1

    bool bit(int row, int col) const
    {
        return (g[size_t(row) * words + (col >> 6)] >> (col & 63)) & 1ULL;
    }
};

// Полный Гаусс по случайному порядку столбцов.
inline bool fullGauss(const Code& c, std::mt19937_64& rng, Sys& s)
{
    std::vector<int> order(static_cast<size_t>(c.n), 0);
    std::iota(order.begin(), order.end(), 0);
    std::shuffle(order.begin(), order.end(), rng);
    InfoSets::InfoSet set;
    if (!InfoSets::systematize(c.rows.data(), c.k, c.n, c.words, order, nullptr, set))
        return false;
    s.n = c.n; s.k = c.k; s.words = c.words;
    s.g     = std::move(set.rows);
    s.pivot = std::move(set.columns);
    s.rowOf.assign(size_t(c.n), -1);
    for (int i = 0; i < c.k; ++i)
        s.rowOf[size_t(s.pivot[size_t(i)])] = i;
    s.other.clear();
    for (int col = 0; col < c.n; ++col)
        if (s.rowOf[size_t(col)] < 0)
            s.other.push_back(col);
    return true;
}

// Шаг Канто–Шабо: случайный проверочный столбец входит в множество, вытесняя
// опорный столбец случайной строки, в которой у него единица. Одно опорное
// преобразование.
inline bool swapOne(Sys& s, std::mt19937_64& rng)
{
    for (int attempt = 0; attempt < 64; ++attempt) {
        const size_t oi = size_t(rng() % s.other.size());
        const int    c  = s.other[oi];
        // Строки с единицей в столбце c.
        int candidates = 0, chosen = -1;
        for (int i = 0; i < s.k; ++i)
            if (s.bit(i, c)) {
                ++candidates;
                if (rng() % quint64(candidates) == 0)   // равновероятный выбор на ходу
                    chosen = i;
            }
        if (chosen < 0)
            continue;   // столбец нулевой — в множество не годится
        const quint64* src = s.g.data() + size_t(chosen) * s.words;
        for (int j = 0; j < s.k; ++j) {
            if (j == chosen || !s.bit(j, c))
                continue;
            quint64* dst = s.g.data() + size_t(j) * s.words;
            for (int w = 0; w < s.words; ++w)
                dst[w] ^= src[w];
        }
        const int old = s.pivot[size_t(chosen)];
        s.pivot[size_t(chosen)] = c;
        s.rowOf[size_t(c)]      = chosen;
        s.rowOf[size_t(old)]    = -1;
        s.other[oi]             = old;
        return true;
    }
    return false;
}

// Суммы до p строк — как Leon::trial.
template <class Visit>
void enumerateSums(const Sys& s, int p, int W, Visit&& visit)
{
    std::vector<quint64> word(size_t(s.words), 0ULL);
    struct Walker
    {
        const Sys& s; int p; int W; std::vector<quint64>& word; Visit& visit;
        void go(int from, int depth)
        {
            for (int i = from; i < s.k; ++i) {
                const quint64* row = s.g.data() + size_t(i) * s.words;
                int weight = 0;
                for (int w = 0; w < s.words; ++w) {
                    word[size_t(w)] ^= row[w];
                    weight += popcount64(word[size_t(w)]);
                }
                if (weight > 0 && weight <= W)
                    visit(word.data(), weight);
                if (depth + 1 < p)
                    go(i + 1, depth + 1);
                for (int w = 0; w < s.words; ++w)
                    word[size_t(w)] ^= row[w];
            }
        }
    };
    Walker walker{ s, p, W, word, visit };
    walker.go(0, 0);
}

// Рабочие буферы окна Штерна — чтобы не выделять память на каждой итерации.
struct SternScratch
{
    std::vector<int>      perm;       // перестановка строк: первая половина A, вторая B
    std::vector<int>      window;     // столбцы окна
    std::vector<uint32_t> key;        // ключ каждой строки на окне
    // Список A: комбинации до p строк, отсортированные по ключу (подсчётом).
    std::vector<uint32_t> aKey;
    std::vector<int16_t>  aRows;      // по 3 индекса на комбинацию, -1 — пусто
    std::vector<uint32_t> bucketStart;
    std::vector<uint32_t> sortedA;
    std::vector<quint64>  word;
};

// Одна итерация Штерна–Дюмера на текущей систематической матрице.
template <class Visit>
void sternIteration(const Sys& s, int p, int l, int W, std::mt19937_64& rng,
                    SternScratch& sc, Visit&& visit)
{
    const int k = s.k, words = s.words;
    const int h1 = k / 2;

    sc.perm.resize(size_t(k));
    std::iota(sc.perm.begin(), sc.perm.end(), 0);
    std::shuffle(sc.perm.begin(), sc.perm.end(), rng);

    // Окно: l различных проверочных столбцов.
    sc.window.assign(s.other.begin(), s.other.end());
    for (int i = 0; i < l; ++i)
        std::swap(sc.window[size_t(i)], sc.window[size_t(i) + rng() % (sc.window.size() - size_t(i))]);
    sc.window.resize(size_t(l));

    sc.key.assign(size_t(k), 0u);
    for (int i = 0; i < k; ++i) {
        uint32_t key = 0;
        for (int b = 0; b < l; ++b)
            key |= uint32_t(s.bit(i, sc.window[size_t(b)])) << b;
        sc.key[size_t(i)] = key;
    }

    // Список A: комбинации размера 0..p из первой половины.
    sc.aKey.clear();
    sc.aRows.clear();
    auto pushA = [&](int r0, int r1, int r2, uint32_t key) {
        sc.aKey.push_back(key);
        sc.aRows.push_back(int16_t(r0)); sc.aRows.push_back(int16_t(r1)); sc.aRows.push_back(int16_t(r2));
    };
    pushA(-1, -1, -1, 0u);
    for (int a = 0; a < h1; ++a) {
        const int ra = sc.perm[size_t(a)];
        pushA(ra, -1, -1, sc.key[size_t(ra)]);
        if (p < 2) continue;
        for (int b = a + 1; b < h1; ++b) {
            const int rb = sc.perm[size_t(b)];
            pushA(ra, rb, -1, sc.key[size_t(ra)] ^ sc.key[size_t(rb)]);
            if (p < 3) continue;
            for (int c = b + 1; c < h1; ++c) {
                const int rc = sc.perm[size_t(c)];
                pushA(ra, rb, rc, sc.key[size_t(ra)] ^ sc.key[size_t(rb)] ^ sc.key[size_t(rc)]);
            }
        }
    }
    // Сортировка подсчётом по ключу.
    const size_t buckets = size_t(1) << l;
    sc.bucketStart.assign(buckets + 1, 0u);
    for (uint32_t key : sc.aKey)
        ++sc.bucketStart[size_t(key) + 1];
    for (size_t b = 0; b < buckets; ++b)
        sc.bucketStart[b + 1] += sc.bucketStart[b];
    sc.sortedA.resize(sc.aKey.size());
    {
        std::vector<uint32_t> fill(sc.bucketStart.begin(), sc.bucketStart.end() - 1);
        for (uint32_t i = 0; i < uint32_t(sc.aKey.size()); ++i)
            sc.sortedA[size_t(fill[size_t(sc.aKey[size_t(i)])]++)] = i;
    }

    sc.word.assign(size_t(words), 0ULL);
    auto emitPair = [&](uint32_t aIndex, int r0, int r1, int r2) {
        // (пусто, пусто) — нулевое слово.
        const int16_t* ar = sc.aRows.data() + size_t(aIndex) * 3;
        if (ar[0] < 0 && r0 < 0)
            return;
        std::fill(sc.word.begin(), sc.word.end(), 0ULL);
        const int rows[6] = { ar[0], ar[1], ar[2], r0, r1, r2 };
        for (int r : rows) {
            if (r < 0) continue;
            const quint64* row = s.g.data() + size_t(r) * words;
            for (int w = 0; w < words; ++w)
                sc.word[size_t(w)] ^= row[w];
        }
        int weight = 0;
        for (int w = 0; w < words; ++w)
            weight += popcount64(sc.word[size_t(w)]);
        if (weight > 0 && weight <= W)
            visit(sc.word.data(), weight);
    };
    auto probe = [&](uint32_t key, int r0, int r1, int r2) {
        for (uint32_t i = sc.bucketStart[size_t(key)]; i < sc.bucketStart[size_t(key) + 1]; ++i)
            emitPair(sc.sortedA[size_t(i)], r0, r1, r2);
    };

    // Список B: комбинации размера 0..p из второй половины, поиск совпадений.
    probe(0u, -1, -1, -1);
    for (int a = h1; a < k; ++a) {
        const int ra = sc.perm[size_t(a)];
        probe(sc.key[size_t(ra)], ra, -1, -1);
        if (p < 2) continue;
        for (int b = a + 1; b < k; ++b) {
            const int rb = sc.perm[size_t(b)];
            probe(sc.key[size_t(ra)] ^ sc.key[size_t(rb)], ra, rb, -1);
            if (p < 3) continue;
            for (int c = b + 1; c < k; ++c) {
                const int rc = sc.perm[size_t(c)];
                probe(sc.key[size_t(ra)] ^ sc.key[size_t(rb)] ^ sc.key[size_t(rc)], ra, rb, rc);
            }
        }
    }
}

// ---------------------------------------------------------------- модель

// Вероятность поймать слово веса w за итерацию Штерна: до p единиц на каждой
// половине (h1 и k-h1 столбцов), ноль на окне из l столбцов, остальное — где
// угодно на n-k-l столбцах.
inline double probStern(int n, int k, int w, int p, int l)
{
    const int h1 = k / 2, h2 = k - h1, rest = n - k - l;
    const double logTotal = logBinom(n, w);
    double sum = 0.0;
    for (int a = 0; a <= p; ++a)
        for (int b = 0; b <= p; ++b) {
            if (a + b == 0 || w - a - b < 0) continue;
            const double t = logBinom(h1, a) + logBinom(h2, b) + logBinom(rest, w - a - b) - logTotal;
            if (std::isfinite(t)) sum += std::exp(t);
        }
    return sum;
}

inline double probLeeBrickell(int n, int k, int w, int p)
{
    return Leon::catchProbability(n, k, w, p);
}

// Попыток, чтобы ни одно из найденных слов не осталось непойманным с
// вероятностью больше miss: максимум по весам ln(A_w/miss)/P_w.
template <class Prob>
double trialsForAll(const std::vector<quint64>& byWeight, double miss, Prob&& prob)
{
    double best = 0.0;
    for (int w = 1; w < int(byWeight.size()); ++w) {
        if (byWeight[size_t(w)] == 0) continue;
        const double P = prob(w);
        if (P <= 0.0) return INFINITY;
        best = std::max(best, std::log(double(byWeight[size_t(w)]) / miss) / P);
    }
    return best;
}

// Список A Штерна: сколько комбинаций.
inline double sternListSize(int k, int p)
{
    const int h = k / 2;
    double s = 1.0;
    for (int a = 1; a <= p; ++a) s += std::exp(logBinom(h, a));
    return s;
}

// Выбор (p, l) Штерна по модели: цена итерации на одну поимку слова веса W.
inline void chooseStern(int n, int k, int W, bool incremental, int& p, int& l)
{
    const int words = (n + 63) / 64;
    const double gauss = incremental ? 2.0 * k * words : double(k) * k * words / 8.0;
    double bestCost = INFINITY;
    for (int pp = 1; pp <= 3; ++pp) {
        const double L = sternListSize(k, pp);
        for (int ll = 1; ll <= 26 && ll < n - k; ll++) {
            const double P = probStern(n, k, W, pp, ll);
            if (P <= 0.0) continue;
            const double collisions = L * L / std::ldexp(1.0, ll);
            const double cost = (gauss + 2.0 * L * 2.0 + collisions * 2.0 * pp * words) / P;
            if (cost < bestCost) { bestCost = cost; p = pp; l = ll; }
        }
    }
}

// ---------------------------------------------------------------- сравнение

struct Variant
{
    QString name;
    bool    stern = false;
    bool    incremental = false;
    int     swaps = 1;     // шагов Канто–Шабо на итерацию: больше — слабее зависимость итераций
    int     p = 2;
    int     l = 0;
};

struct Outcome
{
    quint64 iterations = 0;
    double  seconds    = 0.0;
    quint64 found      = 0;
    quint64 matched    = 0;
    bool    complete   = false;
};

// Прогон варианта: до полноты эталона (reference), либо до предела итераций.
// Если reference пуст — это построение эталона: до числа попыток по
// аддитивной границе.
inline Outcome runVariant(const Code& code, const Variant& v, int W, double miss, quint64 seed,
                          const Leon::WordTable* reference, Leon::WordTable& table, double maxIterations)
{
    std::mt19937_64 rng(seed);
    Sys sys;
    SternScratch scratch;
    Outcome out;
    const auto t0 = std::chrono::steady_clock::now();

    auto prob = [&](int w) {
        return v.stern ? probStern(code.n, code.k, w, v.p, v.l) : probLeeBrickell(code.n, code.k, w, v.p);
    };
    auto visit = [&](const quint64* word, int weight) {
        if (table.add(word, weight) && reference && reference->contains(word))
            ++out.matched;
    };

    bool haveSys = false;
    for (;;) {
        if (!haveSys || !v.incremental) {
            if (!fullGauss(code, rng, sys))
                break;
            haveSys = true;
        } else {
            for (int i = 0; i < v.swaps; ++i)
                swapOne(sys, rng);
        }
        if (v.stern)
            sternIteration(sys, v.p, v.l, W, rng, scratch, visit);
        else
            enumerateSums(sys, v.p, W, visit);
        ++out.iterations;

        if (reference) {
            if (out.matched >= reference->size()) { out.complete = true; break; }
            if (double(out.iterations) >= maxIterations) break;
        } else {
            // Эталон: попыток по аддитивной границе, не реже раза в 64 итерации.
            if ((out.iterations & 63) == 0 || out.iterations < 64) {
                // Пока слов нет — на одно слово веса W.
                const double need = std::max(trialsForAll(table.countByWeight(), miss, prob),
                                             std::log(1.0 / miss) / std::max(prob(W), 1e-300));
                if (double(out.iterations) >= need) { out.complete = true; break; }
            }
            if (double(out.iterations) >= maxIterations) break;
        }
    }
    out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    out.found   = table.size();
    return out;
}

inline int compare(QTextStream& out, const Code& code, int W, int missExp, int runs,
                   int sternP, int sternL)
{
    const double miss = std::pow(10.0, -missExp);
    const int n = code.n, k = code.k;

    // Параметры базового варианта — как в приложении.
    const Leon::Plan plan = Leon::plan(n, k, W, miss, false);
    int pSt = 0, lSt = 0;
    chooseStern(n, k, W, false, pSt, lSt);
    if (sternP > 0) pSt = sternP;
    if (sternL > 0) lSt = sternL;

    std::vector<Variant> variants = {
        { QStringLiteral("Леон (полный Гаусс)"),  false, false, 1,  plan.rows, 0 },
        { QStringLiteral("Канто–Шабо"),           false, true,  1,  plan.rows, 0 },
        { QStringLiteral("Канто–Шабо x8"),        false, true,  8,  plan.rows, 0 },
        { QStringLiteral("Канто–Шабо x32"),       false, true,  32, plan.rows, 0 },
        { QStringLiteral("Штерн–Дюмер"),          true,  false, 1,  pSt,  lSt },
        { QStringLiteral("Штерн + Канто–Шабо x8"),true,  true,  8,  pSt,  lSt },
        { QStringLiteral("Штерн + Канто–Шабо x32"),true, true,  32, pSt,  lSt },
    };

    out << QStringLiteral("[%1,%2], все слова до веса %3, пропуск 10^-%4; один поток")
               .arg(n).arg(k).arg(W).arg(missExp) << Qt::endl;

    // Эталон: базовый вариант до числа попыток по аддитивной границе.
    Leon::WordTable reference(code.words, W);
    const Outcome ref = runVariant(code, variants[0], W, miss, 1, nullptr, reference, 1e12);
    out << QStringLiteral("эталон: p=%1, попыток %2, %3 с, слов %4")
               .arg(plan.rows).arg(ref.iterations).arg(ref.seconds, 0, 'f', 2).arg(ref.found) << Qt::endl;
    const std::vector<quint64> byWeight = reference.countByWeight();
    for (int w = 1; w <= W; ++w)
        if (byWeight[size_t(w)] > 0)
            out << QStringLiteral("   %1  %2").arg(w, 3).arg(byWeight[size_t(w)], 10) << Qt::endl;

    out << Qt::endl
        << QStringLiteral("%1 | %2 | %3 | %4 | %5 | %6")
               .arg(QStringLiteral("вариант"), -22).arg(QStringLiteral("параметры"), -10)
               .arg(QStringLiteral("итер/с"), 9).arg(QStringLiteral("P_W модель"), 11)
               .arg(QStringLiteral("прогноз: попыток, с"), 22).arg(QStringLiteral("факт до полноты: итераций, с (по прогонам)"))
        << Qt::endl;

    for (const Variant& v : variants) {
        auto prob = [&](int w) {
            return v.stern ? probStern(n, k, w, v.p, v.l) : probLeeBrickell(n, k, w, v.p);
        };
        const double PW    = prob(W);
        const double need  = trialsForAll(byWeight, miss, prob);
        const double cap   = std::max(need * 50.0, 1000.0);

        QStringList facts;
        double sumSec = 0.0, sumIter = 0.0, sumRate = 0.0;
        int done = 0;
        for (int r = 0; r < runs; ++r) {
            Leon::WordTable table(code.words, W);
            const Outcome o = runVariant(code, v, W, miss, quint64(1000 + r), &reference, table, cap);
            facts << QStringLiteral("%1/%2").arg(o.iterations).arg(o.seconds, 0, 'f', 2)
                         + (o.complete ? QString() : QStringLiteral("(неполно: %1 из %2)").arg(o.matched).arg(reference.size()));
            sumRate += double(o.iterations) / std::max(o.seconds, 1e-9);
            if (o.complete) { sumSec += o.seconds; sumIter += double(o.iterations); ++done; }
        }
        const double rate = sumRate / runs;
        const QString params = v.stern ? QStringLiteral("p=%1 l=%2").arg(v.p).arg(v.l)
                                       : QStringLiteral("p=%1").arg(v.p);
        Q_UNUSED(cap);
        out << QStringLiteral("%1 | %2 | %3 | %4 | %5 | %6")
                   .arg(v.name, -22).arg(params, -10)
                   .arg(rate, 9, 'f', 1).arg(PW, 11, 'e', 2)
                   .arg(QStringLiteral("%1, %2").arg(need, 0, 'e', 2).arg(need / rate, 0, 'f', 1), 22)
                   .arg(facts.join(QStringLiteral("  ")))
            << Qt::endl;
        if (done > 0)
            out << QStringLiteral("%1   среднее до полноты: %2 итераций, %3 с")
                       .arg(QString(), -22).arg(sumIter / done, 0, 'f', 0).arg(sumSec / done, 0, 'f', 2) << Qt::endl;
    }
    return 0;
}

} // namespace Isd
