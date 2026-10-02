#ifndef WORKER_H
#define WORKER_H

#include <atomic>
#include <chrono>
#include <functional>
#include <QThread>
#include <cmath>
#include <omp.h>
#include <cuda_runtime.h>
#include <qjsonobject.h>
#include <qsettings.h>

#include "defines.h"
#include "dualcode.h"
#include "computeSpectrumKernel.cuh"
#include "settings.h"
#include "progresstracker.h"
#include "binomtable.h"
#include "infosets.h"
#include "leonsearch.h"
#include "productcode.h"
// Переопределяет CUDA_CALL из .cuh: там макрос звал abort(), здесь бросает.
#include "cudabuffers.h"
#include "spectrumring.h"
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

    quint64 numOfRows    = 0;
    quint64 numOfCols    = 0;
    // Число 64-битных слов на одну строку
    quint64 wordsPerRow  = 0;
    quint64 matrixWords  = 0;
    // Длина спектра: веса от 0 до numOfCols включительно
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
    std::vector<int>      setOverlaps;
    std::vector<quint64>  setMasks;         // setCount x wordsPerRow
    std::vector<quint64>  setRows;          // setCount x numOfRows x wordsPerRow
    QVector<QVector<int>> setColumns;       // опорные столбцы — в автосохранение
    // Все слова веса меньше этого найдены. Ноль — не Брауэр–Циммерман.
    int                   guaranteedBelow = 0;

    // Случайный поиск: сколько попыток и сколько слов в каждой; глубина
    // перебора в попытке лежит в maxRows.
    quint64               leonTrials        = 0;
    double                leonWordsPerTrial = 0.0;
    int                   leonWindow        = 0;     // окно Штерна–Дюмера; 0 — без окна
    double                leonPairs         = 0.0;   // пар списков за попытку по профилю ключей
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
    void setKeepFoundWords(bool keep) { keepFoundWords = keep; }
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
    void setWindowPolicy(Leon::WindowPolicy policy) { windowPolicy = policy; }
    // До какой размерности компоненту произведения перебирать целиком, а не
    // вложенным расчётом. Тесты занижают порог, чтобы гонять вложенный
    // расчёт на маленьких кодах.
    void setProductBruteForceMaxK(int k) { productBruteForceMaxK = k; }

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
    void setSettings( const ComputationSettings& newSettings );
    void initializeRunState(LoadMode lm);
signals:
    // Сигнал для обновления progressbar-а
    void updateInfoPBR(       int percent                       );
    // Текущий спектр — числами; как его показать, решает интерфейс.
    void spectrumUpdated(     const SpectrumCounts& spectrum    );
    // Сигнал, посылаемый при возникновении ошибки
    void errorOccurred(       const QString& message            );
    // Сигнал, посылаемый при окончании расчета спектра
    void finished( int );
    // Сигнал для обновления таймера. doneOps/totalOps идут вместе со временем:
    // на задачах в миллиарды слов один процент мало что говорит о масштабе.
    void updateRemainingMinutes( int elapsedSec, int minutesLeft, double speed,
                                 quint64 doneOps, quint64 totalOps );
    // Сигнал того, что надо показать значок сохранения
    void showSaveLBL();
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
    void searchEstimate( int weight, quint64 trialsDone, quint64 trialsTotal,
                         double missProbability, SpectrumFloat unseenByWeight );
    // Код произведения: что сейчас происходит и до какого веса спектр точен
    // (-1 — ещё считается).
    void productPlan( const QString& text, int exactUpToWeight );
