#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <QThread>
#include <cmath>
#include <qjsonobject.h>
#include <qsettings.h>

// Заголовок без CUDA, OpenMP и GMP: его видит интерфейс. Память на
// видеокарте и всё, что касается перебора по чанкам, — в worker_p.h.
#include "constants.h"
#include "settings.h"
#include "types.h"
#include "progresstracker.h"
#include "binomtable.h"
#include "infosets.h"
#include "leon.h"
#include "product.h"
#include "autosavestore.h"

enum LoadMode {
    Reset,
    FromCheckpoint,
    FromSave
};

// Всё, что выводится из настроек и матрицы до начала расчёта.
//
// Раньше эти величины были локальными переменными computeSpectrumImpl и
// передавались в вычислительные функции поштучно — по четыре-семь аргументов,
// причём chunkSize при этом перекрывал одноимённое поле класса.
struct CodeGeometry
{
    // Матрица, по которой идёт перебор. Для дуального кода это проверочная
    // матрица, а не та, что ввёл пользователь.
    QStringList matrix;

    quint64 rows    = 0;
    quint64 cols    = 0;
    // Число 64-битных слов на одну строку
    quint64 wordsPerRow  = 0;
    quint64 matrixWords  = 0;
    // Длина спектра: веса от 0 до cols включительно
    quint64 spectrumSize = 0;
    // Максимальное число складываемых строк при частичном переборе
    quint64 maxRows      = 0;
    // Число масок в одном чанке
    quint64 chunkSize    = 1 << 20;

    int  blocksGpu  = 1;
    int  threadsGpu = 1;

    bool useGpu     = false;
    // Длинные коды (больше 63 строк) считаются другими функциями: маска в одно
    // слово туда уже не помещается.
    bool isLongCode = false;
    // Матрица не влезла в константную память и лежит в глобальной
    bool matrixInGlobalMem = false;

    // Брауэр–Циммерман. Перебор идёт не по введённой матрице, а по
    // нескольким систематическим, лежащим в памяти подряд; слово засчитывает
    // одно из множеств (см. infosets.h). В обычном расчёте множество одно —
    // введённая матрица, и все поля ниже пустые.
    int                   setCount        = 1;
    // Сколько первых множеств проходят последний слой maxRows; остальные
    // перебираются до maxRows − 1 (см. infosets.h). Без Брауэра–Циммермана
    // множество одно, и слой проходится целиком.
    int                   lastLayerSets   = 1;
    std::vector<int>      setOverlaps;
    std::vector<quint64>  setMasks;         // setCount x wordsPerRow
    std::vector<quint64>  setRows;          // setCount x rows x wordsPerRow
    QVector<QVector<int>> setColumns;       // опорные столбцы — в автосохранение
    // Все слова веса меньше этого найдены. Ноль — не Брауэр–Циммерман.
    int                   guaranteedBelow = 0;
    // Брауэр–Циммерман по сдвигам у циклического кода (cyclicorbit.h): круг
    // столбцов; множество тогда одно, а слова до countUpTo считаются правилом
    // орбит, тяжелее — не считаются. Пустой круг — обычный перебор.
    Cyclic::Symmetry      bzOrbit;
    int                   countUpTo       = 0;

    // Код-произведение: компоненты, разложенные из матрицы произведения
    // (Product::factor), — matrix == productFirst ⊗ productSecond.
    QStringList           productFirst;
    QStringList           productSecond;

    // Случайный поиск: сколько попыток и сколько слов в каждой; глубина
    // перебора в попытке лежит в maxRows.
    quint64               leonTrials        = 0;
    double                leonWordsPerTrial = 0.0;
    int                   leonWindow        = 0;     // окно Штерна–Дюмера; 0 — без окна
    double                leonPairs         = 0.0;   // пар списков за попытку по профилю ключей
    // Циклическая симметрия кода: поиск тогда идёт по орбитам сдвигов (cyclic.h).
    Cyclic::Symmetry      leonSymmetry;
};

// Внутренности для собственных .cpp Worker — см. worker_p.h.
struct LayerSlice;
struct ChunkPlan;
struct WorkerBuffers;

Q_DECLARE_METATYPE(LoadMode)
class Worker : public QObject
{
    Q_OBJECT
public:
    explicit Worker(QObject *parent = nullptr);
    ~Worker() override;

    // Частота показа спектра и интервал автосохранения — единственные
    // настройки, которые можно менять на ходу: остальные задают саму задачу и
    // выбираются один раз, при старте.
    //
    // Не слот и вызывается из потока интерфейса напрямую, как cancel(): через
    // очередь событий значение дошло бы только после конца расчёта.
    void setLiveIntervals(int spectrumMs, int checkpointSeconds);

