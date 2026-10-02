#pragma once

#include "autosavestore.h"

#include <QDialog>

class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// Список автосохранений: что уже посчитано, насколько и когда.
//
// Раньше сохранения были чёрным ящиком: они копились в реестре, никак не
// показывались и не удалялись. Отсюда видно, что лежит на диске, сколько это
// занимает, и можно убрать лишнее.
//
// Собран кодом, а не в Designer: почти всё содержимое — заполнение таблицы,
// и в .ui лежала бы одна пустая рамка.
class AutosaveDialog : public QDialog
{
    Q_OBJECT

public:
    AutosaveDialog(AutosaveStore* store, QWidget* parent = nullptr);

signals:
    // Пользователь попросил поднять эту запись: матрица возвращается в
    // редактор, а вместе с ней окно восстанавливает спектр и прогресс.
    void entryChosen(const Matrix& matrix, const AutosaveRecord& record);

private:
    void refresh();
    void removeSelected();
    void removeAll();
    void loadMatrixOfSelected();
    void openFolder();
    void updateButtons();

    // Строка состояния записи: «готово до 7 строк», «47 %».
    static QString describeProgress(const AutosaveEntry& entry);
    static QString describeAlgorithm(const AutosaveRecord& record);
    AutosaveRecord entryOf(QTreeWidgetItem* item) const;

    AutosaveStore* m_store = nullptr;

    QTreeWidget* m_tree        = nullptr;
    QLabel*      m_summary     = nullptr;
    QPushButton* m_removeButton   = nullptr;
    QPushButton* m_removeAllButton = nullptr;
    QPushButton* m_loadButton     = nullptr;
};
