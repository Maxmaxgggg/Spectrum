#pragma once
#include <qjsonobject.h>
#include <qjsonarray.h>
#include <qhash.h>

enum TimeInterval {
    OneSecond = 1,
    FiveSeconds = 5,
    TenSeconds = 10,
    ThirtySeconds = 30,
    OneMinute = 60,
    FiveMinutes = 300,
    TenMinutes = 600
};

struct ComputationSettings
{
    // Порождающая матрица кода
    QStringList matrix;
    // Тип используемого алгоритма (Простой XOR, Код Грея, Дуальный код)
    enum Algorithm { SimpleXor = 0, GrayCode = 1, DualCode = 2 };
    // Тип перебора (Полный, Частичный)
    enum EnumerationType { Full = 0, Partial = 1 };
    // Тип вычислителя (ЦП, ГП)
    enum ComputeDevice { CPU = 0, GPU = 1 };

    Algorithm       algorithmType = SimpleXor;
    EnumerationType enumType = Full;
    ComputeDevice   compDev = CPU;
    // Максимальное число перебираемых строк
    int             maxRows = 0;

    // Подбирать число блоков и нитей замером перед расчётом вместо того,
    // чтобы брать их из настроек. Имеет смысл только для видеокарты.
    bool            autoTuneGrid = false;

    // Настройки вычислителя
    struct computeDeviceSettings {
        // Число потоков ЦП
        int threadsCpu = 1;
        // Число блоков ГП
        int blocksGpu = 1;
        // Число нитей ГП
        int threadsGpu = 1;
    } compDevSet;

    // Потолок числа столбцов на графике. К расчёту отношения не имеет и в
    // ключ автосохранения не входит — это только вид.
    int             maxPlotBars = 160;

    struct timeIntervalSettings {
        // Частота сохранения спектра в реестр
        int saveSpectrumInterval = TenSeconds;
        // Частота обновления спетрка в gui 
        int updateSpectrumInterval = OneSecond;
    } timeIntSet;

    ComputationSettings() noexcept = default;

    // Конструктор копирования
    ComputationSettings(const ComputationSettings& other) noexcept
        : algorithmType(other.algorithmType)
        , enumType(other.enumType)
        , maxRows(other.maxRows)
        , compDev(other.compDev)
        , autoTuneGrid(other.autoTuneGrid)
        , maxPlotBars(other.maxPlotBars)
        , compDevSet(other.compDevSet)
        , timeIntSet(other.timeIntSet) { }

    // Рекомендуется также явно определить оператор присваивания
    ComputationSettings& operator=(const ComputationSettings& other) noexcept
    {
        if (this == &other) return *this;
        matrix = other.matrix;
        algorithmType = other.algorithmType;
        enumType = other.enumType;
        maxRows = other.maxRows;
        compDev = other.compDev;
        autoTuneGrid = other.autoTuneGrid;
        maxPlotBars = other.maxPlotBars;
        compDevSet = other.compDevSet;
        timeIntSet = other.timeIntSet;
        return *this;
    }
    bool operator==(const ComputationSettings& other) const
    {
        // Не сравниваем между собой настройки времени
        return matrix == other.matrix &&
            algorithmType == other.algorithmType &&
            enumType == other.enumType &&
            maxRows == other.maxRows &&
            compDev == other.compDev &&
            autoTuneGrid == other.autoTuneGrid &&
            compDevSet.threadsCpu == other.compDevSet.threadsCpu &&
            compDevSet.blocksGpu == other.compDevSet.blocksGpu &&
            compDevSet.threadsGpu == other.compDevSet.threadsGpu;
    }
    QJsonObject toJson() const
    {
        QJsonObject obj;
        if(!matrix.isEmpty()) {
            QJsonArray arr;
            for (const QString& str : matrix) {
                arr.append(str);
            }
            obj["matrix"] = arr;
        }
        obj["algorithmType"] = static_cast<int>(algorithmType);
        obj["enumType"] = static_cast<int>(enumType);
        obj["maxRows"] = maxRows;
        obj["compDev"] = static_cast<int>(compDev);
        obj["maxPlotBars"] = maxPlotBars;
        obj["autoTuneGrid"] = autoTuneGrid;

        QJsonObject dev;
        dev["threadsCpu"] = compDevSet.threadsCpu;
        dev["blocksGpu"]  = compDevSet.blocksGpu;
        dev["threadsGpu"] = compDevSet.threadsGpu;

        obj["compDevSet"] = dev;

        QJsonObject timeInt;
        timeInt["saveSpectrumInterval"]   = timeIntSet.saveSpectrumInterval;
        timeInt["updateSpectrumInterval"] = timeIntSet.updateSpectrumInterval;
        obj["timeIntSet"] = timeInt;
        return obj;
    }