    void pause();
    void resume();
    void cancel();
    void uncancel();
    bool isCancelled();

    // Воспроизводимое прерывание для тестов возобновления: сохранять чекпоинт
    // каждые everyOps операций и остановиться, дойдя до stopAfterOps.
    // Прерывание по таймеру для этого не годится — точка обрыва каждый раз
    // разная, и результаты двух запусков не сравнить.
    // Оба нуля — обычный режим работы.
    void setCheckpointOpsPolicy(quint64 everyOps, quint64 stopAfterOps);

    // Куда складывать автосохранения. Пустая строка — стандартное место
    // приложения. Тестам нужен свой каталог, чтобы не топтать пользовательский.
    void setAutosaveRoot(const QString& dir);

    // Итог последнего завершённого расчёта: спектр и до какого веса он точен
    // (для полного перебора — до длины кода).
    const QVector<quint64>& finalSpectrum() const { return m_finalSpectrum; }
    int finalExactUpTo() const { return m_finalExactUpTo; }

    // Случайный поиск может отдать не только счёт, но и сами слова — коду
    // произведения нужны они. По умолчанию слова после расчёта не хранятся.
    void setKeepFoundWords(bool keep) { m_keepFoundWords = keep; }
    const std::vector<quint64>& foundWords()   const { return m_foundWords; }
    const std::vector<int>&     foundWeights() const { return m_foundWeights; }

    // Порог, ниже которого подбор сетки не окупается и не проводится.
    // Тесты ставят ноль, чтобы гонять подбор на маленьких матрицах.
    void setGridTuningThreshold(double seconds);

    // Печатать таблицу замеров подбора. Для режима --sweep в тестах.
    void setGridTuningVerbose(bool on);

    // Где плану стохастического поиска можно брать окно Штерна–Дюмера
    // (см. Leon::WindowPolicy). Тесты выключают его, чтобы сравнивать CPU и
    // GPU слово в слово.
    void setWindowPolicy(Leon::WindowPolicy policy) { m_windowPolicy = policy; }
    // До какой размерности компоненту произведения перебирать целиком, а не
    // вложенным расчётом. Тесты занижают порог, чтобы гонять вложенный
    // расчёт на маленьких кодах.
    void setProductBruteForceMaxK(int k) { m_productBruteForceMaxK = k; }

public slots:
    void computeSpectrum( );
    // Замер потолка: сколько раз в секунду спектр реально успевает уйти в
    // интерфейс на текущих настройках.
    //
    // Считать этот потолок нечем — он не выводится из скорости и размера
    // чанка. На длинном пути такая формула сходится, а на коротком ошибается
    // в двести раз: там предел ставит не чанк, а глубина очереди запусков.
    // Поэтому здесь настоящий расчёт, запущенный ненадолго, и подсчёт
    // фактических отправок.
    //
    // Останавливается снаружи, вызовом cancel() по таймеру.
    void measureUpdateRate();
    void setSettings(const ComputationSettings& settings);
    void initializeRunState(LoadMode lm);
signals:
    // Сигнал для обновления progressbar-а
    void progressChanged(       int percent                       );
    // Текущий спектр — числами; как его показать, решает интерфейс.
    void spectrumUpdated(     const SpectrumCounts& spectrum    );
    // Сигнал, посылаемый при возникновении ошибки
    void errorOccurred(       const QString& message            );
    // Сигнал, посылаемый при окончании расчета спектра
    void finished( int );
    // Сигнал для обновления таймера. doneOps/totalOps идут вместе со временем:
    // на задачах в миллиарды слов один процент мало что говорит о масштабе.
    void estimateUpdated( int elapsedSec, int minutesLeft, double speed,
                                 quint64 doneOps, quint64 totalOps );
    // Сигнал того, что надо показать значок сохранения
    void spectrumSaved();
    // Подобранная замером сетка запуска. Пользователю её стоит показать:
    // иначе непонятно, на чём считает программа.
    void gridTuned( int blocks, int threads );
    // Результат measureUpdateRate, отправок в секунду. Ноль — замер ничего не
    // поймал: за отведённое время не ушло ни одного спектра.
    void updateRateMeasured( double perSecond );
    // Замер начался. Отдельный сигнал нужен, потому что перед ним успевает
    // пройти подбор сетки — секунды, — и отсчёт длительности пробы должен
    // начинаться не с просьбы, а отсюда.
    void updateRateProbeStarted();
    // План Брауэра–Циммермана: сколько множеств нашлось, до скольких строк
    // пойдёт перебор и до какого веса спектр будет точным. Пользователь
    // задавал только вес, остальное выведено из матрицы — ему это надо видеть.
    void planReady( int sets, int rows, int exactUpToWeight );
    // Ход случайного поиска: до какого веса собираются слова, сколько попыток
    // сделано из скольких, вероятность пропустить хотя бы одно слово по
    // модели и по весам — сколько слов, по оценке Чао, ещё не найдено.
    void searchEstimateUpdated( int weight, quint64 trialsDone, quint64 trialsTotal,
                         double missProbability, SpectrumFloat unseenByWeight );
    // Код произведения: что сейчас происходит и до какого веса спектр точен
    // (-1 — ещё считается).
    void productPlanReady( const QString& text, int exactUpToWeight );
private:
    /* Функции для работы с биноминальными коэффициентами */
    quint64   totalCombinations(quint64 k, quint64 maxRows) const;
    // Полное число операций расчёта: комбинации до maxRows − 1 по каждому из
    // множеств и слой maxRows по первым lastLayerSets. В обычном расчёте
    // множество одно.
    quint64   totalLayerOps(const CodeGeometry& g) const;


