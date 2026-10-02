#pragma once

#include <QObject>
#include <QString>

#include <functional>

class QAction;
class QWidget;

// Меню «Матрицы»: загрузить из сетки сохранённых, сохранить текущую в сетку,
// создать по конструкции (БЧХ и далее). Сами окна — MatrixSlotsDialog и
// CreateMatrixDialog; здесь только пункты и их состояние.
class MatrixMenu : public QObject
{
    Q_OBJECT

public:
    MatrixMenu(QAction* loadAction, QAction* saveAction, QAction* createAction, QWidget* dialogParent);

    // Откуда брать текст матрицы при сохранении.
    void setMatrixSource(std::function<QString()> source);

    // На время расчёта пункты блокируются: менять матрицу на ходу нельзя.
    void setActionsEnabled(bool enabled);

signals:
    // Матрицу выбрали или построили — её текст надо показать в редакторе.
    void matrixChosen(const QString& matrixText);

private:
    void load();
    void save();
    void create();

    QAction* m_loadAction   = nullptr;
    QAction* m_saveAction   = nullptr;
    QAction* m_createAction = nullptr;
    QWidget* m_dialogParent = nullptr;

    std::function<QString()> m_matrixSource;
};
