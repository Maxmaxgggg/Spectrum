#pragma once

#include "bch.h"
#include "hamming.h"
#include "parity.h"

#include <QDialog>
#include <QVector>

class QCheckBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTabWidget;

// «Создать матрицу»: вкладки по семействам кодов — БЧХ, Хэмминг, чётность.
//
// Вкладка БЧХ — таблица минимальных многочленов (восьмеричных): столбцы —
// m, строки — представители циклотомических классов, как в справочнике.
// Наведение на ячейку подсвечивает её и все ячейки столбца выше: их
// произведение — порождающий многочлен, тут же показываются n и k.
// Щелчок закрепляет выбор. Расширение приписывает бит чётности, укорочение
// на s убирает первые s информационных символов.
class CreateMatrixDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CreateMatrixDialog(QWidget* parent = nullptr);

    // Матрица, построенная по нажатию «Создать»; пусто, если отменили.
    QString matrixText() const { return result; }
    QString matrixName() const { return resultName; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildBchTab();
    void buildHammingTab();
    void buildParityTab();
    // Доступность «Создать» по текущей вкладке.
    void updateCreateButton();
    // Подсветка столбца до строки включительно и показ размеров; row = -1 —
    // снять подсветку и показать закреплённый выбор.
    void highlight(int row, int column);
    void showCode(int m, int reps);
    // Что сейчас закреплено: m и число классов; reps == 0 — ничего.
    Bch::Code current() const;
    void accept() override;

    QTabWidget*   tabs       = nullptr;
    QPushButton*  createButton = nullptr;
    int           bchTab     = -1;
    int           hammingTab = -1;
    int           parityTab  = -1;

    // Вкладка БЧХ.
    QTableWidget* table      = nullptr;
    QLabel*       codeLabel  = nullptr;
    QCheckBox*    extendBox  = nullptr;
    QSpinBox*     shortenBox = nullptr;

    // Вкладка Хэмминга.
    QSpinBox*     hammingR       = nullptr;
    QLabel*       hammingLabel   = nullptr;
    QCheckBox*    hammingExtend  = nullptr;
    QSpinBox*     hammingShorten = nullptr;
    void showHamming();

    // Вкладка чётности.
    QSpinBox*     parityK     = nullptr;
    QLabel*       parityLabel = nullptr;

    // Строки таблицы — представители всех полей, объединённые; в ячейке
    // (строка, m) — сколько классов поля m взято до этой строки включительно,
    // 0 — у поля m такого представителя нет.
    QVector<int>          rowExponents;
    QVector<QVector<int>> repsUpTo;   // [column][row]

    int selectedM    = 0;
    int selectedReps = 0;
    QString result;
    QString resultName;
};
