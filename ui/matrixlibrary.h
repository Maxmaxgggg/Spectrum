#pragma once

#include <QJsonArray>
#include <QString>
#include <QStringList>
#include <QVector>

// Матрицы, сохранённые пользователем, — в сетке ячеек 10 x 10.
//
// Хранилище — QSettings (реестр), один ключ с JSON-массивом объектов
// { "matrixName": …, "matrix": …, "slot": … }. Первые два ключа — как были:
// в реестре у пользователя уже лежат записи. Записи без "slot" (старые)
// при чтении раскладываются по свободным ячейкам подряд и так же
// сохраняются обратно.
//
// Класс отвечает только за хранение; окно с сеткой — MatrixSlotsDialog.
class MatrixLibrary
{
public:
    static constexpr int ROWS  = 10;
    static constexpr int COLS  = 10;
    static constexpr int SLOTS = ROWS * COLS;

    struct Entry
    {
        int     slot = -1;   // 0..SLOTS-1, построчно
        QString name;
        QString matrix;
    };

    // Перечитывает массив из хранилища. Вызывается перед каждой операцией:
    // настройки мог поменять второй запущенный экземпляр.
    void load();

    bool  isEmpty() const;
    bool  has(int slot) const;
    Entry at(int slot) const;   // пустая запись (slot == -1), если ячейка свободна
    QVector<Entry> entries() const;

    // Кладёт матрицу в ячейку, заменяя то, что там было. Сразу пишет в хранилище.
    void save(int slot, const QString& name, const QString& matrixText);
    // false, если ячейка была пуста.
    bool remove(int slot);

    // «(n,k)» по тексту матрицы — подпись ячейки; пусто для пустого текста.
    static QString dimensions(const QString& matrixText);

private:
    int  indexOf(int slot) const;
    void flush() const;

    QJsonArray matrices;
};
