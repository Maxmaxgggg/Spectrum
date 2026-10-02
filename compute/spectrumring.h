#pragma once

#include "cudabuffers.h"

// Кольцо снимков спектра для интерфейса.
//
// Зачем. Спектр показывается по ходу расчёта, и копия с видеокарты ставится в
// тот же поток, что и ядра, — то есть за те ядра, что хост уже успел туда
// наставить. Хост убегает вперёд примерно на три чанка, поэтому копия доезжает
// только через три чанка после того, как её поставили.
//
// Раньше буфер и событие были в единственном числе, и следующую копию нельзя
// было ставить, пока не забрали предыдущую: вторая писала бы в тот же буфер
// поверх первой, а перезаписанное событие потеряло бы след первой. Получалась
// одна отправка на четыре чанка — три ждём, на четвёртом забираем и ставим
// следующую.
//
// Задержка в три чанка неустранима: копия не обгонит очередь. Но пропускная
// способность к задержке отношения не имеет. Здесь копий несколько, они едут
// по кольцу одновременно, и готова одна на каждый чанк — вчетверо чаще.
// Замерено режимом --rate: на длинном пути 6,3 отправки в секунду против
// 24 после этой правки.
//
// Тот же приём, что и двойная буферизация стартовых масок в длинном пути,
// только для обратного направления.
class SpectrumRing
{
public:
    // Мест в кольце. Больше четырёх смысла нет: хост убегает вперёд на три чанка, и лишние
    // места в кольце просто не успевают заполниться.
    static constexpr int MAX_SLOTS = 4;

    // values — длина спектра, copies — сколько копий может ехать одновременно.
    //
    // Имя не slots: так называется макрос Qt из qobjectdefs.h (тот самый из
    // "public slots:"), он разворачивается в пустоту и съедает параметр.
    void allocate(size_t values, int copies = MAX_SLOTS)
    {
        reset();
        m_slots  = qBound(1, copies, MAX_SLOTS);
        m_values = values;
        m_buffers.allocate(values * size_t(m_slots), HostBuffer<quint64>::Kind::Pinned);
        for (int i = 0; i < m_slots; ++i)
            m_events[i].create();
    }

    void reset()
    {
        m_buffers.reset();
        for (int i = 0; i < MAX_SLOTS; ++i)
            m_events[i].reset();
        m_slots = m_head = m_tail = m_inFlight = 0;
        m_values = 0;
    }

    bool valid() const { return m_slots > 0 && m_buffers.get() != nullptr; }

    // Поставить копию спектра в очередь. false — все места заняты, то есть
    // видеокарта не успевает за тем, как часто у неё просят снимки.
    bool enqueue(const quint64* deviceSpectrum, cudaStream_t stream)
    {
        if (!valid() || m_inFlight >= m_slots)
            return false;

        quint64* const slot = m_buffers.get() + size_t(m_head) * m_values;
        CUDA_CALL(cudaMemcpyAsync(slot, deviceSpectrum, m_values * sizeof(quint64),
                                  cudaMemcpyDeviceToHost, stream));
        CUDA_CALL(cudaEventRecord(m_events[m_head].get(), stream));

        m_head = (m_head + 1) % m_slots;
        ++m_inFlight;
        return true;
    }

    // Самая старая доехавшая копия или nullptr, если старейшая ещё в пути.
    //
    // Копии лежат в потоке по порядку, поэтому и доезжают по порядку: спектр
    // не поедет назад. Для накопительного спектра это обязательно — иначе на
    // экране мелькали бы числа то больше, то меньше.
    //
    // Указатель живёт до тех пор, пока это место в кольце не займёт новая
    // копия, то есть ещё slots-1 постановок. Забирать надо сразу.
    const quint64* takeReady()
    {
        if (m_inFlight == 0)
            return nullptr;
        if (cudaEventQuery(m_events[m_tail].get()) != cudaSuccess)
            return nullptr;

        const quint64* const slot = m_buffers.get() + size_t(m_tail) * m_values;
        m_tail = (m_tail + 1) % m_slots;
        --m_inFlight;
        return slot;
    }

private:
    HostBuffer<quint64> m_buffers;
    CudaEvent           m_events[MAX_SLOTS];

    size_t m_values   = 0;
    int    m_slots    = 0;
    int    m_head     = 0;   // куда ставить следующую
    int    m_tail     = 0;   // откуда забирать самую старую
    int    m_inFlight = 0;
};
