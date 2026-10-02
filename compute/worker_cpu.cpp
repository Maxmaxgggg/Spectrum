// Перебор на процессоре: код Грея (и дуальный расчёт), простой XOR и
// Брауэр–Циммерман для коротких и для длинных кодов.
//
// Цикл по слоям и чанкам — общий, Worker::runChunks; здесь только перебор
// одного чанка. Нити OpenMP копят каждая свой спектр, и в h_spectrum чанк
// попадает только целиком: прерванный отменой выбрасывается (см.
// ChunkPlan::run).

#include "worker_p.h"
#include "bitops.h"
#include "combinations.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

namespace {

// Правило единственности Брауэра–Циммермана на хосте — то же, что bzKeep в
// ядре: слово из r строк множества setIndex засчитывается, если прежние
// множества видят у него больше r единиц, а последующие — не меньше r.
inline bool bzKeepHost(const quint64* codeword, const CodeGeometry& g, int r, int setIndex)
{
    for (int i = 0; i < g.setCount; ++i) {
        if (i == setIndex)
            continue;
        const quint64* mask = g.setMasks.data() + size_t(i) * g.wordsPerRow;
        int ones = 0;
        for (quint64 w = 0; w < g.wordsPerRow; ++w)
            ones += BitOps::popcount64(codeword[w] & mask[w]);
        if (i < setIndex ? ones <= r : ones < r)
            return false;
    }
    return true;
}

} // namespace

void Worker::computeGrayCpu(const CodeGeometry& g)
{
    ChunkPlan plan;
    plan.layered     = false;
    plan.totalOps    = 1ULL << g.numOfRows;
    plan.layerSize   = [&g](quint64) { return quint64(1) << g.numOfRows; };
    plan.chunkTarget = g.chunkSize;
    plan.run         = [this, &g](quint64, const LayerSlice& s) { return grayChunkCpu(g, s); };
    runChunks(g, plan);
}

void Worker::computeXorCpuShort(const CodeGeometry& g)
{
    ChunkPlan plan;
    plan.totalOps    = totalLayerOps(g);
    plan.lastLayer   = g.maxRows;
    plan.layerSize   = [this, &g](quint64 r) { return m_binomTable(g.numOfRows, r); };
    plan.chunkTarget = g.chunkSize;
    plan.run         = [this, &g](quint64 r, const LayerSlice& s) { return xorChunkCpuShort(g, r, s); };
    runChunks(g, plan);
    updateSpectrum(int(g.numOfCols));
}

void Worker::computeXorCpuLong(const CodeGeometry& g)
{
    // Масок на нить в чанке
    const quint64 masksPerThread = 1ULL << 20;

    ChunkPlan plan;
    plan.totalOps    = totalLayerOps(g);
    plan.lastLayer   = g.maxRows;
    plan.layerSize   = [this, &g](quint64 r) { return m_binomTable(g.numOfRows, r); };
    plan.chunkTarget = quint64(omp_get_max_threads()) * masksPerThread;
    plan.run         = [this, &g, masksPerThread](quint64 r, const LayerSlice& s) {
        return xorChunkCpuLong(g, r, s, masksPerThread);
    };
    runChunks(g, plan);
    updateSpectrum(int(g.numOfCols));
}

// Код Грея: маски чанка [s.offset, s.offset + s.size) делятся между нитями
// поровну, соседние маски отличаются одним битом — одна строка на шаг.
bool Worker::grayChunkCpu(const CodeGeometry& g, const LayerSlice& s)
{
    const quint64 numOfCols   = g.numOfCols;
    const quint64 wordsPerRow = g.wordsPerRow;
    const quint64 chunkStart  = s.offset;
    const quint64 chunkEnd    = s.offset + s.size;

    // Код Грея номера i
    auto gray = [](quint64 i) -> quint64 { return i ^ (i >> 1); };

    std::vector<quint64> chunkSpectrum(numOfCols + 1, 0);

    #pragma omp parallel
    {
        const int tid      = omp_get_thread_num();
        const int nthreads = omp_get_num_threads();

        // Кусок чанка этой нити
        const quint64 perThread = (s.size + nthreads - 1) / nthreads;
        const quint64 startIdx  = chunkStart + tid * perThread;
        const quint64 endIdx    = std::min(startIdx + perThread, chunkEnd);

        if (startIdx < endIdx) {
            std::vector<quint64> localCodeword(wordsPerRow, 0);
            std::vector<quint64> localSpectrum(numOfCols + 1, 0);

            // Первая маска нити: XOR всех её строк
            quint64 mask = gray(startIdx);
            for (quint64 tmp = mask; tmp; tmp &= tmp - 1) {
                const quint64* rowData = m_buffers->h_matrix.get() + BitOps::lowestSetBit(tmp) * wordsPerRow;
                for (quint64 b = 0; b < wordsPerRow; ++b)
                    localCodeword[b] ^= rowData[b];
            }
            quint64 weight = 0;
            for (quint64 b = 0; b < wordsPerRow; ++b)
                weight += BitOps::popcount64(localCodeword[b]);
            if (weight <= numOfCols)
                localSpectrum[weight]++;

            for (quint64 i = startIdx + 1; i < endIdx; ++i) {
                if (m_cancelled.load())
                    break;
                while (m_paused.load() != 0) {
                    QThread::msleep(50);
                    if (m_cancelled.load())
                        break;
                }

                // Соседние коды Грея отличаются ровно одним битом
                const quint64 next = gray(i);
                const quint64* rowData = m_buffers->h_matrix.get() + BitOps::lowestSetBit(mask ^ next) * wordsPerRow;
                for (quint64 b = 0; b < wordsPerRow; ++b)
                    localCodeword[b] ^= rowData[b];
                mask = next;

                weight = 0;
                for (quint64 b = 0; b < wordsPerRow; ++b)
                    weight += BitOps::popcount64(localCodeword[b]);
                if (weight <= numOfCols)
                    localSpectrum[weight]++;
            }

            #pragma omp critical
            {
                for (quint64 w = 0; w <= numOfCols; ++w)
                    chunkSpectrum[w] += localSpectrum[w];
            }
        }
    }

    // Отмена посреди чанка: нити бросили перебор, не дойдя до конца, и часть
    // масок не перебрана. Такой чанк выбрасывается целиком — ни в спектр, ни
    // в чекпоинт. Флаг отмены за время расчёта не сбрасывается, поэтому если
    // его видела хоть одна нить, видно и здесь.
    if (m_cancelled.load())
        return false;
    for (quint64 w = 0; w <= numOfCols; ++w)
        m_buffers->h_spectrum[w] += chunkSpectrum[w];
    return true;
}

