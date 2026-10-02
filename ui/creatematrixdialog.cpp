#include "creatematrixdialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHoverEvent>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <set>

namespace {
const QColor HIGHLIGHT_COLOR(0x9f, 0xe1, 0xcb);   // подсветка столбца при наведении
const QColor SELECTED_COLOR(0x5d, 0xca, 0xa5);    // закреплённый выбор
// Столбцы таблицы БЧХ — как в справочнике, до m = 10; у m = 11 классов
// вдвое больше, и таблица разрослась бы вдвое.
constexpr int TABLE_MAX_M = 10;
}

CreateMatrixDialog::CreateMatrixDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Создать матрицу"));
    resize(640, 640);

    auto* const layout = new QVBoxLayout(this);
    m_tabs = new QTabWidget(this);
    layout->addWidget(m_tabs, 1);

    buildBchTab();
    buildHammingTab();
    buildParityTab();

    auto* const buttons = new QDialogButtonBox(this);
    m_createButton = buttons->addButton(tr("Создать"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &CreateMatrixDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &CreateMatrixDialog::reject);
    layout->addWidget(buttons);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int) { updateCreateButton(); });
    updateCreateButton();
}

void CreateMatrixDialog::updateCreateButton()
{
    // Вкладки строятся раньше кнопок; пока кнопки нет — нечего обновлять.
    if (!m_createButton)
        return;
    const int tab = m_tabs->currentIndex();
    bool ok = false;
    if (tab == m_bchTab)
        ok = m_selectedReps > 0 && current().n > 0;
    else if (tab == m_hammingTab)
        ok = Hamming::describe(m_hammingR->value(), m_hammingExtend->isChecked(), m_hammingShorten->value()).n > 0;
    else if (tab == m_parityTab)
        ok = true;
    m_createButton->setEnabled(ok);
}

void CreateMatrixDialog::buildParityTab()
{
    auto* const page   = new QWidget(this);
    auto* const column = new QVBoxLayout(page);

    auto* const row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Информационных символов k:"), page));
    m_parityK = new QSpinBox(page);
    m_parityK->setRange(Parity::MIN_K, Parity::MAX_K);
    m_parityK->setValue(7);
    m_parityK->setToolTip(tr("Код [k + 1, k, 2]: единичная матрица и столбец единиц справа"));
    row->addWidget(m_parityK);
    row->addStretch(1);
    column->addLayout(row);

    m_parityLabel = new QLabel(page);
    column->addWidget(m_parityLabel);
    column->addStretch(1);

    auto show = [this]() {
        m_parityLabel->setText(tr("n = %1, k = %2, d = 2").arg(m_parityK->value() + 1).arg(m_parityK->value()));
    };
    connect(m_parityK, QOverload<int>::of(&QSpinBox::valueChanged), this, [show](int) { show(); });
    m_parityTab = m_tabs->addTab(page, tr("Код чётности"));
    show();
}

void CreateMatrixDialog::buildHammingTab()
{
    auto* const page   = new QWidget(this);
    auto* const column = new QVBoxLayout(page);

    auto* const row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Проверочных символов r:"), page));
    m_hammingR = new QSpinBox(page);
    m_hammingR->setRange(Hamming::MIN_R, Hamming::MAX_R);
    m_hammingR->setValue(3);
    m_hammingR->setToolTip(tr("Код [2^r − 1, 2^r − 1 − r, 3]; расширенный — [2^r, 2^r − 1 − r, 4]"));
    row->addWidget(m_hammingR);
    row->addStretch(1);
    column->addLayout(row);

    auto* const options = new QHBoxLayout;
    m_hammingLabel = new QLabel(page);
    m_hammingLabel->setMinimumWidth(200);
    m_hammingExtend  = new QCheckBox(tr("Расширить"), page);
    m_hammingShorten = new QSpinBox(page);
    m_hammingShorten->setRange(0, 2046);
    options->addWidget(m_hammingLabel, 1);
    options->addWidget(m_hammingExtend);
    options->addWidget(new QLabel(tr("Укоротить на:"), page));
    options->addWidget(m_hammingShorten);
    column->addLayout(options);
    column->addStretch(1);

    connect(m_hammingR, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { showHamming(); });
    connect(m_hammingExtend, &QCheckBox::toggled, this, [this](bool) { showHamming(); });
    connect(m_hammingShorten, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { showHamming(); });

    m_hammingTab = m_tabs->addTab(page, tr("Код Хэмминга"));
    showHamming();
}

