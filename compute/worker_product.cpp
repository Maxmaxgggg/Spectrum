// Код-произведение: спектры и списки лёгких слов компонент (вложенным
// Worker), свёртка по рангам — шаг расчёта Worker.

#include "worker_p.h"

#include <algorithm>
#include <cmath>
#include <limits>

// Компонента произведения: точный спектр до weightUpTo и — если размерность
// мала — все слова до этого веса. Маленькая компонента перебирается целиком
// (код Грея), большая — Брауэром–Циммерманом во вложенном Worker: он
// сертифицирует спектр до нужного веса и отдаёт итог через finalSpectrum().
Worker::ComponentPlan Worker::planComponent(const QStringList& rows, int weightUpTo, bool wantWords) const
{
    const int k     = rows.size();
    const int n     = rows.first().length();
    const int words = (n + 63) / 64;
    const int limit = std::min(std::max(weightUpTo, 1), n);
    ComponentPlan best{ ComputationSettings::GrayCode, std::numeric_limits<double>::infinity(), QString() };
    auto consider = [&](ComputationSettings::Algorithm a, double cost, const QString& what) {
        if (cost < best.words) best = ComponentPlan{ a, cost, what };
    };

    // Спискам слов (ранги выше первого) полный перебор не помощник: слова
    // собирает только стохастический поиск; маленькую компоненту перебирает
    // Product::bruteForce сам, до planComponent дело не доходит.
    if (wantWords) {
        const Leon::Plan p = Leon::plan(n, k, limit, settings.leonMissProbability(),
                                       settings.compDev == ComputationSettings::ComputeDevice::GPU);
        return ComponentPlan{ ComputationSettings::RandomInfoSets, double(p.trials) * p.costPerTrial,
                              tr("стохастический поиск до веса %1, в-ть пропуска 10^-%2")
                                  .arg(limit).arg(settings.leonMissExponent) };
    }

    // Полный перебор: 2^k слов по k строкам или 2^(n−k) по проверочной
    // матрице. Даёт весь спектр, поэтому при равной цене он предпочтительнее.
    if (k <= 63)
        consider(ComputationSettings::GrayCode, std::ldexp(1.0, k), tr("полный перебор, 2^%1 слов").arg(k));
    if (n - k <= 63)
        consider(ComputationSettings::DualCode, std::ldexp(1.0, n - k), tr("дуальный перебор, 2^%1 слов").arg(n - k));

    // Брауэр–Циммерман до предела: множества ищутся здесь же (это дёшево),
    // цена — множества × комбинации до нужной глубины.
    if (weightUpTo > 0) {
        int packedWords = 0;
        const std::vector<quint64> packed = InfoSets::packRows(rows, packedWords);
        const int fit     = std::max(1, Constants::MAX_CONST_WORDS / std::max(1, k * packedWords));
        const int maxSets = std::min(Constants::MAX_INFO_SETS, fit);
        const std::vector<InfoSets::InfoSet> sets = InfoSets::find(packed.data(), k, n, packedWords, maxSets);
        if (!sets.empty()) {
            std::vector<int> overlaps;
            for (const InfoSets::InfoSet& set : sets) overlaps.push_back(set.overlap);
            const int m = InfoSets::setsForWeight(overlaps, limit, k, n);
            const int r = InfoSets::rowsForWeight(overlaps, limit, k, n);
            if (r < k) {
                const double cost = double(m) * Leon::wordsPerTrial(k, r);
                consider(ComputationSettings::BrouwerZimmermann, cost * 1.001,   // при равенстве — полный
                         tr("Брауэр–Циммерман до веса %1 (%2 множ., до %3 строк)").arg(limit).arg(m).arg(r));
            }
        }
    }
    Q_UNUSED(words);
    return best;
}

