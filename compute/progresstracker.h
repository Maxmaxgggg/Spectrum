#pragma once

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
    using clock   = std::chrono::steady_clock;
    using seconds = std::chrono::seconds;

    // Что назрело к текущему моменту.
    struct Due {
        bool estimate   = false;   // пересчитать оценку времени и скорость
        bool bar        = false;   // обновить прогрессбар
        bool spectrum   = false;   // показать текущий спектр
        bool checkpoint = false;   // сохранить состояние
    };

    void setIntervals(seconds spectrum, seconds checkpoint)
    {
        m_spectrumInterval   = spectrum;
        m_checkpointInterval = checkpoint;
    }

    // resumeElapsedSec — время, потраченное до загрузки чекпоинта: старт
    // сдвигается назад, чтобы скорость и оценка учитывали прошлый прогон.
    void begin(quint64 totalOps, quint64 doneOps, qint64 resumeElapsedSec)
    {
        m_totalOps   = totalOps;
        m_doneOps    = doneOps;
        m_elapsedSec = resumeElapsedSec;

        m_start = clock::now() - seconds(resumeElapsedSec);
        m_lastEstimate = m_lastBar = m_lastSpectrum = m_lastCheckpoint = m_start;
    }

    void    addOps(quint64 n)     { m_doneOps += n; }
    void    setDoneOps(quint64 n) { m_doneOps  = n; }
    quint64 doneOps() const       { return m_doneOps; }
    quint64 totalOps() const      { return m_totalOps; }

    // Снимок «что назрело». Метки не двигает — это делают mark*().
    Due due()
    {
        m_now = clock::now();
        Due d;
        d.estimate   = (m_now - m_lastEstimate   >= seconds(1));
        d.bar        = (m_now - m_lastBar        >= m_barInterval);
        d.spectrum   = (m_now - m_lastSpectrum   >= m_spectrumInterval);
        d.checkpoint = (m_now - m_lastCheckpoint >= m_checkpointInterval);
        return d;
    }

    void markEstimate()
    {
        m_lastEstimate = m_now;
        m_elapsedSec   = std::chrono::duration_cast<seconds>(m_now - m_start).count();
    }
    void markBar()        { m_lastBar        = m_now; }
    void markSpectrum()   { m_lastSpectrum   = m_now; }
    void markCheckpoint() { m_lastCheckpoint = m_now; }

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

    clock::time_point m_start;
    clock::time_point m_now;
    clock::time_point m_lastEstimate;
    clock::time_point m_lastBar;
    clock::time_point m_lastSpectrum;
    clock::time_point m_lastCheckpoint;

    seconds m_spectrumInterval   { 1  };
    seconds m_checkpointInterval { 10 };
    const seconds m_barInterval  { 1  };
};