// Простой XOR и Брауэр–Циммерман, k <= 63: каждая нить берёт непрерывный
// кусок номеров и идёт по нему приёмом Госпера — так же, как ядро коротких
// кодов. Номер разбирается один раз на нить, дальше маска шагает за
// несколько операций, и XOR-ятся только изменившиеся строки.
//
// Нумерация сочетаний лексикографическая — её задаёт смысл chunkOffset, и
// менять её нельзя. Госпер идёт по числовому порядку, но развёрнутая по
// битам маска (позиция p -> k-1-p) превращает одно в другое. Поэтому берётся
// последний номер куска, и обход идёт в обратную сторону: для гистограммы
// порядок безразличен.
//
// Отмена проверяется только между чанками: чанк короткий, и задержка отмены
// незаметна.
bool Worker::xorChunkCpuShort(const CodeGeometry& g, quint64 r, const LayerSlice& s)
{
    const quint64 numOfRows   = g.numOfRows;
    const quint64 numOfCols   = g.numOfCols;
    const quint64 wordsPerRow = g.wordsPerRow;

    QVector<quint64> chunkSpectrum(numOfCols + 1, 0);

    #pragma omp parallel
    {
        const int tid      = omp_get_thread_num();
        const int nthreads = omp_get_num_threads();

        const quint64 perThread = (s.size + nthreads - 1) / nthreads;
        const quint64 startIdx  = s.offset + quint64(tid) * perThread;
        const quint64 endIdx    = std::min(startIdx + perThread, s.offset + s.size);

        if (startIdx < endIdx) {
            QVector<quint64> localSpectrum(numOfCols + 1, 0);
            QVector<quint64> localCodeword(wordsPerRow, 0);

            quint64 revMask = BitOps::reverseLowBits(
                Combinations::unrankMask(unsigned(numOfRows), unsigned(r), endIdx - 1, m_binomTable),
                int(numOfRows));

            // Бит p развёрнутой маски отвечает строке numOfRows-1-p.
            auto xorRows = [&](quint64 bits) {
                while (bits) {
                    const int p = BitOps::lowestSetBit(bits);
                    bits &= (bits - 1);
                    const quint64* rowData =
                        m_buffers->h_matrix.get() + (s.slot.rowBase + numOfRows - 1 - p) * wordsPerRow;
                    for (quint64 b = 0; b < wordsPerRow; ++b)
                        localCodeword[b] ^= rowData[b];
                }
            };
            auto accumulate = [&]() {
                quint64 weight = 0;
                for (quint64 b = 0; b < wordsPerRow; ++b)
                    weight += BitOps::popcount64(localCodeword[b]);
                if (weight <= numOfCols
                    && (g.setCount <= 1
                        || bzKeepHost(localCodeword.data(), g, int(r), s.slot.setIndex)))
                    localSpectrum[weight]++;
            };

            xorRows(revMask);
            accumulate();

            // При r = 0 и r = numOfRows сочетание одно, и цикл не выполняется —
            // gosperNext на нулевой маске звать нельзя.
            const quint64 count = endIdx - startIdx;
            for (quint64 i = 1; i < count; ++i) {
                const quint64 nextRev = BitOps::gosperNext(revMask);
                xorRows(revMask ^ nextRev);
                accumulate();
                revMask = nextRev;
            }

            #pragma omp critical
            {
                for (quint64 w = 0; w <= numOfCols; ++w)
                    chunkSpectrum[w] += localSpectrum[w];
            }
        }
    }

    for (quint64 w = 0; w <= numOfCols; ++w)
        m_buffers->h_spectrum[w] += chunkSpectrum[w];
    return true;
}