Product::Component Worker::analyzeComponent(const QStringList& rows, int weightUpTo,
                                            const QString& label, bool wantWords)
{
    const int k = rows.size();
    const int n = rows.first().length();

    if (k <= productBruteForceMaxK) {
        emit productPlan(tr("%1: полный перебор (2^%2 слов)…").arg(label).arg(k), -1);
        progress.begin(1ULL << k, 0, 0);
        Product::Component c = Product::bruteForce(rows, std::min(weightUpTo, n),
                                                   [this]() { return cancelled.load() != 0; },
                                                   [this](quint64 done, quint64 total) {
                                                       reportStageProgress(done, total);
                                                   },
                                                   productBruteForceMaxK);
        if (cancelled.load())
            throw std::runtime_error("расчёт отменён");
        emit updateInfoPBR(100);
        return c;
    }

    // Алгоритм — по размеру: что дешевле по числу слов перебора. Предел
    // веса 0 (ищем минимальный вес) полный перебор закрывает целиком, а
    // Брауэру–Циммерману нужен предел — его подставляет вызывающий.
    const ComponentPlan plan = planComponent(rows, weightUpTo, wantWords);
    const bool full = plan.algorithm == ComputationSettings::GrayCode
                   || plan.algorithm == ComputationSettings::DualCode;
    emit productPlan(tr("%1: %2…").arg(label, plan.what), -1);

    Worker sub;
    sub.setAutosaveRoot(autosaveRootDir);
    sub.setGridTuningThreshold(tuneThresholdSec);
    sub.setKeepFoundWords(wantWords);
    sub.setWindowPolicy(windowPolicy);

    ComputationSettings cs = settings;
    cs.matrix        = rows;
    cs.matrix2.clear();
    cs.algorithmType = plan.algorithm;
    cs.enumType      = full ? ComputationSettings::EnumerationType::Full
                            : ComputationSettings::EnumerationType::Partial;
    cs.bzWeight      = std::min(std::max(weightUpTo, 1), n);
    cs.leonWeight    = std::min(std::max(weightUpTo, 1), n);
    sub.setSettings(cs);
    sub.initializeRunState(LoadMode::Reset);

    QString error;
    connect(&sub, &Worker::updateInfoPBR,           this, &Worker::updateInfoPBR);
    connect(&sub, &Worker::updateRemainingMinutes,  this, &Worker::updateRemainingMinutes);
    connect(&sub, &Worker::spectrumUpdated,         this, &Worker::spectrumUpdated);
    connect(&sub, &Worker::gridTuned,               this, &Worker::gridTuned);
    connect(&sub, &Worker::errorOccurred, [&error](const QString& m) { error = m; });

    activeSub.store(&sub);
    if (cancelled.load())
        sub.cancel();
    sub.computeSpectrum();
    activeSub.store(nullptr);

    if (!error.isEmpty())
        throw std::runtime_error(error.toStdString());
    if (cancelled.load())
        throw std::runtime_error("расчёт отменён");

    std::vector<quint64> spectrum(sub.finalSpectrum().begin(), sub.finalSpectrum().end());
    if (!wantWords)
        return Product::fromSpectrum(n, k, spectrum, full ? n : sub.finalExactUpTo());

    // Случайный поиск: слова до предела найдены все — с вероятностью пропуска
    // из настроек поиска. Спектр компоненты в этих пределах — счёт найденного.
    Product::Component c = Product::fromSpectrum(n, k, spectrum, std::min(weightUpTo, n));
    c.probabilistic   = true;
    c.missProbability = settings.leonMissProbability();
    c.hasWords        = true;
    c.wordsUpTo       = std::min(weightUpTo, n);
    c.words           = sub.foundWords();
    c.weights         = sub.foundWeights();
    return c;
}

