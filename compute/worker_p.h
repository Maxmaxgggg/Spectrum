#pragma once

// Внутренности Worker — только для его собственных .cpp.
//
// В worker.h этого нет: его видит интерфейс, и ради памяти на видеокарте
// весь интерфейс собирался с заголовками CUDA, OpenMP и GMP.

#include "worker.h"

#include "computeSpectrumKernel.cuh"
// Переопределяет CUDA_CALL из .cuh: там макрос звал abort(), здесь бросает.
#include "cudabuffers.h"
#include "spectrumring.h"

#include <omp.h>

#include <functional>

// Память расчёта: матрица и спектр на хосте и на видеокарте, поток ядер и
// кольцо снимков спектра.
struct WorkerBuffers
{
    CudaStream            stream;

    HostBuffer<quint64>   h_spectrum;
    // Снимки спектра для показа по ходу расчёта. h_spectrum этим не занят:
    // туда пишет чекпоинт и итоговая копия, и они должны быть точными.
    SpectrumRing          spectrumRing;

    HostBuffer<quint64>   h_matrix;

    DeviceBuffer<quint64> d_spectrum;
    DeviceBuffer<quint64> d_matrix;
    DeviceBuffer<quint64> d_binomTable;
};

// Кусок слоя, уходящий в один запуск ядра или один параллельный проход.
//
// Слой r у Брауэра–Циммермана — это C(k, r) сочетаний на каждое множество,
// подряд: сначала все сочетания первого, потом второго и так далее. Номер в
// слое (chunkOffset) сквозной, поэтому чекпоинты устроены так же, как в
// обычном расчёте. Кусок никогда не пересекает границу множества: ядру
// нужна одна матрица и один номер множества на запуск. У кода Грея слой
// один и множество одно — кусок задаёт номера масок.
struct LayerSlice
{
    MatrixSlot slot;
    quint64    offset = 0;   // номер первого сочетания внутри своего множества
    quint64    size   = 0;
};

// Что перебирать и как — для Worker::runChunks. Общий цикл ведёт слои и
// чанки, паузу и отмену, ход расчёта, снимки спектра и чекпоинты; путь
// перебора задаёт только размеры и перебор одного чанка.
struct ChunkPlan
{
    // Слои по числу складываемых строк: простой XOR и Брауэр–Циммерман. У
    // кода Грея (и дуального расчёта) слой один — все 2^k масок подряд.
    bool    layered     = true;
    quint64 lastLayer   = 0;
    quint64 totalOps    = 0;
    quint64 chunkTarget = 0;
    // Сочетаний в слое на одно множество.
    std::function<quint64(quint64 layer)> layerSize;
    // Перебор чанка. false — прерван отменой: результат чанка отброшен
    // целиком и не попадает ни в спектр, ни в чекпоинт.
    std::function<bool(quint64 layer, const LayerSlice& slice)> run;
    // Поток видеокарты, в который идут ядра; nullptr — расчёт на процессоре,
    // и спектр уже лежит в h_spectrum.
    cudaStream_t stream = nullptr;
};
