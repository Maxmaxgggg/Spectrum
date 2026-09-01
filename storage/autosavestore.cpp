#include "autosavestore.h"

#include "defines.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>

#include <algorithm>

namespace {

const QLatin1String MATRIX_FILE("matrix.txt");

// Версия формата записи. Пока одна, но читать чужую версию вслепую нельзя:
// в спектре лежат числа, и молча принять чужую раскладку — это тихо неверный
// результат, а не ошибка.
constexpr int FORMAT_VERSION = 1;

QString matrixText(const Matrix& matrix)
{
    return matrix.join(QLatin1Char('\n'));
}

} // namespace

// ---------------------------------------------------------------- запись

QJsonObject AutosaveRecord::toJson() const
{
    QJsonObject obj;
    obj["version"]   = FORMAT_VERSION;
    obj["algorithm"] = int(algorithm);
    obj["enumType"]  = int(enumType);
    obj["maxRows"]   = maxRows;
    obj["finished"]  = finished;
    obj["savedAt"]   = savedAt.toString(Qt::ISODate);
    obj["state"]     = state.toJson();
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
    return r;
}

bool canResume(const AutosaveRecord& record, const ComputationSettings& settings)
{
    if (settings.algorithmType != ComputationSettings::SimpleXor)
        return true;

    const quint64 maxRows = quint64(settings.maxRows);

    // Слой rOffset пройден частично: его вклад уже лежит в спектре, поэтому
    // расчёт обязан этот слой досчитать, а не остановиться раньше. Иначе
    // получился бы завышенный спектр без единого признака ошибки.
    if (record.state.chunkOffset > 0)
        return record.state.rOffset <= maxRows;

    // Ровная граница слоёв: посчитано всё до rOffset - 1 включительно, и
    // rOffset == maxRows + 1 означает «уже готово».
    return record.state.rOffset <= maxRows + 1;
}

// ---------------------------------------------------------------- хранилище

AutosaveStore::AutosaveStore(const QString& rootDir)
    : root(rootDir)
{
    if (root.isEmpty()) {
        root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
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
    return root + QLatin1Char('/') + folderName(matrix);
}

QString AutosaveStore::fileNameFor(ComputationSettings::Algorithm algorithm)
{
    switch (algorithm) {
        case ComputationSettings::GrayCode: return QStringLiteral("gray.json");
        case ComputationSettings::DualCode: return QStringLiteral("dual.json");
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
    if (obj["version"].toInt() != FORMAT_VERSION)
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
    const QString dir = folderPath(matrix);
    if (!QFile::remove(dir + QLatin1Char('/') + fileNameFor(algorithm)))
        return false;

    // Осталась одна matrix.txt — папку держать незачем.
    QDir folder(dir);
    const QStringList left = folder.entryList(QStringList() << QStringLiteral("*.json"),
                                              QDir::Files);
    if (left.isEmpty())
        folder.removeRecursively();

    return true;
}

bool AutosaveStore::removeFolder(const QString& folder)
{
    if (folder.isEmpty() || folder.contains(QLatin1Char('/')) || folder.contains(QLatin1Char('\\')))
        return false;   // только имя папки, без путей наружу
    return QDir(root + QLatin1Char('/') + folder).removeRecursively();
}

void AutosaveStore::removeAll()
{
    QDir(root).removeRecursively();
}

Matrix AutosaveStore::matrixOf(const QString& folder) const
{
    QFile file(root + QLatin1Char('/') + folder + QLatin1Char('/') + MATRIX_FILE);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return Matrix();

    QTextStream stream(&file);
    stream.setCodec("UTF-8");
    return stream.readAll().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

QVector<AutosaveEntry> AutosaveStore::list() const
{
    QVector<AutosaveEntry> entries;

    QDir dir(root);
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
            if (!doc.isObject() || doc.object()["version"].toInt() != FORMAT_VERSION)
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

        QDir sub(root + QLatin1Char('/') + entry.folder);
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
        record.algorithm = old.algorithmType;
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
