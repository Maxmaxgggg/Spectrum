#pragma once
#include <qjsonobject.h>
#include <qjsonarray.h>
#include <qhash.h>
#include <QMetaType>

// Счётчики до 2^64 в JSON автосохранения.
//
// Пишутся строкой. Числом их писать нельзя: Qt 5 выводит JSON-числа в текст
// через double, и всё, что больше 2^53, округляется. У кода Грея при k >= 54
// это и точка продолжения, и сами числа спектра, а 2^63 и больше при
// приведении к qint64 вдобавок становились отрицательными.
//
// Читаются и строки, и числа — записи, сделанные до перехода на строки.
// Обратно тоже совместимо: прежняя версия читает эти поля через
// toVariant().toULongLong(), а он разбирает и строку.
namespace JsonU64
{
    inline QJsonValue write(quint64 value)
    {
        return QString::number(value);
    }

    inline quint64 read(const QJsonValue& value)
    {
        if (value.isString())
            return value.toString().toULongLong();
        return value.toVariant().toULongLong();
    }
}

enum TimeInterval {
    OneSecond = 1,
    FiveSeconds = 5,
    TenSeconds = 10,
    ThirtySeconds = 30,
    OneMinute = 60,
    FiveMinutes = 300,
    TenMinutes = 600
};

// Интервал обновления спектра на экране — в миллисекундах, отдельной шкалой.
// Секунда здесь слишком грубый шаг: ход расчёта виден рывками. Сохранение на
// диск, наоборот, чаще десяти секунд не нужно, поэтому оно осталось в секундах.
enum UpdateInterval {
    EveryTenthSecond   = 100,
    EveryQuarterSecond = 250,
    EveryHalfSecond    = 500,
    EverySecond        = 1000,
    EveryFiveSeconds   = 5000,
    EveryTenSeconds    = 10000,
    EveryThirtySeconds = 30000,
    EveryMinute        = 60000
};

struct ComputationSettings
{
    // Порождающая матрица кода
    QStringList matrix;
    // Вторая компонента кода произведения; matrix — первая.
    QStringList matrix2;
    // Тип используемого алгоритма (Простой XOR, Код Грея, Дуальный код,
    // Брауэр–Циммерман, случайный поиск по информационным множествам)
    enum Algorithm { SimpleXor = 0, GrayCode = 1, DualCode = 2, BrouwerZimmermann = 3,
                     RandomInfoSets = 4, ProductCode = 5 };
    // Тип перебора (Полный, Частичный)
    enum EnumerationType { Full = 0, Partial = 1 };
    // Тип вычислителя (ЦП, ГП)
    enum ComputeDevice { CPU = 0, GPU = 1 };

    Algorithm       algorithmType = SimpleXor;
    EnumerationType enumType = Full;
    ComputeDevice   compDev = CPU;
    // Максимальное число перебираемых строк
    int             maxRows = 0;
    // Брауэр–Циммерман: до какого веса спектр нужен точно. Число строк
    // перебора программа выводит из него сама — по найденным множествам.
    int             bzWeight = 8;
    // Случайный поиск: до какого веса собирать слова и с какой вероятностью
    // пропуска смириться — 10 в минус этой степени. Глубину перебора и число
    // попыток программа выводит сама.
    int             leonWeight       = 24;
    int             leonMissExponent = 9;
    // Память под таблицу найденных слов, мегабайты; 0 — половина физической.
    int             leonMemoryMb     = 0;

    double leonMissProbability() const
    {
        double miss = 1.0;
        for (int i = 0; i < leonMissExponent; ++i) miss /= 10.0;
        return miss;
    }
    // Код произведения: до какого веса считать (0 — до границы, за которой
    // начинаются слова следующего ранга) и до какого ранга слов идти.
    int             productWeight = 0;
    int             productRank   = 2;
    // Чем считать большие компоненты произведения: Брауэр–Циммерман (точно,
    // только ранг 1) или случайный поиск (список слов, ранги выше). Хранится
    // как значение Algorithm.
    int             productAlgorithm = RandomInfoSets;

