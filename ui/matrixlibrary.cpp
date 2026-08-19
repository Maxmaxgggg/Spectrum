#include "matrixlibrary.h"

#include "defines.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

namespace {
// Ключи объектов в JSON. Совпадают с тем, что уже лежит в реестре.
inline QString keyName() { return QStringLiteral("matrixName"); }
inline QString keyBody() { return QStringLiteral("matrix"); }
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

    matrices = doc.array();
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

int MatrixLibrary::indexOf(const QString& name) const
{
    for (int i = 0; i < matrices.size(); ++i) {
        if (!matrices.at(i).isObject())
            continue;
        if (matrices.at(i).toObject().value(keyName()).toString() == name)
            return i;
    }
    return -1;
}

bool MatrixLibrary::contains(const QString& name) const
{
    return indexOf(name) >= 0;
}

QStringList MatrixLibrary::names() const
{
    QStringList out;
    for (const QJsonValue& value : matrices) {
        if (!value.isObject())
            continue;
        out << value.toObject().value(keyName()).toString();
    }
    return out;
}

QString MatrixLibrary::matrix(const QString& name) const
{
    const int idx = indexOf(name);
    if (idx < 0)
        return QString();
    return matrices.at(idx).toObject().value(keyBody()).toString();
}

void MatrixLibrary::save(const QString& name, const QString& matrixText)
{
    if (name.isEmpty())
        return;

    QJsonObject entry;
    entry[keyName()] = name;
    entry[keyBody()] = matrixText;

    const int idx = indexOf(name);
    if (idx >= 0)
        matrices[idx] = entry;
    else
        matrices.append(entry);

    flush();
}

bool MatrixLibrary::remove(const QString& name)
{
    const int idx = indexOf(name);
    if (idx < 0)
        return false;

    matrices.removeAt(idx);
    flush();
    return true;
}