// Простой XOR и Брауэр–Циммерман, k >= 64: маска в одно слово не помещается,
// сочетание — массив позиций. Нить берёт masksPerThread сочетаний подряд от
// своего стартового номера и шагает nextPositions, XOR-я только изменившиеся
// строки.
bool Worker::xorChunkCpuLong(const CodeGeometry& g, quint64 r, const LayerSlice& s,
                             quint64 masksPerThread)
{
    const quint64 numOfRows   = g.numOfRows;
    const quint64 numOfCols   = g.numOfCols;
    const quint64 wordsPerRow = g.wordsPerRow;
    const quint64 chunkSize   = s.size;
    const int     numThreads  = omp_get_max_threads();

    const quint64 numStartMasks = (chunkSize + masksPerThread - 1ULL) / masksPerThread;
    if (numStartMasks == 0)
        return true;

    std::vector<std::vector<quint64>> threadSpectrum(numThreads, std::vector<quint64>(numOfCols + 1, 0ULL));

    #pragma omp parallel
    {
        auto& localSpectrum = threadSpectrum[omp_get_thread_num()];

        // Позиции единиц: текущее сочетание, прежнее и их разность
        int16_t a[Constants::MAX_POSITIONS];
        int16_t oldA[Constants::MAX_POSITIONS];
        int16_t changed[2 * Constants::MAX_POSITIONS];

        std::vector<quint64> codeword(size_t(wordsPerRow), 0ULL);

        auto rowOf = [&](int row) {
            return m_buffers->h_matrix.get() + quint64(s.slot.rowBase + row) * wordsPerRow;
        };
        auto rebuild = [&]() {
            std::fill(codeword.begin(), codeword.end(), 0ULL);
            for (int i = 0; i < int(r); ++i) {
                const quint64* rowData = rowOf(a[i]);
                for (size_t w = 0; w < size_t(wordsPerRow); ++w)
                    codeword[w] ^= rowData[w];
            }
        };
        auto accumulate = [&]() {
            quint64 weight = 0;
            for (size_t w = 0; w < size_t(wordsPerRow); ++w)
                weight += BitOps::popcount64(codeword[w]);
            if (weight <= numOfCols
                && (g.setCount <= 1 || bzKeepHost(codeword.data(), g, int(r), s.slot.setIndex)))
                localSpectrum[size_t(weight)]++;
        };

        // Счётчик знаковый: OpenMP до версии 3.0 не допускает беззнаковых
        // индексов в parallel for. numStartMasks заведомо помещается в
        // int64_t, поэтому приводим его, а не счётчик.
        #pragma omp for schedule(dynamic)
        for (int64_t gtid = 0; gtid < int64_t(numStartMasks); ++gtid) {
            if (m_cancelled.load())
                continue;

            // Стартовый номер и сколько сочетаний реально нужно
            const quint64 startRank = quint64(gtid) * masksPerThread;
            if (startRank >= chunkSize)
                continue;
            const quint64 iters = std::min(masksPerThread, chunkSize - startRank);

            // Номер внутри своего множества
            Combinations::unrankPositions(s.offset + startRank, int(numOfRows), int(r), a,
                                          Constants::MAX_POSITIONS, m_binomTable);
            rebuild();
            accumulate();

            for (quint64 it = 1; it < iters; ++it) {
                if (m_cancelled.load())
                    break;
                while (m_paused.load() != 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    if (m_cancelled.load())
                        break;
                }
                if (m_cancelled.load())
                    break;

                std::copy(a, a + r, oldA);
                if (!Combinations::nextPositions(a, int(r), int(numOfRows)))
                    break;   // сочетания слоя кончились

                int numChanged = 0;
                Combinations::diffPositions(oldA, a, int(r), changed, numChanged);

                // Изменилось больше половины — дешевле собрать слово заново
                if (numChanged > int(r)) {
                    rebuild();
                } else {
                    for (int t = 0; t < numChanged; ++t) {
                        const quint64* rowData = rowOf(changed[t]);
                        for (size_t w = 0; w < size_t(wordsPerRow); ++w)
                            codeword[w] ^= rowData[w];
                    }
                }
                accumulate();
            }
        }
    }

    // Отмена посреди чанка: часть сочетаний не перебрана, и чанк выбрасывается
    // целиком — ни в спектр, ни в чекпоинт (см. grayChunkCpu).
    if (m_cancelled.load())
        return false;
    for (int t = 0; t < numThreads; ++t)
        for (quint64 w = 0; w <= numOfCols; ++w)
            m_buffers->h_spectrum[w] += threadSpectrum[size_t(t)][w];
    return true;
}