    // Перебор идёт слоями по числу складываемых строк: простой XOR и
    // Брауэр–Циммерман. Код Грея и дуальный расчёт идут по маскам сплошь.
    bool layered() const
    {
        return algorithmType == SimpleXor || algorithmType == BrouwerZimmermann;
    }

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
        // Частота обновления спектра на экране, миллисекунды
        int updateSpectrumInterval = EverySecond;
    } timeIntSet;

    // Копирование и перемещение — умолчательные, поле в поле. Самописный
    // конструктор копирования пропускал matrix и matrix2: копия выходила без
    // матриц, а fromJson держался только на NRVO. Новое поле в умолчательном
    // копировании тоже не потеряется.
    ComputationSettings() noexcept = default;

    bool operator==(const ComputationSettings& other) const
    {
        // Не сравниваем между собой настройки времени
        return matrix == other.matrix &&
            matrix2 == other.matrix2 &&
            algorithmType == other.algorithmType &&
            enumType == other.enumType &&
            maxRows == other.maxRows &&
            bzWeight == other.bzWeight &&
            leonWeight == other.leonWeight &&
            leonMissExponent == other.leonMissExponent &&
            productWeight == other.productWeight &&
            productRank == other.productRank &&
            productAlgorithm == other.productAlgorithm &&
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
        if (!matrix2.isEmpty()) {
            QJsonArray arr;
            for (const QString& str : matrix2)
                arr.append(str);
            obj["matrix2"] = arr;
        }
        obj["algorithmType"] = static_cast<int>(algorithmType);
        obj["enumType"] = static_cast<int>(enumType);
        obj["maxRows"] = maxRows;
        obj["bzWeight"] = bzWeight;
        obj["leonWeight"] = leonWeight;
        obj["leonMissExponent"] = leonMissExponent;
        obj["leonMemoryMb"] = leonMemoryMb;
        obj["productWeight"] = productWeight;
        obj["productRank"] = productRank;
        obj["productAlgorithm"] = productAlgorithm;
        obj["compDev"] = static_cast<int>(compDev);
        obj["maxPlotBars"] = maxPlotBars;
        obj["autoTuneGrid"] = autoTuneGrid;

        QJsonObject dev;
        dev["threadsCpu"] = compDevSet.threadsCpu;
        dev["blocksGpu"]  = compDevSet.blocksGpu;
        dev["threadsGpu"] = compDevSet.threadsGpu;

        obj["compDevSet"] = dev;

        // Версия схемы. Нужна ровно для одного: отличить настройки, где
        // интервал обновления лежал в секундах, от нынешних, где он в
        // миллисекундах. Без метки различать приходилось по самому значению,
        // а это угадывание: 30 — это тридцать секунд по-старому и тридцать
        // миллисекунд по-новому, и по числу они неразличимы.
        obj["version"] = 2;

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
        if (obj.contains("matrix2") && obj["matrix2"].isArray()) {
            for (const QJsonValue& val : obj["matrix2"].toArray())
                s.matrix2.append(val.toString());
        }
        s.algorithmType = static_cast<Algorithm>(obj["algorithmType"].toInt());
        s.enumType = static_cast<EnumerationType>(obj["enumType"].toInt());
        s.maxRows = obj["maxRows"].toInt();
        // Ноль означает «ключа не было» — остаётся значение по умолчанию.
        if (obj["bzWeight"].toInt() > 0)
            s.bzWeight = obj["bzWeight"].toInt();
        if (obj["leonWeight"].toInt() > 0)
            s.leonWeight = obj["leonWeight"].toInt();
        if (obj["leonMissExponent"].toInt() > 0)
            s.leonMissExponent = obj["leonMissExponent"].toInt();
        s.leonMemoryMb = obj["leonMemoryMb"].toInt();
        s.productWeight = obj["productWeight"].toInt();
        if (obj["productRank"].toInt() > 0)
            s.productRank = obj["productRank"].toInt();
        if (obj.contains("productAlgorithm"))
            s.productAlgorithm = obj["productAlgorithm"].toInt();
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
        // Настройки без метки версии писала программа, хранившая этот
        // интервал в секундах. Без пересчёта расчёт стал бы обновлять спектр
        // шестьдесят раз в секунду вместо раза в минуту.
        if (obj["version"].toInt() < 2)
            s.timeIntSet.updateSpectrumInterval *= 1000;
        // Пункт «33 мс» из списка убран. У тех, кто успел его выбрать, значение
        // осталось в настройках, и без правки они остались бы на интервале,
        // которого в списке нет.
        if (s.timeIntSet.updateSpectrumInterval < EveryTenthSecond)
            s.timeIntSet.updateSpectrumInterval = EveryTenthSecond;
        return s;
    }
    quint64 computeHash(quint64 seed = 0ULL) const
    {
        seed = qHash(matrix, seed);
        seed = qHash(static_cast<int>(algorithmType), seed);
        seed = qHash(static_cast<int>(enumType), seed);

        // Подумать, как реализовать, чтобы можно было сначала перебрать для maxRows = n, а потом для n+1
        seed = qHash(maxRows, seed);
        seed = qHash(bzWeight, seed);
        seed = qHash(leonWeight, seed);
        seed = qHash(leonMissExponent, seed);
        seed = qHash(matrix2, seed);
        seed = qHash(productWeight, seed);
        seed = qHash(productRank, seed);
        seed = qHash(productAlgorithm, seed);
        seed = qHash(static_cast<int>(compDev), seed);

        // autoTuneGrid в ключ не входит намеренно: спектр от сетки не зависит,
        // и переключение галочки не должно осиротить сохранённый чекпоинт.
        seed = qHash(compDevSet.threadsCpu, seed);
        seed = qHash(compDevSet.blocksGpu, seed);
        seed = qHash(compDevSet.threadsGpu, seed);

        return seed;
    }
};
// Настройки уходят воркеру через очередь событий — как есть, без JSON.
Q_DECLARE_METATYPE(ComputationSettings)

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

        obj["rOffset"] = JsonU64::write(rOffset);
        obj["chunkOffset"] = JsonU64::write(chunkOffset);
        obj["doneOps"] = JsonU64::write(doneOps);
        obj["elapsedSec"] = static_cast<qint64>(elapsedSec);

        // spectrum
        QJsonArray arr;
        for (quint64 v : spectrum) {
            arr.append(JsonU64::write(v));
        }
        obj["spectrum"] = arr;

        return obj;
    }
    static RunState fromJson(const QJsonObject& obj)
    {
        RunState s;

        s.rOffset = JsonU64::read(obj["rOffset"]);
        s.chunkOffset = JsonU64::read(obj["chunkOffset"]);
        s.doneOps = JsonU64::read(obj["doneOps"]);
        s.elapsedSec = obj["elapsedSec"].toVariant().toLongLong();

        // spectrum
        if (obj.contains("spectrum") && obj["spectrum"].isArray()) {
            QJsonArray arr = obj["spectrum"].toArray();
            s.spectrum.resize(arr.size());

            for (int i = 0; i < arr.size(); ++i) {
                s.spectrum[i] = JsonU64::read(arr[i]);
            }
        }

        return s;
    }
};