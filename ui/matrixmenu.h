#pragma once

#include "matrixlibrary.h"

#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

class QAction;
class QMenu;
class QTimer;
class QWidget;

// Меню «Матрицы»: список сохранённых матриц, их загрузка, сохранение текущей
// и подменю удаления.
//
// Само меню и два его статических пункта приходят из .ui; здесь живёт всё
// динамическое — пункты по числу сохранённых матриц, которые пересобираются
// перед каждым показом.
class MatrixMenu : public QObject
{
    Q_OBJECT

public:
    // addAction — пункт «Сохранить матрицу…»; dialogParent нужен только как
    // родитель для диалогов ввода имени и подтверждения.
    MatrixMenu(QMenu* menu, QMenu* deleteMenu, QAction* addAction, QWidget* dialogParent);

    // Откуда брать текст матрицы при сохранении. Без источника пункт
    // сохранения ничего не делает.
    void setMatrixSource(std::function<QString()> source);

    // На время расчёта пункты блокируются: менять матрицу на ходу нельзя.
    void setActionsEnabled(bool enabled);

signals:
    // Пользователь выбрал сохранённую матрицу — её текст надо показать
    // в редакторе.
    void matrixChosen(const QString& matrixText);

private:
    // Пересобирает динамические пункты по содержимому библиотеки.
    void rebuild();
    void addCurrentMatrix();
    void removeMatrix(const QString& name, QAction* deleteAction);
    // Имя вида «Матрица (49,16)» — подставляется в диалог сохранения.
    QString suggestedName() const;

    void updateDeleteMenuState();
    // Пункт подменю «Удалить» — его нельзя трогать как динамический.
    QAction* deleteMenuAction() const;

    QMenu*   menu         = nullptr;
    QMenu*   deleteMenu   = nullptr;
    QAction* addAction    = nullptr;
    QWidget* dialogParent = nullptr;

    // Подменю раскрывается не сразу: без задержки оно выскакивает от любого
    // случайного пробега курсора по списку матриц.
    QTimer*           hoverTimer = nullptr;
    QPointer<QAction> pendingHover;

    MatrixLibrary            library;
    std::function<QString()> matrixSource;
    bool                     actionsEnabled = true;
};
