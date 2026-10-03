#include "autosavestore.h"

#include "constants.h"
#include "cyclic.h"
#include "infosets.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>

#include <algorithm>
#include <cmath>

namespace {

const QLatin1String MATRIX_FILE("matrix.txt");

// Версия формата записи. Читать чужую версию вслепую нельзя: в спектре
// лежат числа, и молча принять чужую раскладку — это тихо неверный
// результат, а не ошибка.
//
// Версия 2 — только у записей Брауэра–Циммермана по сдвигам (cyclicorbit.h):
// их спектр считан правилом орбит, и сборка, которая о нём не знает, обязана
// такую запись пропустить, а не продолжить как обычную. Остальные записи
// пишутся версией 1 — их читают и прежние сборки.
constexpr int FORMAT_VERSION       = 1;
constexpr int FORMAT_VERSION_ORBIT = 2;

bool knownVersion(const QJsonObject& obj)
{
    const int version = obj["version"].toInt();
    return version == FORMAT_VERSION || version == FORMAT_VERSION_ORBIT;
}

QString matrixText(const Matrix& matrix)
{
    return matrix.join(QLatin1Char('\n'));
}

} // namespace

// ---------------------------------------------------------------- запись

QJsonObject AutosaveRecord::toJson() const
{
    QJsonObject obj;
    obj["version"]   = cyclicLength > 0 ? FORMAT_VERSION_ORBIT : FORMAT_VERSION;
    obj["algorithm"] = int(algorithm);
    if (cyclic)
        obj["cyclic"] = true;
    obj["enumType"]  = int(enumType);
    obj["maxRows"]   = maxRows;
    obj["finished"]  = finished;
    obj["savedAt"]   = savedAt.toString(Qt::ISODate);
    obj["state"]     = state.toJson();
    if (algorithm == ComputationSettings::BrouwerZimmermann) {
        obj["bzWeight"] = bzWeight;
        QJsonArray sets;
        for (const QVector<int>& set : infoSets) {
            QJsonArray columns;
            for (int c : set)
                columns.append(c);
            sets.append(columns);
        }
        obj["infoSets"] = sets;
        if (cyclicLength > 0) {
            obj["cyclicStart"]  = cyclicStart;
            obj["cyclicLength"] = cyclicLength;
        }
    }
    if (algorithm == ComputationSettings::RandomInfoSets) {
        obj["leonWeight"]       = leonWeight;
        obj["leonMissExponent"] = leonMissExponent;
        obj["leonTrials"]       = JsonU64::write(leonTrials);
    }
    if (algorithm == ComputationSettings::ProductCode) {
        obj["productWeight"]    = productWeight;
        obj["productRank"]      = productRank;
        obj["productRows1"]     = productRows1;
        obj["productExactUpTo"] = productExactUpTo;
        obj["productMissExponent"] = productMissExponent;
    }
    return obj;
}

AutosaveRecord AutosaveRecord::fromJson(const QJsonObject& obj)
{
    AutosaveRecord r;
    r.algorithm = static_cast<ComputationSettings::Algorithm>(obj["algorithm"].toInt());
    r.enumType  = static_cast<ComputationSettings::EnumerationType>(obj["enumType"].toInt());
    r.maxRows   = obj["maxRows"].toInt();
    r.finished  = obj["finished"].toBool();
    r.savedAt   = QDateTime::fromString(obj["savedAt"].toString(), Qt::ISODate);
    r.state     = RunState::fromJson(obj["state"].toObject());
    r.bzWeight  = obj["bzWeight"].toInt();
    for (const QJsonValue& set : obj["infoSets"].toArray()) {
        QVector<int> columns;
        for (const QJsonValue& c : set.toArray())
            columns.append(c.toInt());
        r.infoSets.append(columns);
    }
    r.cyclicStart  = obj["cyclicStart"].toInt();
    r.cyclicLength = obj["cyclicLength"].toInt();
    r.cyclic       = obj["cyclic"].toBool() || r.cyclicLength > 0;
    r.leonWeight       = obj["leonWeight"].toInt();
    r.leonMissExponent = obj["leonMissExponent"].toInt();
    r.leonTrials       = JsonU64::read(obj["leonTrials"]);
    r.productWeight    = obj["productWeight"].toInt();
    r.productRank      = obj["productRank"].toInt();
    r.productRows1     = obj["productRows1"].toInt();
    r.productExactUpTo = obj.contains("productExactUpTo") ? obj["productExactUpTo"].toInt() : -1;
    r.productMissExponent = obj["productMissExponent"].toInt();
    return r;
}

