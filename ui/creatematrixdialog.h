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
    QString matrixText() const { return m_result; }
    QString matrixName() const { return m_resultName; }

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

    QTabWidget*   m_tabs       = nullptr;
    QPushButton*  m_createButton = nullptr;
    int           m_bchTab     = -1;
    int           m_hammingTab = -1;
    int           m_parityTab  = -1;

    // Вкладка БЧХ.
    QTableWidget* m_table      = nullptr;
    QLabel*       m_codeLabel  = nullptr;
    QCheckBox*    m_extendBox  = nullptr;
    QSpinBox*     m_shortenBox = nullptr;

    // Вкладка Хэмминга.
    QSpinBox*     m_hammingR       = nullptr;
    QLabel*       m_hammingLabel   = nullptr;
    QCheckBox*    m_hammingExtend  = nullptr;
    QSpinBox*     m_hammingShorten = nullptr;
    void showHamming();

    // Вкладка чётности.
    QSpinBox*     m_parityK     = nullptr;
    QLabel*       m_parityLabel = nullptr;

    // Строки таблицы — представители всех полей, объединённые; в ячейке
    // (строка, m) — сколько классов поля m взято до этой строки включительно,
    // 0 — у поля m такого представителя нет.
    QVector<int>          m_rowExponents;
    QVector<QVector<int>> m_repsUpTo;   // [column][row]

    int m_selectedM    = 0;
    int m_selectedReps = 0;
    QString m_result;
    QString m_resultName;
};