    /* Подготовка расчёта */
    // Выводит размеры и режимы из настроек и матрицы.
    CodeGeometry describeTask() const;
    // Брауэр–Циммерман: находит информационные множества (или поднимает их
    // из сохранения), выводит глубину перебора из заданного веса.
    void planInfoSets(CodeGeometry& g) const;
    // Упаковывает матрицу из строк QStringList в биты и раскладывает буферы
    // по памяти хоста и устройства.
    void prepareBuffers(const CodeGeometry& g);
    // Замеряет несколько сеток на настоящем ядре и ставит в g лучшую.
    // Ничего не делает, если подбор не включён или не применим.
    void tuneGrid(CodeGeometry& g);
    // Выбирает вычислительную функцию по алгоритму, устройству и длине кода.
    void dispatchComputation(const CodeGeometry& g);
    // Финальная выгрузка спектра, сигналы и освобождение ресурсов.
    void finishComputation(const CodeGeometry& g, std::chrono::steady_clock::time_point startedAt);

    /* Функции для расчета спектра кода */
    // Общий цикл перебора по слоям и чанкам — см. ChunkPlan.
    void runChunks(const CodeGeometry& g, const ChunkPlan& plan);
    // Имя пути — алгоритм, устройство и, где вариантов два, длина кода.
    void computeGrayGpu    (const CodeGeometry& g);
    void computeXorGpuShort(const CodeGeometry& g);
    void computeXorGpuLong (const CodeGeometry& g);
    void computeGrayCpu    (const CodeGeometry& g);
    void computeXorCpuShort(const CodeGeometry& g);
    void computeXorCpuLong (const CodeGeometry& g);
    // Перебор одного чанка на процессоре; false — прерван отменой.
    bool grayChunkCpu    (const CodeGeometry& g, const LayerSlice& s);
    bool xorChunkCpuShort(const CodeGeometry& g, quint64 r, const LayerSlice& s);
    bool xorChunkCpuLong (const CodeGeometry& g, quint64 r, const LayerSlice& s, quint64 masksPerThread);
    // Итоговая копия спектра с видеокарты в h_spectrum.
    void copySpectrumFromDevice(int cols);
    // Случайный поиск по информационным множествам, CPU и GPU.
    void computeLeon   (const CodeGeometry& g);
    // Код произведения: низ спектра по компонентам.
    void computeProduct(const CodeGeometry& g);
    // Каким алгоритмом считать компоненту произведения: по оценке числа
    // слов перебора — полный перебор (Грей по k или дуальный по n − k),
    // Брауэр–Циммерман до предела или стохастический поиск, если нужны
    // списки слов. Возвращает алгоритм и оценку в словах.
    struct ComponentPlan { ComputationSettings::Algorithm algorithm; double words; QString what; };
    ComponentPlan planComponent(const QStringList& rows, int weightUpTo, bool wantWords) const;
    // Компонента произведения: спектр до weightUpTo и слова до него же.
    // Маленькую перебирает целиком; большую считает вложенный Worker —
    // Брауэром–Циммерманом, если слова не нужны (сертификат, только счёт),
    // или случайным поиском, если нужны (список слов, но без гарантии).
    Product::Component analyzeComponent(const QStringList& rows, int weightUpTo,
                                        const QString& label, bool wantWords);

    /* Функции, посылающие сигнал для обновления интерфейса */
    void updateSpectrum(int cols);
    // Отправить в интерфейс спектр, лежащий по указателю: снимки для показа
    // берутся из кольца, а не из h_spectrum.
    void updateSpectrumFrom(const quint64* spectrum, int cols);
    // Отправить спектр в окно; при замере потолка — только сосчитать.
    void publishSpectrum(const SpectrumCounts& shown);


