#pragma once

#include "matrixlibrary.h"

#include <QDialog>
#include <QVector>

class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;

// Сетка сохранённых матриц 10 x 10.
//
// В ячейке — размер матрицы «(n,k)» или ничего; название, которое давал
// пользователь, показывается в поле внизу при наведении и при выборе.
// Одно окно на два случая: загрузка (выбрать заполненную ячейку, можно и
// удалить) и сохранение (выбрать любую ячейку, подписать; занятая
// перезаписывается после подтверждения).
class MatrixSlotsDialog : public QDialog
{
    Q_OBJECT

public:
    enum class Mode { Load, Save };

    // matrixText — что сохранять (только в режиме Save).
    MatrixSlotsDialog(Mode mode, const QString& matrixText, QWidget* parent = nullptr);

    // Режим Load: текст выбранной матрицы (пусто, если ничего не выбрано).
    QString chosenMatrix() const { return chosen; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebuild();
    void select(int slot);
    void showName(int slot);
    void accept() override;
    void removeSelected();

    Mode          mode;
    QString       matrixText;
    QString       chosen;
    MatrixLibrary library;

    QVector<QToolButton*> cells;
    int                   selected = -1;
    QLineEdit*            nameEdit  = nullptr;
    QLabel*               sizeLabel = nullptr;
    QPushButton*          okButton  = nullptr;
    QPushButton*          removeButton = nullptr;
};
