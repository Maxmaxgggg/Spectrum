// Перебор на видеокарте: код Грея (и дуальный расчёт), простой XOR и
// Брауэр–Циммерман для коротких и для длинных кодов.
//
// Цикл по слоям и чанкам — общий, Worker::runChunks; здесь только запуск
// ядра на чанк. Ядра одного расчёта идут в один поток (WorkerBuffers::stream):
// спектр копится атомарно в d_spectrum, и порядок запусков задаёт сам поток.

#include "worker_p.h"
#include "combinations.h"

#include <cassert>

void Worker::computeSpectrumGpuGrayShort(const CodeGeometry& g)
{
    ChunkPlan plan;
    plan.layered     = false;
    plan.totalOps    = 1ULL << g.numOfRows;
    plan.layerSize   = [&g](quint64) { return quint64(1) << g.numOfRows; };
    plan.chunkTarget = g.chunkSize;
    plan.stream      = buffers->stream.get();
    plan.run = [this, &g](quint64, const LayerSlice& s) {
        launchSpectrumKernelGrayShort(g.blocksGpu, g.threadsGpu, buffers->stream.get(), buffers->d_spectrum.get(),
                                      int(g.numOfCols), int(g.numOfRows), int(g.wordsPerRow),
                                      s.offset, s.size);
        return true;
    };
    runChunks(g, plan);
    copySpectrumFromDevice(int(g.numOfCols));
    updateSpectrum(int(g.numOfCols));
}

void Worker::computeSpectrumGpuNoGrayShort(const CodeGeometry& g)
{
    ChunkPlan plan;
    plan.totalOps    = totalLayerOps(g);
    plan.lastLayer   = g.maxRows;
    plan.layerSize   = [this, &g](quint64 r) { return binomTable(g.numOfRows, r); };
    plan.chunkTarget = g.chunkSize;
    plan.stream      = buffers->stream.get();
    plan.run = [this, &g](quint64 r, const LayerSlice& s) {
        launchSpectrumKernelShort(buffers->d_spectrum.get(), buffers->d_binomTable.get(), g.blocksGpu, g.threadsGpu,
                                  buffers->stream.get(), int(g.numOfCols), int(g.numOfRows), int(g.wordsPerRow),
                                  s.offset, s.size, r, s.slot);
        return true;
    };
    runChunks(g, plan);
    copySpectrumFromDevice(int(g.numOfCols));
    updateSpectrum(int(g.numOfCols));
}