void CreateMatrixDialog::showHamming()
{
    const Hamming::Code code = Hamming::describe(m_hammingR->value(), m_hammingExtend->isChecked(), m_hammingShorten->value());
    m_hammingLabel->setText(code.n == 0 ? tr("укорочение больше k")
                                      : tr("n = %1, k = %2, d = %3").arg(code.n).arg(code.k).arg(code.d));
    updateCreateButton();
}

void CreateMatrixDialog::buildBchTab()
{
    auto* const page   = new QWidget(this);
    auto* const column = new QVBoxLayout(page);

    // Строки — все представители всех полей, по возрастанию.
    std::set<int> exponents;
    QVector<std::vector<Bch::MinimalPolynomial>> fields;
    for (int m = Bch::MIN_M; m <= TABLE_MAX_M; ++m) {
        fields.append(Bch::minimalPolynomials(m));
        for (const Bch::MinimalPolynomial& p : fields.last())
            exponents.insert(p.exponent);
    }
    m_rowExponents = QVector<int>(exponents.begin(), exponents.end());
    const int columns = TABLE_MAX_M - Bch::MIN_M + 1;

    m_table = new QTableWidget(m_rowExponents.size(), columns, page);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setFocusPolicy(Qt::NoFocus);
    m_table->setMouseTracking(true);
    m_table->viewport()->setMouseTracking(true);
    m_table->viewport()->installEventFilter(this);
    QStringList headers;
    for (int m = Bch::MIN_M; m <= TABLE_MAX_M; ++m) headers << QString::number(m);
    m_table->setHorizontalHeaderLabels(headers);
    QStringList rowHeaders;
    for (int e : m_rowExponents) rowHeaders << QString::number(e);
    m_table->setVerticalHeaderLabels(rowHeaders);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->verticalHeader()->setDefaultSectionSize(20);

    m_repsUpTo = QVector<QVector<int>>(columns, QVector<int>(m_rowExponents.size(), 0));
    for (int c = 0; c < columns; ++c) {
        int taken = 0;
        for (int r = 0; r < m_rowExponents.size(); ++r) {
            const auto& list = fields[c];
            const auto it = std::find_if(list.begin(), list.end(),
                                         [&](const Bch::MinimalPolynomial& p) { return p.exponent == m_rowExponents[r]; });
            auto* const item = new QTableWidgetItem(it == list.end() ? QStringLiteral("–") : it->octal);
            item->setTextAlignment(Qt::AlignCenter);
            if (it != list.end()) {
                ++taken;
                m_repsUpTo[c][r] = taken;
                item->setToolTip(tr("m%1(x), степень %2").arg(it->exponent).arg(it->degree));
            } else {
                item->setForeground(QColor(0xb4, 0xb2, 0xa9));
            }
            m_table->setItem(r, c, item);
        }
    }
    connect(m_table, &QTableWidget::cellClicked, this, [this](int row, int col) {
        if (m_repsUpTo[col][row] == 0)
            return;
        m_selectedM    = Bch::MIN_M + col;
        m_selectedReps = m_repsUpTo[col][row];
        highlight(row, col);
        updateCreateButton();
    });
    column->addWidget(m_table, 1);

    auto* const options = new QHBoxLayout;
    m_codeLabel = new QLabel(page);
    m_codeLabel->setMinimumWidth(200);
    m_extendBox  = new QCheckBox(tr("Расширить"), page);
    m_shortenBox = new QSpinBox(page);
    m_shortenBox->setRange(0, 1022);
    auto* const shortenLabel = new QLabel(tr("Укоротить на:"), page);
    options->addWidget(m_codeLabel, 1);
    options->addWidget(m_extendBox);
    options->addWidget(shortenLabel);
    options->addWidget(m_shortenBox);
    column->addLayout(options);
    connect(m_extendBox, &QCheckBox::toggled, this, [this](bool) { showCode(m_selectedM, m_selectedReps); updateCreateButton(); });
    connect(m_shortenBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { showCode(m_selectedM, m_selectedReps); updateCreateButton(); });

    m_bchTab = m_tabs->addTab(page, tr("БЧХ-код"));
}

