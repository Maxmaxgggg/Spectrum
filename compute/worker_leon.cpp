// Случайный поиск по информационным множествам (Леон, Штерн–Дюмер) на
// процессоре и на видеокарте — шаг расчёта Worker.

#include "worker_p.h"
#include "leonkernel.cuh"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>

// Случайный поиск по информационным множествам — см. leon.h.
//
// Попытки независимы и нумерованы; номер задаёт порядок столбцов, поэтому
// попытка с одним номером даёт одно и то же множество на CPU и на GPU, и оба
// пути обязаны находить одни и те же слова. Найденное складывается в таблицу
// на хосте: там считаются поимки, по ним — оценка ненайденного.
//
// CPU: пачка попыток раздаётся потокам OpenMP, слова кладутся в таблицу под
// замком. Замок не мешает: слов нужного веса — доли процента от перебранных.
//
// GPU: блок нитей — попытка (leonkernel.cu), слова выкладываются в буфер, а
// хост забирает их и вставляет в таблицу. Буферов два: пока хост разбирает
// одну пачку, видеокарта считает следующую.
//
// Чекпоинтов по ходу нет: пришлось бы писать на диск всю таблицу, а поиск по
// самой своей природе короткий — его предел ставит память под слова.
void Worker::computeSpectrumLeon(const CodeGeometry& g)
{
    const int rows  = int(g.numOfRows);
    const int cols  = int(g.numOfCols);
    const int words = int(g.wordsPerRow);
    const int depth = int(g.maxRows);
    const int maxWeight = m_settings.leonWeight;

    // Память под слова — из настроек; по умолчанию половина физической: таблица
    // растёт удвоением, и в момент роста ей нужно место под старую и новую
    // копии сразу.
    const quint64 kTableLimitBytes = m_settings.leonMemoryMb > 0
        ? quint64(m_settings.leonMemoryMb) << 20
        : std::max<quint64>(256ULL << 20, Leon::physicalMemoryBytes() / 2);

    // Ранг проверяется один раз здесь: ядро молча даёт пустую попытку, а
    // CPU-путь узнал бы об этом только внутри параллельной области.
    {
        std::vector<int> order;
        Leon::shuffledColumns(cols, 0, order);
        InfoSets::InfoSet set;
        if (!InfoSets::systematize(m_buffers->h_matrix.get(), rows, cols, words, order, nullptr, set))
            throw std::invalid_argument(
                "строки матрицы зависимы: стохастическому поиску нужна матрица полного ранга");
    }

    // Частей — по числу потоков: каждая наполняется своим.
    Leon::ShardedWordTable table(words, maxWeight, std::max(1, omp_get_max_threads()));

    // Прогресс считается в словах, как везде: скорость тогда сравнима.
    // Число попыток растёт по ходу: чем больше слов какого-то веса нашлось,
    // тем больше попыток нужно, чтобы ни одно из них не оказалось пропущено
    // с заданной вероятностью. План даёт нижнюю границу — на одно слово.
    quint64 target = g.leonTrials;
    auto opsFor = [&](quint64 trials) {
        const double ops = double(trials) * g.leonWordsPerTrial;
        return ops >= 1.8e19 ? std::numeric_limits<quint64>::max() : quint64(ops);
    };
    m_progress.begin(opsFor(target), 0, 0);

    quint64 launched  = 0;   // попыток начато
    quint64 collected = 0;   // попыток, чьи слова уже в таблице

    auto retarget = [&]() {
        const quint64 needed = Leon::trialsForAll(cols, rows, maxWeight, depth, g.leonWindow,
                                                  m_settings.leonMissProbability(),
                                                  table.countByWeight());
        if (needed > target) {
            target = needed;
            m_progress.setTotalOps(opsFor(target));
        }
    };

    // Оценка Чао: поимки слов считаются либо на хосте (CPU-путь и слова, не
    // попавшие в таблицу видеокарты), либо на видеокарте; f1/f2 складываются.
    std::function<void(std::vector<quint64>&, std::vector<quint64>&)> deviceHitCounts;
    auto chaoUnseen = [&]() {
        std::vector<quint64> f1, f2;
        table.hitCounts(f1, f2);
        if (deviceHitCounts) {
            std::vector<quint64> d1, d2;
            deviceHitCounts(d1, d2);
            for (size_t w = 0; w < f1.size() && w < d1.size(); ++w) {
                f1[w] += d1[w];
                f2[w] += d2[w];
            }
        }
        return Leon::chaoUnseen(f1, f2);
    };

    auto publish = [&](bool force) {
        const std::vector<quint64> found = table.countByWeight();
        m_buffers->h_spectrum.fillZero();
        m_buffers->h_spectrum[0] = 1;
        for (size_t w = 1; w < found.size() && w < g.spectrumSize; ++w)
            m_buffers->h_spectrum[w] = found[w];

        const ProgressTracker::Due due = m_progress.due();
        if (due.estimate)
            reportEstimate();
        if (due.bar)
            reportProgressBar();
        if (due.spectrum || force) {
            m_progress.markSpectrum();
            updateSpectrum(cols);

            // Вероятность пропустить хотя бы одно слово: по модели, для
            // каждого веса — найденные слова умножить на шанс пропуска
            // одного слова, поделённый на шанс поимки. Сумма по весам.
            double missTotal = 0.0;
            SpectrumFloat unseen(cols + 1, 0.0f);
            const std::vector<double> chao = chaoUnseen();
            for (int w = 1; w <= maxWeight && w <= cols; ++w) {
                const double p = Leon::catchProbabilityFor(cols, rows, w, depth, g.leonWindow);
                const double q = std::exp(double(collected) * std::log1p(-p));   // (1-p)^collected
                if (found[size_t(w)] > 0)
                    missTotal += double(found[size_t(w)]) * q / std::max(1.0 - q, 1e-300);
                unseen[w] = float(chao[size_t(w)]);
            }
            emit searchEstimate(maxWeight, collected, target,
                                std::min(1.0, missTotal), unseen);
        }
    };

    auto checkMemory = [&]() {
        if (table.bytes() > kTableLimitBytes) {
            publish(true);
            throw std::runtime_error(QStringLiteral(
                "слишком много слов до заданного веса: таблица (%1 слов) не помещается в "
                "отведённые %2 МБ — уменьшите вес или поднимите память в настройках поиска")
                .arg(table.size()).arg(kTableLimitBytes >> 20).toStdString());
        }
    };

    // ------------------------------------------------------------- CPU
    if (!g.useGpu) {
        // Пачка попыток на один проход: достаточно мелкая, чтобы отмена и
        // пауза отзывались быстро, и достаточно крупная, чтобы потоки не
        // простаивали.
        const quint64 batch = quint64(std::max(1, omp_get_max_threads())) * 16;

        while (launched < target) {
            if (!waitWhilePaused())
                break;
            const quint64 count = std::min(batch, target - launched);

            #pragma omp parallel for schedule(dynamic)
            for (long long t = 0; t < (long long)count; ++t) {
                auto visit = [&](const quint64* word, int weight) { table.add(word, weight); };
                if (g.leonWindow > 0)
                    Leon::trialStern(m_buffers->h_matrix.get(), rows, cols, words, depth, g.leonWindow, maxWeight,
                                     launched + quint64(t), visit);
                else
                    Leon::trial(m_buffers->h_matrix.get(), rows, cols, words, depth, maxWeight,
                                launched + quint64(t), visit);
            }

            launched  += count;
            collected  = launched;
            m_activeTrials = collected;
            m_progress.addOps(quint64(double(count) * g.leonWordsPerTrial));
            m_runState.doneOps = m_progress.doneOps();

            retarget();
            checkMemory();
            publish(false);
            if (m_cancelled.load())
                break;
        }
        publish(true);
        m_foundWords.clear();
        m_foundWeights.clear();
        if (m_keepFoundWords && !m_cancelled.load())
            table.exportWords(m_foundWords, m_foundWeights);
        return;
    }

    // ------------------------------------------------------------- GPU
    // Блок на попытку (LEON_THREADS нитей); сетка из настроек тут ни при чём.
    constexpr int     THREADS      = LEON_THREADS;
    constexpr quint64 BATCH_MAX    = 8192;         // попыток на запуск, потолок
    constexpr quint64 CAPACITY_MAX = 8ULL << 20;   // слов в буфере, потолок
    quint64           capacity     = 1ULL << 20;

    // Матрица, не влезшая в разделяемую память, и хеш-таблица окна у каждого
    // блока свои — в рабочем буфере. Буфер на слот не больше 128 МБ (с окном
    // — четверти свободной памяти, до гигабайта: таблица блока — мегабайты),
    // им и ограничена пачка.
    // Список пар окна — с запасом вчетверо против профиля: пар в попытке
    // столько же по порядку, что и в попытке 0, но не поровну.
    const unsigned pairCapacity = g.leonWindow > 0
        ? unsigned(std::min(64.0e6, std::max(4096.0, 4.0 * g.leonPairs))) : 0u;
    const size_t scratchWords = leonScratchWords(rows, cols, words, depth, g.leonWindow, pairCapacity);
    quint64      blocksMax    = BATCH_MAX;
    if (scratchWords > 0) {
        quint64 budget = 128ULL << 20;
        if (g.leonWindow > 0) {
            size_t freeBytes = 0, totalBytes = 0;
            CUDA_CALL(cudaMemGetInfo(&freeBytes, &totalBytes));
            budget = std::min<quint64>(1024ULL << 20, quint64(freeBytes) / 4);
        }
        blocksMax = std::max<quint64>(1, std::min<quint64>(BATCH_MAX,
                        budget / (scratchWords * sizeof(quint64))));
    }

    DeviceBuffer<quint64> d_mat;
    d_mat.allocate(size_t(rows) * words);
    CUDA_CALL(cudaMemcpy(d_mat.get(), m_buffers->h_matrix.get(), size_t(rows) * words * sizeof(quint64),
                         cudaMemcpyHostToDevice));

    struct Slot
    {
        DeviceBuffer<quint64>  d_out;
        DeviceBuffer<quint64>  d_scratch;
        DeviceBuffer<unsigned> d_count;
        HostBuffer<quint64>    h_out;
        HostBuffer<unsigned>   h_count;
        CudaStream             stream;
        quint64                first   = 0;
        quint64                count   = 0;
        bool                   pending = false;
    };
    Slot slot[2];
    auto allocateOut = [&](Slot& s) {
        s.d_out.allocate(size_t(capacity) * words);
        s.h_out.allocate(size_t(capacity) * words, HostBuffer<quint64>::Kind::Pinned);
    };
    for (Slot& s : slot) {
        allocateOut(s);
        if (scratchWords > 0)
            s.d_scratch.allocate(size_t(blocksMax) * scratchWords);
        s.d_count.allocate(1);
        s.h_count.allocate(1, HostBuffer<unsigned>::Kind::Pinned);
        s.stream.create();
    }

    // Таблица виденных слов на видеокарте. Ячейка — отпечаток (8 байт),
    // поимки (4) и вес (2). Начинается с миллиона ячеек, при заполнении 0,6
    // удваивается; не хватило памяти — остаётся как есть, а слова без ячейки
    // выкладываются при каждой поимке (см. leonkernel.cuh).
    struct Seen
    {
        DeviceBuffer<uint64_t> fp;
        DeviceBuffer<unsigned> hits;
        DeviceBuffer<uint16_t> weight;
        DeviceBuffer<unsigned> count;
        HostBuffer<unsigned>   h_count;
        quint64                capacity = 0;
        bool                   canGrow  = true;
        quint64                inserted = 0;

        bool allocate(quint64 cells)
        {
            try {
                DeviceBuffer<uint64_t> f; f.allocate(size_t(cells));
                DeviceBuffer<unsigned> h; h.allocate(size_t(cells));
                DeviceBuffer<uint16_t> w; w.allocate(size_t(cells));
                f.fillZero(); h.fillZero(); w.fillZero();
                fp = std::move(f); hits = std::move(h); weight = std::move(w);
            } catch (const std::exception&) {
                cudaGetLastError();   // снять cudaErrorMemoryAllocation
                return false;
            }
            capacity = cells;
            return true;
        }
    } seen;
    if (seen.allocate(1ULL << 20)) {
        seen.count.allocate(1);
        seen.count.fillZero();
        seen.h_count.allocate(1, HostBuffer<unsigned>::Kind::Pinned);
    }

    // Удвоение таблицы, когда занято больше 0,6. Ядра обеих пачек на время
    // перекладки должны стоять — оба потока дожидаются; удвоений за прогон
    // не больше десятка, простой не в счёт.
    auto growSeen = [&]() {
        if (!seen.canGrow || seen.capacity == 0)
            return;
        if (seen.inserted * 10 <= seen.capacity * 6)
            return;
        for (Slot& s : slot)
            CUDA_CALL(cudaStreamSynchronize(s.stream.get()));
        // Одна плотная пачка может забить таблицу с запасом — удваивать до
        // тех пор, пока занято не меньше 0,6, а не по разу.
        while (seen.inserted * 10 > seen.capacity * 6) {
            Seen bigger;
            if (!bigger.allocate(seen.capacity * 2)) {
                seen.canGrow = false;
                return;
            }
            launchSeenRehash(seen.fp.get(), seen.hits.get(), seen.weight.get(), seen.capacity,
                             bigger.fp.get(), bigger.hits.get(), bigger.weight.get(), bigger.capacity - 1,
                             slot[0].stream.get());
            CUDA_CALL(cudaStreamSynchronize(slot[0].stream.get()));
            seen.fp = std::move(bigger.fp);
            seen.hits = std::move(bigger.hits);
            seen.weight = std::move(bigger.weight);
            seen.capacity = bigger.capacity;
        }
    };

    // f1/f2 по весам из таблицы видеокарты: счётчики и веса скачиваются
    // целиком (6 байт на ячейку), поэтому не чаще раза в две секунды —
    // между скачиваниями отдаётся прошлый ответ.
    std::vector<quint64> seenF1, seenF2;
    auto seenStamp = std::chrono::steady_clock::now() - std::chrono::hours(1);
    if (seen.capacity > 0) {
        deviceHitCounts = [&](std::vector<quint64>& f1, std::vector<quint64>& f2) {
            const auto now = std::chrono::steady_clock::now();
            if (!seenF1.empty() && now - seenStamp < std::chrono::seconds(2)) {
                f1 = seenF1; f2 = seenF2;
                return;
            }
            f1.assign(size_t(maxWeight) + 1, 0ULL);
            f2.assign(size_t(maxWeight) + 1, 0ULL);
            for (Slot& s : slot)
                CUDA_CALL(cudaStreamSynchronize(s.stream.get()));
            std::vector<unsigned> hits(size_t(seen.capacity));
            std::vector<uint16_t> weight(size_t(seen.capacity));
            CUDA_CALL(cudaMemcpy(hits.data(), seen.hits.get(), hits.size() * sizeof(unsigned), cudaMemcpyDeviceToHost));
            CUDA_CALL(cudaMemcpy(weight.data(), seen.weight.get(), weight.size() * sizeof(uint16_t), cudaMemcpyDeviceToHost));
            for (size_t i = 0; i < hits.size(); ++i) {
                if (hits[i] == 1u && weight[i] <= maxWeight) ++f1[weight[i]];
                else if (hits[i] == 2u && weight[i] <= maxWeight) ++f2[weight[i]];
            }
            seenF1 = f1; seenF2 = f2; seenStamp = now;
        };
    }

    // Размер пачки подстраивается под плотность находок: у плотного кода
    // слов нужного веса тысячи на попытку, у редкого — доли. Первая пачка
    // маленькая — по ней и меряется.
    quint64 batchTrials  = std::min<quint64>(256, blocksMax);
    double  hitsPerTrial = 0.0;
    auto adaptBatch = [&](quint64 found, quint64 count) {
        if (count == 0)
            return;
        hitsPerTrial = std::max(hitsPerTrial, double(found) / double(count));
        const double room = double(capacity) / 4.0 / std::max(1.0, hitsPerTrial);
        batchTrials = quint64(std::min(double(blocksMax), std::max(1.0, room)));
    };

    // Пачки, которые пришлось отложить: переполнившаяся делится пополам, и
    // вторая половина ждёт своей очереди здесь.
    std::vector<std::pair<quint64, quint64>> deferred;

    auto launchBatch = [&](Slot& s, quint64 first, quint64 count) {
        CUDA_CALL(cudaMemsetAsync(s.d_count.get(), 0, sizeof(unsigned), s.stream.get()));
        LeonLaunch launch;
        launch.matrix       = d_mat.get();
        launch.rows         = rows;
        launch.cols         = cols;
        launch.wordsPerRow  = words;
        launch.rowsPerTrial = depth;
        launch.maxWeight    = maxWeight;
        launch.firstTrial   = first;
        launch.trials       = int(count);
        launch.outWords     = s.d_out.get();
        launch.outCount     = s.d_count.get();
        launch.capacity     = unsigned(capacity);
        launch.scratch      = s.d_scratch.get();
        launch.window       = g.leonWindow;
        launch.pairCapacity = pairCapacity;
        if (seen.capacity > 0) {
            launch.seenFp     = seen.fp.get();
            launch.seenHits   = seen.hits.get();
            launch.seenWeight = seen.weight.get();
            launch.seenCount  = seen.count.get();
            launch.seenMask   = seen.capacity - 1;
        }
        launchLeonTrials(launch, THREADS, s.stream.get());
        // Счётчик здесь не копируется. Копии всех потоков стоят в одной
        // очереди движка копирования, и четыре байта, поставленные за ядром
        // этой пачки, задержали бы за собой мегабайты соседней: та ждала бы
        // конца чужого ядра, а видеокарта — хоста (профиль: занята 37 %).
        s.first = first; s.count = count; s.pending = true;
    };

    // Забирает слова пачки в таблицу. false — буфер оказался мал: слова
    // сверх него потеряны, пачку надо повторить.
    auto collectBatch = [&](Slot& s) -> bool {
        // Ядро пачки закончилось — только теперь копии, и они идут сразу.
        CUDA_CALL(cudaStreamSynchronize(s.stream.get()));
        CUDA_CALL(cudaMemcpyAsync(s.h_count.get(), s.d_count.get(), sizeof(unsigned),
                                  cudaMemcpyDeviceToHost, s.stream.get()));
        if (seen.capacity > 0)
            CUDA_CALL(cudaMemcpyAsync(seen.h_count.get(), seen.count.get(), sizeof(unsigned),
                                      cudaMemcpyDeviceToHost, s.stream.get()));
        CUDA_CALL(cudaStreamSynchronize(s.stream.get()));
        const unsigned found = s.h_count[0];
        if (seen.capacity > 0)
            seen.inserted = seen.h_count[0];
        // Переполнение. Без таблицы пачка отбрасывается целиком и идёт
        // заново. С таблицей так нельзя: слова, что поместились, уже
        // записаны в таблицу и при повторе пачки хосту не выложатся —
        // поэтому поместившаяся часть (она целая: место занимается до
        // записи) забирается сейчас, а повтор доложит остальное.
        const bool     overflow = quint64(found) > capacity;
        const unsigned usable   = overflow ? (seen.capacity > 0 ? unsigned(capacity) : 0u) : found;
        if (usable > 0) {
            // В потоке пачки, не в нулевом: синхронный cudaMemcpy ждал бы
            // и ядро соседней пачки.
            CUDA_CALL(cudaMemcpyAsync(s.h_out.get(), s.d_out.get(),
                                      size_t(usable) * words * sizeof(quint64),
                                      cudaMemcpyDeviceToHost, s.stream.get()));
            CUDA_CALL(cudaStreamSynchronize(s.stream.get()));
            // С таблицей на видеокарте поимки посчитаны там; здесь слово
            // только хранится.
            table.addBatch(s.h_out.get(), usable, seen.capacity == 0);
        }
        adaptBatch(found, s.count);
        if (overflow)
            return false;
        s.pending  = false;
        collected += s.count;
        return true;
    };

    // Переполнение. Пока буфер можно увеличить — увеличивается (оба сразу,
    // они одного размера); упёрлись в потолок — пачка делится пополам, и
    // вторая половина откладывается. Чужую пачку сначала забрать: иначе её
    // слова пропадут вместе со старым буфером; не влезла и она — повторится
    // тем же порядком.
    auto shrinkOrGrow = [&](Slot& s, quint64 need) {
        // Счётчик ядра считает все находки, и за пределами буфера тоже, —
        // плотность по нему честная.
        adaptBatch(need, s.count);
        if (need <= CAPACITY_MAX && capacity < CAPACITY_MAX) {
            capacity = std::min(CAPACITY_MAX, std::max(capacity * 2, need + need / 4 + 1024));
            return;
        }
        if (s.count <= 1) {
            // С таблицей повтор выложит только то, чего в ней ещё нет, —
            // с каждым разом меньше; без таблицы повтор даст то же самое.
            if (seen.capacity > 0 && seen.canGrow)
                return;
            throw std::runtime_error(
                "одна попытка даёт больше восьми миллионов слов до заданного веса: уменьшите вес");
        }
        const quint64 half = s.count / 2;
        deferred.push_back({ s.first + half, s.count - half });
        s.count = half;
    };
    auto collectOrRetry = [&](Slot& s) {
        while (!collectBatch(s)) {
            const quint64 before = capacity;
            Slot& other = (&s == &slot[0]) ? slot[1] : slot[0];
            bool rerunOther = false;
            if (other.pending && !collectBatch(other)) {
                rerunOther = true;
                shrinkOrGrow(other, other.h_count[0]);
            }
            shrinkOrGrow(s, s.h_count[0]);
            if (capacity != before) {
                allocateOut(slot[0]);
                allocateOut(slot[1]);
            }
            if (rerunOther)
                launchBatch(other, other.first, other.count);
            launchBatch(s, s.first, s.count);
        }
    };

    // Следующий кусок работы: сначала отложенное, потом новые попытки.
    auto takeRange = [&](quint64& first, quint64& count) -> bool {
        if (!deferred.empty()) {
            const std::pair<quint64, quint64> range = deferred.back();
            deferred.pop_back();
            first = range.first;
            count = std::min(range.second, batchTrials);
            if (range.second > count)
                deferred.push_back({ first + count, range.second - count });
            return true;
        }
        if (launched >= target)
            return false;
        first = launched;
        count = std::min(batchTrials, target - launched);
        launched += count;
        m_progress.addOps(quint64(double(count) * g.leonWordsPerTrial));
        m_runState.doneOps = m_progress.doneOps();
        return true;
    };

    int cur = 0;
    for (;;) {
        quint64 first = 0, count = 0;
        if (!takeRange(first, count)) {
            // Всё запущено — дождаться хвоста и решить, не нужно ли ещё.
            for (Slot& s : slot)
                if (s.pending)
                    collectOrRetry(s);
            m_activeTrials = collected;
            retarget();
            checkMemory();
            if (deferred.empty() && launched >= target)
                break;
            continue;
        }
        if (!waitWhilePaused())
            break;

        Slot& s = slot[cur];
        if (s.pending)
            collectOrRetry(s);
        growSeen();
        launchBatch(s, first, count);
        m_activeTrials = collected;

        retarget();
        checkMemory();
        publish(false);
        cur ^= 1;
        if (m_cancelled.load())
            break;
    }

    // Буферы освобождаются деструкторами; ядра к этому моменту должны
    // закончиться — иначе они писали бы в уже отданную память.
    for (Slot& s : slot)
        CUDA_CALL(cudaStreamSynchronize(s.stream.get()));
    m_activeTrials = collected;
    publish(true);
    m_foundWords.clear();
    m_foundWeights.clear();
    if (m_keepFoundWords && !m_cancelled.load())
        table.exportWords(m_foundWords, m_foundWeights);
}