InfoSets::Depth resumeDepth(const AutosaveRecord& record, int weight, int rows, int cols)
{
    if (record.infoSets.isEmpty())
        return InfoSets::Depth{ 0, 1 };
    if (record.cyclicLength > 0)
        return InfoSets::Depth{ Cyclic::orbitDepthForWeight(
                                    Cyclic::Symmetry{ record.cyclicStart, record.cyclicLength },
                                    rows, weight, cols), 1 };
    return InfoSets::depthForWeight(InfoSets::overlapsOf(record.infoSets), weight, rows, cols);
}

bool canResume(const AutosaveRecord& record, const ComputationSettings& settings)
{
    // Запись случайного поиска — только итог: найденные слова в ней не
    // хранятся, а без них продолжать нечего.
    if (settings.algorithm == ComputationSettings::RandomInfoSets
        || settings.algorithm == ComputationSettings::ProductCode)
        return false;
    if (!settings.layered())
        return true;

    quint64 maxRows = quint64(settings.maxRows);
    if (settings.algorithm == ComputationSettings::BrouwerZimmermann) {
        // Без множеств запись не продолжить: неизвестно, по каким матрицам
        // шёл перебор и какое множество засчитывало какое слово.
        if (record.infoSets.isEmpty() || settings.matrix.isEmpty())
            return false;
        // Структура «БЧХ» — другой способ перебора и счёта: продолжать можно
        // только запись с той же структурой, что выбрана сейчас.
        if (record.cyclic != settings.cyclic)
            return false;
        maxRows = quint64(resumeDepth(record, settings.bzWeight,
                                      settings.matrix.size(),
                                      settings.matrix.first().length()).maxRows);
        // Перебор по сдвигам: какое слово засчитывать, решает глубина, и до
        // какого веса считать — тоже. Другая глубина — другой счёт, начинать
        // заново.
        if (record.cyclicLength > 0 && int(maxRows) != record.maxRows)
            return false;
    }

    // Слой rOffset пройден частично: его вклад уже лежит в спектре, поэтому
    // расчёт обязан этот слой досчитать, а не остановиться раньше. Иначе
    // получился бы завышенный спектр без единого признака ошибки.
    if (record.state.chunkOffset > 0)
        return record.state.rOffset <= maxRows;

    // Ровная граница слоёв: посчитано всё до rOffset - 1 включительно, и
    // rOffset == maxRows + 1 означает «уже готово».
    return record.state.rOffset <= maxRows + 1;
}

double totalOperations(const AutosaveRecord& record, int rows)
{
    if (rows <= 0)
        return 0.0;

    if (record.algorithm != ComputationSettings::SimpleXor
        && record.algorithm != ComputationSettings::BrouwerZimmermann)
        return std::pow(2.0, double(rows));

    const int maxRows = record.maxRows > 0 ? qMin(record.maxRows, rows) : rows;

    // У Брауэра–Циммермана слои до последнего — по всем множествам, последний
    // — по части (см. infosets.h); глубина выводится из веса, как в расчёте.
    // По сдвигам — одно множество до глубины записи.
    // Глубина в записи своя и может быть нулевой.
    if (record.algorithm == ComputationSettings::BrouwerZimmermann && record.cyclicLength > 0)
        return InfoSets::combinationsFor(InfoSets::Depth{ qMin(record.maxRows, rows), 1 }, 1, rows);
    if (record.algorithm == ComputationSettings::BrouwerZimmermann
        && !record.infoSets.isEmpty() && !record.state.spectrum.isEmpty()) {
        const int cols = record.state.spectrum.size() - 1;
        const std::vector<int> overlaps = InfoSets::overlapsOf(record.infoSets);
        const InfoSets::Depth  depth    = InfoSets::depthForWeight(overlaps, record.bzWeight, rows, cols);
        return InfoSets::combinationsFor(depth, int(overlaps.size()), rows);
    }

    double total = 0.0;
    double term  = 1.0;               // C(rows, 0)
    for (int r = 0; r <= maxRows; ++r) {
        total += term;
        term = term * double(rows - r) / double(r + 1);
    }
    // Запись Брауэра–Циммермана без множеств или без спектра: оценка по
    // полным слоям.
    if (record.algorithm == ComputationSettings::BrouwerZimmermann)
        total *= double(qMax(1, record.infoSets.size()));
    return total;
}

// ---------------------------------------------------------------- хранилище

AutosaveStore::AutosaveStore(const QString& rootDir)
    : m_root(rootDir)
{
    if (m_root.isEmpty()) {
        m_root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
             + QLatin1String("/autosave");
    }
}

