#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <QtGlobal>

// Учёт хода длительного расчёта: сколько операций сделано, сколько осталось,
// и что пора сделать на текущей итерации цикла.
//
// Раньше этот код был скопирован в каждую из шести функций расчёта, причём
// копии успели разойтись: где-то стояла защита от деления на ноль, где-то нет;
// счётчик выполненных операций обновлялся то до отчёта, то после.
//
// Метки времени сдвигаются не внутри проверки, а отдельными mark*(), потому
// что GPU-пути обновляют спектр иначе, чем CPU: там метка двигается только
// если асинхронное копирование действительно началось, а не при каждом
// наступлении срока.
class ProgressTracker
{
public:
    using Clock   = std::chrono::steady_clock;
    using Seconds = std::chrono::seconds;
    // Обновление спектра задаётся в миллисекундах: секунда — слишком грубый
    // шаг для показа хода расчёта, глазу заметно.
    using Millis  = std::chrono::milliseconds;

    // Что назрело к текущему моменту.
    struct Due {
        bool estimate   = false;   // пересчитать оценку времени и скорость
        bool bar        = false;   // обновить прогрессбар
        bool spectrum   = false;   // показать текущий спектр
        bool checkpoint = false;   // сохранить состояние
    };

    // Интервалы разрешено менять на ходу, из потока интерфейса, пока расчёт
    // идёт. Через очередь событий это не сделать: поток воркера весь расчёт
    // сидит внутри computeSpectrum и до своего цикла событий не возвращается,
    // а значит настройка пролежала бы там до конца. Поэтому числа атомарные —
    // их запись безопасна из любого потока и ничего не ждёт.
    //
    // Ровно так же устроена отмена расчёта: она тоже пишет атомик напрямую.
    void setIntervals(Millis spectrum, Seconds checkpoint)
    {
        m_spectrumMs        .store(int(spectrum.count()),   std::memory_order_relaxed);
        m_checkpointSeconds .store(int(checkpoint.count()), std::memory_order_relaxed);
    }

    // Сохранять чекпоинт каждые everyOps операций вместо привязки к таймеру.
    // 0 — обычный режим, по времени.
    //
    // Нужно тестам возобновления: момент срабатывания таймера от запуска к
    // запуску разный, а «каждые N операций» — всегда одна и та же точка
    // прерывания, и результат можно сравнивать побитово.
    void setOpsCheckpoint(quint64 everyOps)
    {
        m_opsCheckpoint     = everyOps;
        m_nextCheckpointOps = m_doneOps + everyOps;
    }

    // resumeElapsedSec — время, потраченное до загрузки чекпоинта: старт
    // сдвигается назад, чтобы скорость и оценка учитывали прошлый прогон.
    void begin(quint64 totalOps, quint64 doneOps, qint64 resumeElapsedSec)
    {
        m_totalOps   = totalOps;
        m_doneOps    = doneOps;
        m_elapsedSec = resumeElapsedSec;

        m_start = Clock::now() - Seconds(resumeElapsedSec);
        m_lastEstimate = m_lastBar = m_lastSpectrum = m_lastCheckpoint = m_start;

        // Режим по операциям настраивают до begin(), а doneOps здесь меняется —
        // порог надо пересчитать от новой отправной точки.
        m_nextCheckpointOps = m_doneOps + m_opsCheckpoint;
    }

    void    addOps(quint64 n)     { m_doneOps += n; }
    void    setDoneOps(quint64 n) { m_doneOps  = n; }
    // Объём работы бывает известен не сразу: случайный поиск уточняет число
    // попыток по ходу, по количеству уже найденных слов.
    void    setTotalOps(quint64 n) { m_totalOps = n; }
    quint64 doneOps() const       { return m_doneOps; }
    quint64 totalOps() const      { return m_totalOps; }

    // Снимок «что назрело». Метки не двигает — это делают mark*().
    Due due()
    {
        m_now = Clock::now();
        Due d;
        d.estimate   = (m_now - m_lastEstimate   >= Seconds(1));
        d.bar        = (m_now - m_lastBar        >= m_barInterval);
        d.spectrum   = (m_now - m_lastSpectrum
                            >= Millis(m_spectrumMs.load(std::memory_order_relaxed)));
        d.checkpoint = m_opsCheckpoint > 0
                           ? (m_doneOps >= m_nextCheckpointOps)
                           : (m_now - m_lastCheckpoint
                                  >= Seconds(m_checkpointSeconds.load(std::memory_order_relaxed)));
        return d;
    }

    void markEstimate()
    {
        m_lastEstimate = m_now;
        m_elapsedSec   = std::chrono::duration_cast<Seconds>(m_now - m_start).count();
    }
    void markBar()      { m_lastBar      = m_now; }
    void markSpectrum() { m_lastSpectrum = m_now; }
    void markCheckpoint()
    {
        m_lastCheckpoint    = m_now;
        m_nextCheckpointOps = m_doneOps + m_opsCheckpoint;
    }

    qint64 elapsedSec() const { return m_elapsedSec; }

    int percent() const
    {
        if (m_totalOps == 0) return 0;
        const quint64 done = qMin(m_doneOps, m_totalOps);
        return int(done * 100ULL / m_totalOps);
    }

    // Операций в секунду. До первой полной секунды скорость неизвестна.
    double speed() const
    {
        if (m_elapsedSec <= 0 || m_doneOps == 0) return 0.0;
        return double(m_doneOps) / double(m_elapsedSec);
    }

    int minutesLeft() const
    {
        const double s = speed();
        if (s <= 0.0) return 0;
        const quint64 remaining = m_totalOps > m_doneOps ? m_totalOps - m_doneOps : 0;
        return int(std::ceil(double(remaining) / s / 60.0));
    }

private:
    quint64 m_totalOps   = 0;
    quint64 m_doneOps    = 0;
    qint64  m_elapsedSec = 0;

    // Чекпоинт по числу операций: 0 — режим по таймеру.
    quint64 m_opsCheckpoint     = 0;
    quint64 m_nextCheckpointOps = 0;

    Clock::time_point m_start;
    Clock::time_point m_now;
    Clock::time_point m_lastEstimate;
    Clock::time_point m_lastBar;
    Clock::time_point m_lastSpectrum;
    Clock::time_point m_lastCheckpoint;

    std::atomic<int> m_spectrumMs        { 1000 };
    std::atomic<int> m_checkpointSeconds { 10 };
    const Seconds m_barInterval  { 1  };
};
