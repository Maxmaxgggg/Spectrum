#pragma once

#include "settings.h"
#include "types.h"

#include <QDateTime>
#include <QString>
#include <QVector>

// Хранилище автосохранений расчёта.
//
// Раньше всё лежало в реестре: по записи на хеш настроек, и в каждой — своя
// копия матрицы. На матрице 1000x997 это два мегабайта в HKCU за запись, а
// удалять их было нечем вовсе.
//
// Теперь на диске, по папке на матрицу:
//
//     autosave/
//       49x16-a3f19c2b/
//         matrix.txt        сама матрица, одна на все записи этой папки
//         xor.json          автосохранение расчёта простым XOR
//         gray.json         ... кодом Грея
//       1000x997-5b2ac190/
//         ...
//
// Имя папки — размер кода, как он показан в интерфейсе, плюс восемь знаков
// SHA-1 от текста матрицы: размеров мало, а матриц одного размера может быть
// сколько угодно.
//
// Ключ записи — матрица и алгоритм, и больше ничего. Число потоков, блоков и
// нитей, устройство и даже тип перебора в ключ не входят: спектр от них не
// зависит (это проверяется тестами переноса), а раньше входили, и смена
// спинбокса осиротила бы сохранение на ровном месте.
struct AutosaveRecord
{
    ComputationSettings::Algorithm       algorithm = ComputationSettings::SimpleXor;
    ComputationSettings::EnumerationType enumType  = ComputationSettings::Full;

    // До скольких строк шёл расчёт.
    int  maxRows  = 0;
    // Слои до maxRows включительно посчитаны полностью.
    bool finished = false;

    // Брауэр–Циммерман: до какого веса просили точный спектр и по каким
    // информационным множествам шёл перебор — опорные столбцы каждого, в
    // порядке строк. Множества входят в запись, потому что продолжать расчёт
    // можно только по тем же самым: от их состава зависит, какое множество
    // засчитывает какое слово. maxRows здесь — выведенная глубина перебора.
    int                    bzWeight = 0;
    QVector<QVector<int>>  infoSets;

    // Случайный поиск: до какого веса собирали, сколько сделано попыток и с
    // какой вероятностью пропуска (степень десятки). Такая запись хранит
    // только итог — продолжать поиск не с чего, найденные слова в неё не
    // входят.
    int     leonWeight       = 0;
    int     leonMissExponent = 0;
    quint64 leonTrials       = 0;

    // Код произведения: матрица записи — обе компоненты подряд, первые
    // productRows1 строк — первая. Спектр точен до productExactUpTo.
    int productWeight    = 0;
    int productRank      = 0;
    int productRows1     = 0;
    int productExactUpTo = -1;
    // Ноль — все компоненты сертифицированы; иначе хотя бы одна собрана
    // случайным поиском с вероятностью пропуска 10 в минус этой степени.
    int productMissExponent = 0;

    QDateTime savedAt;
    RunState  state;

    QJsonObject toJson() const;
    static AutosaveRecord fromJson(const QJsonObject& obj);
};

// Строка списка для диалога управления. Матрица сюда не читается: она бывает
// в мегабайт, а размеры кода видны прямо из имени папки.
struct AutosaveEntry
{
    QString folder;
    int     cols  = 0;
    int     rows  = 0;
    qint64  bytes = 0;

    AutosaveRecord record;
};

// Годится ли запись для расчёта с такими настройками.
//
// Слои по числу складываемых строк независимы и перебираются по возрастанию,
// поэтому сохранение, дошедшее до слоя r, — это законное начало любого расчёта
// с maxRows >= r. Обратно нельзя: в накопленном спектре уже учтён слой,
// которого при меньшем maxRows быть не должно, и результат вышел бы завышен.
//
// Отдельно учитывается недосчитанный слой: если chunkOffset не ноль, часть
// слоя rOffset уже в спектре, и остановиться раньше этого слоя нельзя.
//
// У кода Грея и дуального расчёта слоёв нет — маски нумеруются сплошь, и
// maxRows там не при чём.
//
// У Брауэра–Циммермана глубина перебора выводится из веса, до которого нужен
// точный спектр, и из множеств, записанных в сохранении, — по ним и
// сверяется, как по maxRows.
bool canResume(const AutosaveRecord& record, const ComputationSettings& settings);

// Глубина перебора, которой запись Брауэра–Циммермана отвечает при заданном
// весе: по её множествам. Ноль, если множеств в записи нет.
int resumeRows(const AutosaveRecord& record, int weight, int rows, int cols);

// Полное число операций расчёта записи — по нему считается процент готовности.
// Через double: точность здесь не нужна, а сумма биномов для кода длиной под
// тысячу ни во что целое не помещается.
double totalOperations(const AutosaveRecord& record, int rows);

class AutosaveStore
{
public:
    // Пустой путь — стандартное место приложения. Явный нужен тестам.
    explicit AutosaveStore(const QString& rootDir = QString());

    QString rootPath() const { return m_root; }

    // <столбцов>x<строк>-<8 знаков SHA-1 от текста матрицы>
    static QString folderName(const Matrix& matrix);

    bool save(const Matrix& matrix, const AutosaveRecord& record);
    // false, если записи нет или файл не читается.
    bool load(const Matrix& matrix, ComputationSettings::Algorithm algorithm,
              AutosaveRecord& out) const;
    bool contains(const Matrix& matrix, ComputationSettings::Algorithm algorithm) const;

    // Удаляет одну запись; вместе с последней уходит и папка матрицы.
    bool remove(const Matrix& matrix, ComputationSettings::Algorithm algorithm);
    // То же по имени папки: диалогу списка незачем читать ради удаления
    // матрицу, которая бывает в мегабайт.
    bool removeRecord(const QString& folder, ComputationSettings::Algorithm algorithm);
    bool removeFolder(const QString& folder);
    void removeAll();

    // Всё содержимое, свежие записи первыми.
    QVector<AutosaveEntry> list() const;

    // Матрица из папки — нужна диалогу, чтобы вернуть её в редактор.
    Matrix matrixOf(const QString& folder) const;

    // Оставляет не больше maxRecords записей и удаляет всё старше maxAgeDays.
    // Ноль в любом из пределов означает «не ограничивать».
    void applyRetention(int maxRecords, int maxAgeDays);

    // Переносит записи из старой ветки реестра и стирает её. Возвращает,
    // сколько записей перенесено. Из нескольких записей одной матрицы и
    // алгоритма остаётся самая дальняя по числу операций: раньше в ключ
    // входили параметры запуска, и одна и та же задача плодила копии.
    int migrateFromRegistry();

private:
    QString folderPath(const Matrix& matrix) const;
    static QString fileNameFor(ComputationSettings::Algorithm algorithm);

    QString m_root;
};
