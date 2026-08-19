#pragma once

#include <QJsonArray>
#include <QString>
#include <QStringList>

// Именованные матрицы, сохранённые пользователем.
//
// Хранилище — QSettings (реестр), один ключ с JSON-массивом объектов
// { "matrixName": ..., "matrix": ... }. Формат оставлен как был: в реестре
// у пользователя уже лежат записи, и менять их разбор без миграции нельзя.
//
// Класс отвечает только за хранение. Ни имя по умолчанию, ни пункты меню
// сюда не относятся: то и другое выводится из текущего состояния интерфейса.
class MatrixLibrary
{
public:
    // Перечитывает массив из хранилища. Вызывается перед каждой операцией
    // изменения: настройки мог поменять второй запущенный экземпляр.
    void load();

    bool        isEmpty() const;
    bool        contains(const QString& name) const;
    QStringList names() const;
    // Пустая строка, если матрицы с таким именем нет.
    QString     matrix(const QString& name) const;

    // Добавляет матрицу или заменяет одноимённую. Сразу пишет в хранилище.
    void save(const QString& name, const QString& matrixText);
    // false, если удалять было нечего.
    bool remove(const QString& name);

private:
    int  indexOf(const QString& name) const;
    void flush() const;

    QJsonArray matrices;
};