QString AutosaveStore::folderName(const Matrix& matrix)
{
    const int rows = matrix.size();
    const int cols = matrix.isEmpty() ? 0 : matrix.first().length();

    // SHA-1, а не qHash: имя папки должно быть одним и тем же на любой сборке
    // и в любом запуске, а qHash такого не обещает.
    const QByteArray digest =
        QCryptographicHash::hash(matrixText(matrix).toUtf8(), QCryptographicHash::Sha1);

    return QStringLiteral("%1x%2-%3").arg(cols).arg(rows)
                                     .arg(QString::fromLatin1(digest.toHex().left(8)));
}

QString AutosaveStore::folderPath(const Matrix& matrix) const
{
    return m_root + QLatin1Char('/') + folderName(matrix);
}

QString AutosaveStore::fileNameFor(ComputationSettings::Algorithm algorithm)
{
    switch (algorithm) {
        case ComputationSettings::GrayCode:          return QStringLiteral("gray.json");
        case ComputationSettings::DualCode:          return QStringLiteral("dual.json");
        case ComputationSettings::BrouwerZimmermann: return QStringLiteral("bz.json");
        case ComputationSettings::RandomInfoSets:    return QStringLiteral("leon.json");
        case ComputationSettings::ProductCode:       return QStringLiteral("product.json");
        default:                            return QStringLiteral("xor.json");
    }
}