// Длинные коды (k >= 64): нить ядра берёт masksPerThread сочетаний подряд от
// своего стартового сочетания. Стартовые сочетания разбирает хост — по
// номеру, массивом позиций — и копирует на устройство перед каждым чанком.
void Worker::computeSpectrumGpuNoGrayLong(const CodeGeometry& g)
{
    const quint64 numOfRows       = g.numOfRows;
    const int     threadsPerBlock = g.threadsGpu;

    // Нитей в сетке и сочетаний на нить
    const quint64 maxThreads     = std::max<quint64>(1, quint64(g.blocksGpu) * quint64(g.threadsGpu));
    const quint64 masksPerThread = 1ULL << 12;

    // Стартовые сочетания: по MAX_POSITIONS позиций на нить.
    //
    // На хосте буферов два: пока с одного идёт копирование на устройство, хост
    // заполняет второй. Иначе он затирал бы данные под работающим DMA, и
    // приходилось бы после каждого чанка дожидаться устройства целиком.
    //
    // На устройстве достаточно одного: копия следующего чанка ставится в
    // очередь того же потока после ядра текущего и раньше не начнётся.
    const size_t slotBytes      = Constants::MAX_POSITIONS * sizeof(int16_t);
    const size_t slotsPerBuffer = maxThreads * Constants::MAX_POSITIONS;

    HostBuffer<int16_t>   h_slots;
    DeviceBuffer<int16_t> d_slots;
    h_slots.allocate(2 * slotsPerBuffer, HostBuffer<int16_t>::Kind::Pinned);
    d_slots.allocate(slotsPerBuffer);

    // Событие на каждый буфер: отмечает конец копирования именно из него.
    CudaEvent slotsCopied[2];
    slotsCopied[0].create();
    slotsCopied[1].create();
    bool slotsBusy[2] = { false, false };
    int  slotsBuf     = 0;

    #ifdef _DEBUG
    // Счётчик обработанных масок для сверки
    HostBuffer<uint64_t>   h_maskCounter;
    DeviceBuffer<uint64_t> d_maskCounter;
    h_maskCounter.allocate(1, HostBuffer<uint64_t>::Kind::Pinned);
    d_maskCounter.allocate(1);
    #endif

    ChunkPlan plan;
    plan.totalOps    = totalLayerOps(g);
    plan.lastLayer   = g.maxRows;
    plan.layerSize   = [this, &g](quint64 r) { return binomTable(g.numOfRows, r); };
    plan.chunkTarget = maxThreads * masksPerThread;
    plan.stream      = buffers->stream.get();
    plan.run = [&](quint64 r, const LayerSlice& slice) {
        const quint64 chunkSize = slice.size;

        // Нитей, у которых в этом чанке есть работа
        const quint64 numStartMasks = (chunkSize + masksPerThread - 1ULL) / masksPerThread;
        if (numStartMasks == 0)
            return true;

        // Ждём, пока освободится буфер, который сейчас будем переписывать. Через
        // чанк после его использования копирование давно закончилось, так что
        // ожидание холостое.
        if (slotsBusy[slotsBuf])
            CUDA_CALL(cudaEventSynchronize(slotsCopied[slotsBuf].get()));

        // Имя не slots: так называется макрос Qt из qobjectdefs.h (тот самый из
        // "public slots:"), он раскрывается в пустоту и ломает объявление.
        int16_t* const hostSlots = h_slots.get() + slotsBuf * slotsPerBuffer;

        // Стартовые сочетания разбираются на CPU — это и есть та работа, ради
        // совмещения которой с расчётом заведён второй буфер.
        for (quint64 tid = 0; tid < numStartMasks; ++tid) {
            const quint64 rank = slice.offset + tid * masksPerThread;
            assert(rank < binomTable(numOfRows, r));
            Combinations::unrankPositions(rank, int(numOfRows), int(r),
                                          hostSlots + tid * Constants::MAX_POSITIONS,
                                          Constants::MAX_POSITIONS, binomTable);
        }

        CUDA_CALL(cudaMemcpyAsync(d_slots.get(), hostSlots, numStartMasks * slotBytes,
                                  cudaMemcpyHostToDevice, buffers->stream.get()));
        CUDA_CALL(cudaEventRecord(slotsCopied[slotsBuf].get(), buffers->stream.get()));
        slotsBusy[slotsBuf] = true;
        slotsBuf ^= 1;

        const int grid = std::max<int>(1, int((numStartMasks + threadsPerBlock - 1) / threadsPerBlock));

        #ifdef _DEBUG
        d_maskCounter.fillZero();
        uint64_t* const maskCounter = d_maskCounter.get();
        #else
        uint64_t* const maskCounter = nullptr;
        #endif
        launchSpectrumKernelLong(grid, threadsPerBlock, buffers->stream.get(), buffers->d_spectrum.get(), buffers->d_matrix.get(),
                                 int(g.numOfCols), int(numOfRows), int(g.wordsPerRow), chunkSize,
                                 d_slots.get(), masksPerThread, numStartMasks, r, maskCounter,
                                 slice.slot);
        // Синхронизации после ядра нет: хост переписывает только тот буфер
        // стартовых сочетаний, с которого копирование уже закончилось, а
        // остальное упорядочено самим потоком.

        #ifdef _DEBUG
        CUDA_CALL(cudaMemcpy(h_maskCounter.get(), d_maskCounter.get(), sizeof(uint64_t),
                             cudaMemcpyDeviceToHost));
        if (h_maskCounter[0] != chunkSize)
            throw std::runtime_error("расхождение числа обработанных масок");
        #endif
        return true;
    };
    runChunks(g, plan);
    copySpectrumFromDevice(int(g.numOfCols));
    updateSpectrum(int(g.numOfCols));
    // Освобождать вручную нечего: буферы, события и счётчик владеющие — их
    // снимут деструкторы, в том числе при исключении.
}

// Итоговая копия спектра с видеокарты.
void Worker::copySpectrumFromDevice(int numOfCols)
{
    CUDA_CALL(cudaMemcpyAsync(buffers->h_spectrum.get(), buffers->d_spectrum.get(), size_t(numOfCols + 1) * sizeof(quint64),
                              cudaMemcpyDeviceToHost, buffers->stream.get()));
    CUDA_CALL(cudaStreamSynchronize(buffers->stream.get()));
}