private:
    /* Функции для работы с биноминальными коэффициентами */
    quint64   totalCombinations(quint64 k, quint64 maxComb) const;
    // Полное число операций расчёта: комбинации до maxRows по каждому из
    // множеств. В обычном расчёте множество одно.
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
    void computeSpectrumGpuGrayShort  (const CodeGeometry& g);
    void computeSpectrumGpuNoGrayShort(const CodeGeometry& g);
    void computeSpectrumGpuNoGrayLong (const CodeGeometry& g);
    void computeSpectrumCpuGrayShort  (const CodeGeometry& g);
    void computeSpectrumCpuNoGrayShort(const CodeGeometry& g);
    void computeSpectrumCpuNoGrayLong (const CodeGeometry& g);
    // Перебор одного чанка на процессоре; false — прерван отменой.
    bool cpuGrayChunk    (const CodeGeometry& g, const LayerSlice& s);
    bool cpuXorShortChunk(const CodeGeometry& g, quint64 r, const LayerSlice& s);
    bool cpuXorLongChunk (const CodeGeometry& g, quint64 r, const LayerSlice& s, quint64 masksPerThread);
    // Итоговая копия спектра с видеокарты в h_spectrum.
    void copySpectrumFromDevice(int numOfCols);
    // Случайный поиск по информационным множествам, CPU и GPU.
    void computeSpectrumLeon          (const CodeGeometry& g);
    // Код произведения: низ спектра по компонентам.
    void computeSpectrumProduct       (const CodeGeometry& g);
    // Компонента произведения: спектр до weightUpTo и слова до него же.
    // Маленькую перебирает целиком; большую считает вложенный Worker —
    // Брауэром–Циммерманом, если слова не нужны (сертификат, только счёт),
    // или случайным поиском, если нужны (список слов, но без гарантии).
    // Каким алгоритмом считать компоненту произведения: по оценке числа
    // слов перебора — полный перебор (Грей по k или дуальный по n − k),
    // Брауэр–Циммерман до предела или стохастический поиск, если нужны
    // списки слов. Возвращает алгоритм и оценку в словах.
    struct ComponentPlan { ComputationSettings::Algorithm algorithm; double words; QString what; };
    ComponentPlan planComponent(const QStringList& rows, int weightUpTo, bool wantWords) const;
    Product::Component analyzeComponent(const QStringList& rows, int weightUpTo,
                                        const QString& label, bool wantWords);
    // Матрица-ключ автосохранения: у произведения обе компоненты подряд.
    QStringList autosaveKeyMatrix() const;

    /* Функции, посылающие сигнал для обновления интерфейса */
    void updateSpectrum(int numOfCols);
    // Отправить в интерфейс спектр, лежащий по указателю: снимки для показа
    // берутся из кольца, а не из h_spectrum.
    void updateSpectrumFrom(const quint64* spectrum, int numOfCols);
    // Спектр по Мак-Вильямс — точными большими целыми.
    void updateSpectrumExact(const std::vector<mpz_class>& spectrum);


    /* Функции для работы с чекпоинтами */
    // finished — расчёт дошёл до конца. Такая запись не удаляется: по ней
    // потом можно досчитать спектр до большего числа строк, а не с нуля.
    void    makeCheckpoint(int numOfCols, bool finished = false);
    // Чекпоинт по ходу перебора. gpuStream — поток, после которого спектр
    // забирается с видеокарты; nullptr — CPU, спектр уже в h_spectrum.
    void    saveCheckpoint(cudaStream_t gpuStream, int numOfCols,
                           quint64 rOffset, quint64 chunkOffset);
    // Останавливает расчёт, если задан порог из setCheckpointOpsPolicy.
    void    stopIfOpsLimitReached();

    /* Отчёт о ходе расчёта */
    void    reportEstimate();
    void    reportProgressBar();
    // Ход шага, у которого своя единица работы (код произведения).
    void    reportStageProgress(quint64 done, quint64 total);

    // Ждёт снятия паузы. Возвращает false, если расчёт отменили.
    bool    waitWhilePaused();


    std::atomic<int> paused    { 0 };
    std::atomic<int> cancelled { 0 };

    ComputationSettings         settings;
    RunState                    runState;
    ProgressTracker             progress;

    // Множества из поднятого сохранения: продолжать расчёт можно только по
    // ним. Пусто — искать заново.
    QVector<QVector<int>>       resumedInfoSets;
    // Множества и глубина идущего расчёта — для записи в автосохранение.
    QVector<QVector<int>>       activeInfoSets;
    int                         activeMaxRows = 0;
    // Сделано попыток случайного поиска — тоже в запись.
    quint64                     activeTrials  = 0;
    // Код произведения: до какого веса спектр точен — в запись.
    int                         productExactUpTo = -1;
    // Брауэр–Циммерман: спектр показывается только до заказанного веса.
    // Слои перебора цепляют и более тяжёлые слова, но не все — эти счётчики
    // остаются в состоянии (они нужны при досчёте), а на экран не идут.
    // -1 — показывать всё.
    int                         displayUpToWeight = -1;

    // Итог последнего расчёта — для вложенного использования: код
    // произведения считает компоненты вложенным Worker и забирает отсюда.
    QVector<quint64>            m_finalSpectrum;
    int                         m_finalExactUpTo = -1;
    // Вложенный расчёт, которому надо передать отмену и паузу.
    std::atomic<Worker*>        activeSub { nullptr };
    QString                     autosaveRootDir;
    bool                        keepFoundWords = false;
    std::vector<quint64>        m_foundWords;
    std::vector<int>            m_foundWeights;
    // Код произведения: степень пропуска, если компоненты собраны случайным
    // поиском; ноль — всё сертифицировано.
    int                         productMissExponent = 0;

    bool                        exportSpectrum = false;
    // 0 — обычный режим; см. setCheckpointOpsPolicy
    quint64                     stopAfterOps   = 0;
    quint64                     checkpointEveryOps = 0;
    // См. setGridTuningThreshold
    double                      tuneThresholdSec   = 10.0;
    bool                        tuneVerbose        = false;
    Leon::WindowPolicy          windowPolicy;
    int                         productBruteForceMaxK = Product::kBruteForceMaxK;

    // Все ресурсы владеющие: освобождаются вместе с объектом, каким бы путём
    // ни завершился расчёт — успехом, отменой или исключением.
    CudaStream           stream;

    HostBuffer<quint64>  h_spectrum;
    // Снимки спектра для показа по ходу расчёта. h_spectrum этим не занят:
    // туда пишет чекпоинт и итоговая копия, и они должны быть точными.
    SpectrumRing         spectrumRing;

    // Идёт замер потолка: спектры не отправляются, а считаются, и ход расчёта
    // в интерфейс не идёт — прогрессбар не должен дёргаться от пробы.
    bool                 probeMode  = false;
    quint64              probeSends = 0;
    HostBuffer<quint64>  h_matrix;
    BinomTable           binomTable;
    AutosaveStore        autosave;

    DeviceBuffer<quint64> d_spectrum;
    DeviceBuffer<quint64> d_matrix;
    DeviceBuffer<quint64> d_binomTable;

    // Тело расчёта. Отделено от computeSpectrum(), чтобы та могла обернуть
    // его в try/catch и превратить исключение в сигнал об ошибке.
    void computeSpectrumImpl();
    // Освобождает всё, что выделено под расчёт.
    void releaseResources();
};

#endif // WORKER_H