    static ComputationSettings fromJson(const QJsonObject& obj)
    {
        ComputationSettings s;

        if (obj.contains("matrix") && obj["matrix"].isArray()) {
            QJsonArray arr = obj["matrix"].toArray();

            QStringList list;
            for (const QJsonValue& val : arr) {
                list.append(val.toString());
            }

            s.matrix = list;
        }
        s.algorithmType = static_cast<Algorithm>(obj["algorithmType"].toInt());
        s.enumType = static_cast<EnumerationType>(obj["enumType"].toInt());
        s.maxRows = obj["maxRows"].toInt();
        s.compDev = static_cast<ComputeDevice>(obj["compDev"].toInt());
        // Ноль означает «ключа не было» — остаётся значение по умолчанию.
        if (obj["maxPlotBars"].toInt() > 0)
            s.maxPlotBars = obj["maxPlotBars"].toInt();
        s.autoTuneGrid = obj["autoTuneGrid"].toBool();

        QJsonObject dev = obj["compDevSet"].toObject();
        s.compDevSet.threadsCpu = dev["threadsCpu"].toInt();
        s.compDevSet.blocksGpu = dev["blocksGpu"].toInt();
        s.compDevSet.threadsGpu = dev["threadsGpu"].toInt();

        QJsonObject timeInt = obj["timeIntSet"].toObject();
        s.timeIntSet.saveSpectrumInterval   = timeInt["saveSpectrumInterval"].toInt();
        s.timeIntSet.updateSpectrumInterval = timeInt["updateSpectrumInterval"].toInt();
        return s;
    }
    quint64 computeHash(quint64 seed = 0ULL) const
    {
        seed = qHash(matrix, seed);
        seed = qHash(static_cast<int>(algorithmType), seed);
        seed = qHash(static_cast<int>(enumType), seed);

        // Подумать, как реализовать, чтобы можно было сначала перебрать для maxRows = n, а потом для n+1
        seed = qHash(maxRows, seed);
        seed = qHash(static_cast<int>(compDev), seed);

        // autoTuneGrid в ключ не входит намеренно: спектр от сетки не зависит,
        // и переключение галочки не должно осиротить сохранённый чекпоинт.
        seed = qHash(compDevSet.threadsCpu, seed);
        seed = qHash(compDevSet.blocksGpu, seed);
        seed = qHash(compDevSet.threadsGpu, seed);

        return seed;
    }
};

struct RunState {
    // Текущее перебираемое число строк
    quint64 rOffset = 0ULL;
    // Индекс чанка
    quint64 chunkOffset = 0ULL;
    // Число произведенных операций
    quint64 doneOps = 0ULL;
    // Число прошедних секунд
    long long elapsedSec = 0;
    // Текущий спектр
    QVector<quint64> spectrum;

    // Функция для сохранения текущего состояния вычислений
    void saveProgress(
        quint64 rOffset,
        quint64 chunkOffset,
        quint64 doneOps,
        long long elapsedSec,
        QVector<quint64> spectrum
    ) {
        this->rOffset = rOffset;
        this->chunkOffset = chunkOffset;
        this->doneOps = doneOps;
        this->elapsedSec = elapsedSec;
        this->spectrum = spectrum;
    };
    QJsonObject toJson() const
    {
        QJsonObject obj;

        obj["rOffset"] = static_cast<qint64>(rOffset);
        obj["chunkOffset"] = static_cast<qint64>(chunkOffset);
        obj["doneOps"] = static_cast<qint64>(doneOps);
        obj["elapsedSec"] = static_cast<qint64>(elapsedSec);

        // spectrum
        QJsonArray arr;
        for (quint64 v : spectrum) {
            arr.append(static_cast<qint64>(v));
        }
        obj["spectrum"] = arr;

        return obj;
    }
    static RunState fromJson(const QJsonObject& obj)
    {
        RunState s;

        s.rOffset = obj["rOffset"].toVariant().toULongLong();
        s.chunkOffset = obj["chunkOffset"].toVariant().toULongLong();
        s.doneOps = obj["doneOps"].toVariant().toULongLong();
        s.elapsedSec = obj["elapsedSec"].toVariant().toLongLong();

        // spectrum
        if (obj.contains("spectrum") && obj["spectrum"].isArray()) {
            QJsonArray arr = obj["spectrum"].toArray();
            s.spectrum.resize(arr.size());

            for (int i = 0; i < arr.size(); ++i) {
                s.spectrum[i] = arr[i].toVariant().toULongLong();
            }
        }

        return s;
    }
};