// Код произведения C1 ⊗ C2 — см. product.h. Три шага: минимальные веса
// компонент, спектры и списки лёгких слов до нужного предела, свёртка по
// рангам. Отмена по ходу — исключение, как и ошибка компоненты.
void Worker::computeSpectrumProduct(const CodeGeometry& g)
{
    const QStringList& g1 = settings.matrix;
    const QStringList& g2 = settings.matrix2;
    const int n1 = g1.first().length(), k1 = g1.size();
    const int n2 = g2.first().length(), k2 = g2.size();
    const int maxRank = std::max(1, std::min({ settings.productRank, 4, k1, k2 }));
    const quint64 productLength = quint64(n1) * quint64(n2);

    progress.begin(1, 0, 0);
    productExactUpTo    = -1;
    productMissExponent = 0;
    // Ранги выше первого строятся из списков слов; маленькая компонента
    // отдаёт их с перебором, большой — только случайный поиск, и лишь если
    // наборы посильны (шаг 2).
    const bool wantWords = maxRank >= 2;

    // Шаг 1. Минимальные веса. У маленькой компоненты — из полного перебора,
    // у большой — Брауэром–Циммерманом с удвоением предела, пока слово не
    // найдётся: сертификат «нет слов легче t» с каждым шагом дорожает, но
    // суммарно это не больше двух последних шагов.
    auto minWeight = [&](const QStringList& rows, const QString& label, Product::Component& out) {
        const int n = rows.first().length();
        if (rows.size() <= productBruteForceMaxK) {
            out = analyzeComponent(rows, 0, label, wantWords);
            return;
        }
        // Списки слов здесь не собираются: сперва спектр, по нему решится,
        // посильны ли ранги выше первого (шаг 2).
        for (int t = 8; ; t = std::min(n, t * 2)) {
            out = analyzeComponent(rows, t, label, false);
            if (out.d > 0 || t >= n || out.exactUpTo >= n)
                return;
        }
    };
    Product::Component c1, c2;
    minWeight(g1, tr("компонента 1"), c1);
    minWeight(g2, tr("компонента 2"), c2);
    if (c1.d == 0 || c2.d == 0)
        throw std::invalid_argument("у компоненты нет ненулевых слов: матрица нулевая");

    const int d = c1.d * c2.d;

    // Шаг 2. До какого веса считать. Без явного — до границы, за которой
    // начинаются слова ранга maxRank + 1.
    quint64 target = settings.productWeight > 0
                       ? quint64(settings.productWeight)
                       : Product::rankWeightBound(c1.d, c2.d, maxRank + 1) - 1;
    target = std::min(target, productLength);

    // Пределы по компонентам: слово произведения веса <= target собрано из
    // слов веса <= target/d другой компоненты.
    const int limit1 = int(std::min<quint64>(target / quint64(c2.d), quint64(n1)));
    const int limit2 = int(std::min<quint64>(target / quint64(c1.d), quint64(n2)));
    QStringList notes;
    constexpr quint64 kWorkLimit = 20ULL << 30;
    // Спектры до предела — без списков слов: их даёт дешёвый путь (полный
    // перебор, дуальный, БЦ). Списки нужны только рангам выше первого, а
    // стоят они дорого (миллионы слов в памяти) — сначала по спектру
    // прикидывается, посильны ли наборы; нет — ранги выше первого
    // отменяются сразу, и слова не собираются.
    if (c1.exactUpTo < limit1)
        c1 = analyzeComponent(g1, limit1, tr("компонента 1"), false);
    if (c2.exactUpTo < limit2)
        c2 = analyzeComponent(g2, limit2, tr("компонента 2"), false);
    bool ranksFeasible = maxRank >= 2;
    if (ranksFeasible) {
        const double work1 = Product::estimatedProfileWork(c1, 2, limit1);
        const double work2 = Product::estimatedProfileWork(c2, 2, limit2);
        if (work1 > double(kWorkLimit) || work2 > double(kWorkLimit)) {
            ranksFeasible = false;
            notes << tr("ранг 2 и выше не считался: наборов слишком много (≈%1 проверок у компоненты %2, предел %3)")
                         .arg(QString::number(std::max(work1, work2), 'g', 2))
                         .arg(work1 > work2 ? 1 : 2).arg(QString::number(double(kWorkLimit), 'g', 2));
        }
    }
    if (ranksFeasible) {
        if (!c1.hasWords || c1.wordsUpTo < limit1)
            c1 = analyzeComponent(g1, limit1, tr("компонента 1"), true);
        if (!c2.hasWords || c2.wordsUpTo < limit2)
            c2 = analyzeComponent(g2, limit2, tr("компонента 2"), true);
    }
    if (c1.probabilistic || c2.probabilistic)
        productMissExponent = settings.leonMissExponent;

    // Точность спектра произведения: по рангам — до границы следующего, по
    // компонентам — пока хватает их точности.
    int ranksDone = 1;
    auto exactFor = [&](int ranks) {
        quint64 bound = Product::rankWeightBound(c1.d, c2.d, ranks + 1) - 1;
        bound = std::min(bound, target);
        bound = std::min(bound, quint64(c1.exactUpTo + 1) * quint64(c2.d) - 1);
        bound = std::min(bound, quint64(c2.exactUpTo + 1) * quint64(c1.d) - 1);
        return int(bound);
    };

    // Шаг 3. Ранги. Каждый шаг — со своим ходом и оценкой времени: у
    // перебора наборов единица работы — первое слово набора, у свёртки —
    // профиль первой компоненты.
    std::vector<quint64> total = Product::rankOne(c1, c2, target);
    auto cancelledPoll = [this]() { return cancelled.load() != 0; };
    auto onProgress    = [this](quint64 done, quint64 all) { reportStageProgress(done, all); };
    auto lightWords    = [](const Product::Component& c, int limit) {
        quint64 count = 0;
        for (int w : c.weights) if (w <= limit) ++count;
        return count;
    };
    for (int r = 2; r <= maxRank && ranksFeasible; ++r) {
        if (!c1.hasWords || !c2.hasWords) {
            notes << tr("ранг %1 и выше не считался: у большой компоненты нет списка слов").arg(r);
            break;
        }
        Product::ProfileMap p1, p2;

        // Для рангов выше второго та же прикидка (по второму рангу она
        // оценка снизу).
        if (r > 2) {
            const double work1 = Product::estimatedProfileWork(c1, r, limit1);
            const double work2 = Product::estimatedProfileWork(c2, r, limit2);
            if (work1 > double(kWorkLimit) || work2 > double(kWorkLimit)) {
                notes << tr("ранг %1 и выше не считался: наборов слишком много (≈%2 проверок у компоненты %3, предел %4)")
                             .arg(r).arg(QString::number(std::max(work1, work2), 'g', 2))
                             .arg(work1 > work2 ? 1 : 2).arg(QString::number(double(kWorkLimit), 'g', 2));
                break;
            }
        }

        const quint64 words1 = lightWords(c1, limit1);
        emit productPlan(tr("ранг %1: наборы компоненты 1 (%2 слов веса до %3)…")
                             .arg(r).arg(QString::number(words1)).arg(limit1), -1);
        progress.begin(std::max<quint64>(1, words1), 0, 0);
        const bool ok1 = Product::profiles(c1, r, limit1, kWorkLimit, p1, false, cancelledPoll, onProgress);

        const quint64 words2 = lightWords(c2, limit2);
        emit productPlan(tr("ранг %1: наборы компоненты 2 (%2 слов веса до %3)…")
                             .arg(r).arg(QString::number(words2)).arg(limit2), -1);
        progress.begin(std::max<quint64>(1, words2), 0, 0);
        const bool ok2 = ok1 && Product::profiles(c2, r, limit2, kWorkLimit, p2, true, cancelledPoll, onProgress);
        if (cancelled.load())
            throw std::runtime_error("расчёт отменён");
        if (!ok1 || !ok2) {
            notes << tr("ранг %1 и выше не считался: слишком много наборов").arg(r);
            break;
        }

        emit productPlan(tr("ранг %1: свёртка профилей (%2 x %3)…")
                             .arg(r).arg(QString::number(p1.size())).arg(QString::number(p2.size())), -1);
        progress.begin(std::max<quint64>(1, p1.size()), 0, 0);
        const std::vector<quint64> part = Product::rankR(p1, p2, r, target, onProgress);
        for (size_t w = 0; w < total.size(); ++w)
            total[w] += part[w];
        ranksDone = r;
        emit updateInfoPBR(100);
    }

    productExactUpTo = exactFor(ranksDone);
    buffers->h_spectrum.fillZero();
    buffers->h_spectrum[0] = 1;
    for (size_t w = 1; w < total.size() && int(w) <= productExactUpTo; ++w)
        buffers->h_spectrum[w] = total[w];

    if (productMissExponent > 0)
        notes.prepend(tr("компоненты %1 — стохастическим поиском, в-ть пропуска до 10^-%2")
                          .arg(c1.probabilistic && c2.probabilistic ? QStringLiteral("1 и 2")
                               : c1.probabilistic ? QStringLiteral("1") : QStringLiteral("2"))
                          .arg(productMissExponent));
    const QString summary = tr("[%1,%2,%3] x [%4,%5,%6] = [%7,%8,%9]; ранги до %10; %13 до веса %11%12")
                                .arg(n1).arg(k1).arg(c1.d).arg(n2).arg(k2).arg(c2.d)
                                .arg(productLength).arg(quint64(k1) * quint64(k2)).arg(d)
                                .arg(ranksDone).arg(productExactUpTo)
                                .arg(notes.isEmpty() ? QString() : QStringLiteral("; ") + notes.join(QStringLiteral("; ")))
                                .arg(productMissExponent > 0 ? tr("полно") : tr("точно"));
    emit productPlan(summary, productExactUpTo);
    progress.setDoneOps(1);
    emit updateInfoPBR(100);
    updateSpectrum(int(g.numOfCols));
}