bool AutosaveStore::save(const Matrix& matrix, const AutosaveRecord& record)
{
    if (matrix.isEmpty())
        return false;

    const QString dir = folderPath(matrix);
    if (!QDir().mkpath(dir))
        return false;

    // Матрица кладётся один раз на папку: она одна и та же для всех записей,
    // а весит до мегабайта.
    const QString matrixPath = dir + QLatin1Char('/') + MATRIX_FILE;
    if (!QFile::exists(matrixPath)) {
        QSaveFile file(matrixPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
            return false;
        QTextStream stream(&file);
        stream.setCodec("UTF-8");
        stream << matrixText(matrix) << '\n';
        if (!file.commit())
            return false;
    }

    // QSaveFile, а не QFile: расчёт идёт часами, и обрыв питания посреди
    // записи не должен оставлять обрубок вместо предыдущего сохранения.
    QSaveFile file(dir + QLatin1Char('/') + fileNameFor(record.algorithm));
    if (!file.open(QIODevice::WriteOnly))
        return false;

    file.write(QJsonDocument(record.toJson()).toJson(QJsonDocument::Compact));
    return file.commit();
}

bool AutosaveStore::load(const Matrix& matrix, ComputationSettings::Algorithm algorithm,
                         AutosaveRecord& out) const
{
    QFile file(folderPath(matrix) + QLatin1Char('/') + fileNameFor(algorithm));
    if (!file.open(QIODevice::ReadOnly))
        return false;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return false;

    const QJsonObject obj = doc.object();
    if (!knownVersion(obj))
        return false;

    out = AutosaveRecord::fromJson(obj);
    return true;
}

bool AutosaveStore::contains(const Matrix& matrix,
                             ComputationSettings::Algorithm algorithm) const
{
    return QFile::exists(folderPath(matrix) + QLatin1Char('/') + fileNameFor(algorithm));
}

bool AutosaveStore::remove(const Matrix& matrix, ComputationSettings::Algorithm algorithm)
{
    return removeRecord(folderName(matrix), algorithm);
}

bool AutosaveStore::removeRecord(const QString& folder,
                                 ComputationSettings::Algorithm algorithm)
{
    if (folder.isEmpty() || folder.contains(QLatin1Char('/'))
                         || folder.contains(QLatin1Char('\\')))
        return false;   // только имя папки, без путей наружу

    const QString dir = m_root + QLatin1Char('/') + folder;
    if (!QFile::remove(dir + QLatin1Char('/') + fileNameFor(algorithm)))
        return false;

    // Осталась одна matrix.txt — папку держать незачем.
    QDir folderDir(dir);
    if (folderDir.entryList(QStringList() << QStringLiteral("*.json"), QDir::Files).isEmpty())
        folderDir.removeRecursively();

    return true;
}

bool AutosaveStore::removeFolder(const QString& folder)
{
    if (folder.isEmpty() || folder.contains(QLatin1Char('/')) || folder.contains(QLatin1Char('\\')))
        return false;   // только имя папки, без путей наружу
    return QDir(m_root + QLatin1Char('/') + folder).removeRecursively();
}

void AutosaveStore::removeAll()
{
    QDir(m_root).removeRecursively();
}

Matrix AutosaveStore::matrixOf(const QString& folder) const
{
    QFile file(m_root + QLatin1Char('/') + folder + QLatin1Char('/') + MATRIX_FILE);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return Matrix();

    QTextStream stream(&file);
    stream.setCodec("UTF-8");
    return stream.readAll().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

QVector<AutosaveEntry> AutosaveStore::list() const
{
    QVector<AutosaveEntry> entries;

    QDir dir(m_root);
    const QStringList folders = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& folder : folders) {
        QDir sub(dir.filePath(folder));

        // Размеры кода берутся из имени папки: читать ради них матрицу в
        // мегабайт на каждую строчку списка — расточительство.
        int cols = 0;
        int rows = 0;
        const int dash = folder.indexOf(QLatin1Char('-'));
        const QString size = dash < 0 ? folder : folder.left(dash);
        const QStringList parts = size.split(QLatin1Char('x'));
        if (parts.size() == 2) {
            cols = parts.at(0).toInt();
            rows = parts.at(1).toInt();
        }

        const QStringList files = sub.entryList(QStringList() << QStringLiteral("*.json"),
                                                QDir::Files);
        for (const QString& name : files) {
            QFile file(sub.filePath(name));
            if (!file.open(QIODevice::ReadOnly))
                continue;

            const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
            if (!doc.isObject() || !knownVersion(doc.object()))
                continue;

            AutosaveEntry entry;
            entry.folder = folder;
            entry.cols   = cols;
            entry.rows   = rows;
            entry.bytes  = file.size();
            entry.record = AutosaveRecord::fromJson(doc.object());
            entries.append(entry);
        }
    }

    std::sort(entries.begin(), entries.end(),
              [](const AutosaveEntry& a, const AutosaveEntry& b) {
                  return a.record.savedAt > b.record.savedAt;
              });
    return entries;
}

void AutosaveStore::applyRetention(int maxRecords, int maxAgeDays)
{
    const QVector<AutosaveEntry> entries = list();   // свежие первыми
    const QDateTime now = QDateTime::currentDateTime();

    for (int i = 0; i < entries.size(); ++i) {
        const AutosaveEntry& entry = entries.at(i);

        const bool tooMany = maxRecords > 0 && i >= maxRecords;
        const bool tooOld  = maxAgeDays > 0 && entry.record.savedAt.isValid()
                          && entry.record.savedAt.daysTo(now) > maxAgeDays;
        if (!tooMany && !tooOld)
            continue;

        QDir sub(m_root + QLatin1Char('/') + entry.folder);
        sub.remove(fileNameFor(entry.record.algorithm));
        if (sub.entryList(QStringList() << QStringLiteral("*.json"), QDir::Files).isEmpty())
            sub.removeRecursively();
    }
}

int AutosaveStore::migrateFromRegistry()
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("checkpoints"));
    const QStringList groups = settings.childGroups();
    if (groups.isEmpty()) {
        settings.endGroup();
        return 0;
    }

    // Старый ключ включал устройство и параметры запуска, поэтому одна и та
    // же задача могла лежать в нескольких записях. Оставляем самую дальнюю.
    struct Candidate { Matrix matrix; AutosaveRecord record; };
    QHash<QString, Candidate> best;

    for (const QString& group : groups) {
        settings.beginGroup(group);
        const QJsonObject savedSettings = settings.value(QStringLiteral("settings")).toJsonObject();
        const QJsonObject savedState    = settings.value(QStringLiteral("runState")).toJsonObject();
        settings.endGroup();

        if (savedSettings.isEmpty() || savedState.isEmpty())
            continue;

        const ComputationSettings old = ComputationSettings::fromJson(savedSettings);
        if (old.matrix.isEmpty())
            continue;

        AutosaveRecord record;
        record.algorithm = old.algorithm;
        record.enumType  = old.enumType;
        record.maxRows   = old.maxRows;
        record.finished  = false;   // о завершённости старый формат не знал
        record.savedAt   = QDateTime::currentDateTime();
        record.state     = RunState::fromJson(savedState);

        const QString key = folderName(old.matrix)
                          + QLatin1Char('/') + fileNameFor(record.algorithm);

        const auto it = best.constFind(key);
        if (it == best.constEnd() || it->record.state.doneOps < record.state.doneOps)
            best.insert(key, Candidate{ old.matrix, record });
    }
    settings.endGroup();

    int moved = 0;
    for (auto it = best.constBegin(); it != best.constEnd(); ++it) {
        if (save(it->matrix, it->record))
            ++moved;
    }

    // Ветка убирается независимо от числа перенесённых: то, что не разобралось,
    // не разберётся и в следующий раз, а мегабайты в реестре останутся.
    settings.remove(QStringLiteral("checkpoints"));
    settings.sync();

    return moved;
}