    /* Функции для работы с чекпоинтами */
    // finished — расчёт дошёл до конца. Такая запись не удаляется: по ней
    // потом можно досчитать спектр до большего числа строк, а не с нуля.
    void    makeCheckpoint(int cols, bool finished = false);
    // Чекпоинт по ходу перебора. На видеокарте спектр сначала забирается
    // после потока ядер (plan.stream), на процессоре он уже в h_spectrum.
    void    saveCheckpoint(const ChunkPlan& plan, int cols,
                           quint64 rOffset, quint64 chunkOffset);
    // Останавливает расчёт, если задан порог из setCheckpointOpsPolicy.
    void    stopIfOpsLimitReached();

    /* Отчёт о ходе расчёта */
    void    reportEstimate();
    void    reportProgress();
    // Ход шага, у которого своя единица работы (код произведения).
    void    reportStageProgress(quint64 done, quint64 total);

    // Ждёт снятия паузы. Возвращает false, если расчёт отменили.
    bool    waitWhilePaused();


    std::atomic<int> m_paused    { 0 };
    std::atomic<int> m_cancelled { 0 };

    ComputationSettings         m_settings;
    RunState                    m_runState;
    ProgressTracker             m_progress;

    // Множества из поднятого сохранения: продолжать расчёт можно только по
    // ним. Пусто — искать заново.
    QVector<QVector<int>>       m_resumedInfoSets;
    Cyclic::Symmetry            m_resumedOrbit;
    // Множества и глубина идущего расчёта — для записи в автосохранение.
    QVector<QVector<int>>       m_activeInfoSets;
    int                         m_activeMaxRows = 0;
    Cyclic::Symmetry            m_activeOrbit;
    // Сделано попыток случайного поиска — тоже в запись.
    quint64                     m_activeTrials  = 0;
    // Код произведения: до какого веса спектр точен — в запись.
    int                         m_productExactUpTo = -1;
    // Брауэр–Циммерман: спектр показывается только до заказанного веса.
    // Слои перебора цепляют и более тяжёлые слова, но не все — эти счётчики
    // остаются в состоянии (они нужны при досчёте), а на экран не идут.
    // -1 — показывать всё.
    int                         m_displayUpToWeight = -1;

    // Итог последнего расчёта — для вложенного использования: код
    // произведения считает компоненты вложенным Worker и забирает отсюда.
    QVector<quint64>            m_finalSpectrum;
    int                         m_finalExactUpTo = -1;
    // Вложенный расчёт, которому надо передать отмену и паузу.
    std::atomic<Worker*>        m_activeSub { nullptr };
    QString                     m_autosaveRootDir;
    bool                        m_keepFoundWords = false;
    std::vector<quint64>        m_foundWords;
    std::vector<int>            m_foundWeights;
    // Код произведения: степень пропуска, если компоненты собраны случайным
    // поиском; ноль — всё сертифицировано.
    int                         m_productMissExponent = 0;

    // Продолжение с чекпоинта: накопленный спектр переносится в буферы
    // расчёта (prepareBuffers), а не обнуляется.
    bool                        m_resumeSpectrum = false;
    // 0 — обычный режим; см. setCheckpointOpsPolicy
    quint64                     m_stopAfterOps   = 0;
    quint64                     m_checkpointEveryOps = 0;
    // См. setGridTuningThreshold
    double                      m_tuneThresholdSec   = 10.0;
    bool                        m_tuneVerbose        = false;
    Leon::WindowPolicy          m_windowPolicy;
    int                         m_productBruteForceMaxK = Product::BRUTE_FORCE_MAX_K;

    // Память расчёта на хосте и на видеокарте, поток ядер и кольцо снимков
    // (worker_p.h). Владеющая: освобождается вместе с объектом, каким бы
    // путём ни завершился расчёт — успехом, отменой или исключением.
    std::unique_ptr<WorkerBuffers> m_buffers;

    // Идёт замер потолка: спектры не отправляются, а считаются, и ход расчёта
    // в интерфейс не идёт — прогрессбар не должен дёргаться от пробы.
    bool                 m_probeMode  = false;
    quint64              m_probeSends = 0;
    BinomTable           m_binomTable;
    AutosaveStore        m_autosave;

    // Тело расчёта. Отделено от computeSpectrum(), чтобы та могла обернуть
    // его в try/catch и превратить исключение в сигнал об ошибке.
    void computeSpectrumImpl();
    // Освобождает всё, что выделено под расчёт.
    void releaseResources();
};