bool CreateMatrixDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (m_table && watched == m_table->viewport()) {
        if (event->type() == QEvent::MouseMove) {
            const QPoint pos = static_cast<QMouseEvent*>(event)->pos();
            const QModelIndex index = m_table->indexAt(pos);
            if (index.isValid() && m_repsUpTo[index.column()][index.row()] > 0)
                highlight(index.row(), index.column());
            else
                highlight(-1, -1);
        } else if (event->type() == QEvent::Leave) {
            highlight(-1, -1);
        }
    }
    return QDialog::eventFilter(watched, event);
}

void CreateMatrixDialog::highlight(int row, int column)
{
    const int selCol = m_selectedReps > 0 ? m_selectedM - Bch::MIN_M : -1;
    for (int c = 0; c < m_table->columnCount(); ++c)
        for (int r = 0; r < m_table->rowCount(); ++r) {
            QTableWidgetItem* const item = m_table->item(r, c);
            QColor color = Qt::transparent;
            if (c == selCol && m_repsUpTo[c][r] > 0 && m_repsUpTo[c][r] <= m_selectedReps)
                color = SELECTED_COLOR;
            if (c == column && m_repsUpTo[c][r] > 0 && m_repsUpTo[c][r] <= m_repsUpTo[column][row])
                color = HIGHLIGHT_COLOR;
            item->setBackground(color == Qt::transparent ? QBrush() : QBrush(color));
        }
    if (row >= 0)
        showCode(Bch::MIN_M + column, m_repsUpTo[column][row]);
    else
        showCode(m_selectedM, m_selectedReps);
}

void CreateMatrixDialog::showCode(int m, int reps)
{
    if (reps <= 0) {
        m_codeLabel->clear();
        return;
    }
    const Bch::Code code = Bch::describe(m, reps, m_extendBox->isChecked(), m_shortenBox->value());
    if (code.n == 0) {
        m_codeLabel->setText(tr("укорочение больше k"));
        return;
    }
    m_codeLabel->setText(tr("n = %1, k = %2, δ = %3, t = %4").arg(code.n).arg(code.k)
                           .arg(code.designedDistance).arg(code.corrects));
}

Bch::Code CreateMatrixDialog::current() const
{
    if (m_selectedReps <= 0)
        return Bch::Code();
    return Bch::build(m_selectedM, m_selectedReps, m_extendBox->isChecked(), m_shortenBox->value());
}

void CreateMatrixDialog::accept()
{
    const int tab = m_tabs->currentIndex();
    if (tab == m_parityTab) {
        m_result     = Parity::build(m_parityK->value()).join(QLatin1Char('\n'));
        m_resultName = tr("Чётность (%1,%2)").arg(m_parityK->value() + 1).arg(m_parityK->value());
        QDialog::accept();
        return;
    }
    if (tab == m_hammingTab) {
        const Hamming::Code code = Hamming::build(m_hammingR->value(), m_hammingExtend->isChecked(), m_hammingShorten->value());
        if (code.n == 0)
            return;
        m_result     = code.rows.join(QLatin1Char('\n'));
        m_resultName = tr("Хэмминг (%1,%2)").arg(code.n).arg(code.k);
        QDialog::accept();
        return;
    }
    const Bch::Code code = current();
    if (code.n == 0)
        return;
    m_result     = code.rows.join(QLatin1Char('\n'));
    m_resultName = tr("БЧХ (%1,%2)").arg(code.n).arg(code.k);
    QDialog::accept();
}
