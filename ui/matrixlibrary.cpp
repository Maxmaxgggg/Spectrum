#include "matrixlibrary.h"

#include "constants.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

namespace {
// Ключи объектов в JSON. Первые два совпадают с тем, что уже лежит в реестре.
inline QString keyName() { return QStringLiteral("matrixName"); }
inline QString keyBody() { return QStringLiteral("matrix"); }
inline QString keySlot() { return QStringLiteral("slot"); }
} // namespace

void MatrixLibrary::load()
{
    matrices = QJsonArray();

    QSettings settings;
    const QByteArray data = settings.value(SettingsKeys::MATRICES_JSON).toByteArray();
    if (data.isEmpty())
        return;

    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isArray())
        return;

    // Старые записи без ячейки — по свободным ячейкам подряд, в порядке
    // следования; занятые ячейки уже раскиданных записей обходятся.
    QJsonArray raw = doc.array();
    QVector<bool> taken(SLOTS, false);
    for (const QJsonValue& value : raw) {
        if (!value.isObject()) continue;
        const int slot = value.toObject().value(keySlot()).toInt(-1);
        if (slot >= 0 && slot < SLOTS) taken[slot] = true;
    }
    bool migrated = false;
    for (const QJsonValue& value : raw) {
        if (!value.isObject()) continue;
        QJsonObject object = value.toObject();
        int slot = object.value(keySlot()).toInt(-1);
        if (slot < 0 || slot >= SLOTS) {
            slot = 0;
            while (slot < SLOTS && taken[slot]) ++slot;
            if (slot >= SLOTS) continue;   // больше ста старых записей — лишние теряем
            taken[slot] = true;
            object.insert(keySlot(), slot);
            migrated = true;
        }
        matrices.append(object);
    }
    if (migrated)
        flush();
}

void MatrixLibrary::flush() const
{
    QSettings settings;
    settings.setValue(SettingsKeys::MATRICES_JSON,
                      QJsonDocument(matrices).toJson(QJsonDocument::Compact));
    settings.sync();
}

bool MatrixLibrary::isEmpty() const
{
    return matrices.isEmpty();
}

int MatrixLibrary::indexOf(int slot) const
{
    for (int i = 0; i < matrices.size(); ++i)
        if (matrices.at(i).isObject() && matrices.at(i).toObject().value(keySlot()).toInt(-1) == slot)
            return i;
    return -1;
}

bool MatrixLibrary::has(int slot) const
{
    return indexOf(slot) >= 0;
}

MatrixLibrary::Entry MatrixLibrary::at(int slot) const
{
    Entry entry;
    const int idx = indexOf(slot);
    if (idx < 0)
        return entry;
    const QJsonObject object = matrices.at(idx).toObject();
    entry.slot   = slot;
    entry.name   = object.value(keyName()).toString();
    entry.matrix = object.value(keyBody()).toString();
    return entry;
}

QVector<MatrixLibrary::Entry> MatrixLibrary::entries() const
{
    QVector<Entry> out;
    for (int slot = 0; slot < SLOTS; ++slot)
        if (has(slot)) out.append(at(slot));
    return out;
}

void MatrixLibrary::save(int slot, const QString& name, const QString& matrixText)
{
    if (slot < 0 || slot >= SLOTS)
        return;
    QJsonObject object;
    object.insert(keyName(), name);
    object.insert(keyBody(), matrixText);
    object.insert(keySlot(), slot);

    const int idx = indexOf(slot);
    if (idx >= 0)
        matrices.replace(idx, object);
    else
        matrices.append(object);
    flush();
}

bool MatrixLibrary::remove(int slot)
{
    const int idx = indexOf(slot);
    if (idx < 0)
        return false;
    matrices.removeAt(idx);
    flush();
    return true;
}

QString MatrixLibrary::dimensions(const QString& matrixText)
{
    int rows = 0, cols = 0;
    for (const QString& line : matrixText.split(QLatin1Char('\n'))) {
        const QString row = line.trimmed();
        if (row.isEmpty()) continue;
        ++rows;
        cols = qMax(cols, row.length());
    }
    if (rows == 0)
        return QString();
    return QStringLiteral("(%1,%2)").arg(cols).arg(rows);
}
