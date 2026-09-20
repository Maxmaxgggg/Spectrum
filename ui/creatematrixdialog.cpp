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
const QColor kHighlight(0x9f, 0xe1, 0xcb);   // подсветка столбца при наведении
const QColor kSelected(0x5d, 0xca, 0xa5);    // закреплённый выбор
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
    tabs = new QTabWidget(this);
    layout->addWidget(tabs, 1);

    buildBchTab();
    {
        auto* const page = new QWidget(this);
        tabs->setTabEnabled(tabs->addTab(page, tr("РС-код")), false);
    }
    buildHammingTab();
    {
        auto* const page = new QWidget(this);
        tabs->setTabEnabled(tabs->addTab(page, tr("Код Рида–Маллера")), false);
    }

    auto* const buttons = new QDialogButtonBox(this);
    createButton = buttons->addButton(tr("Создать"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &CreateMatrixDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &CreateMatrixDialog::reject);
    layout->addWidget(buttons);
    connect(tabs, &QTabWidget::currentChanged, this, [this](int) { updateCreateButton(); });
    updateCreateButton();
}

void CreateMatrixDialog::updateCreateButton()
{
    // Вкладки строятся раньше кнопок; пока кнопки нет — нечего обновлять.
    if (!createButton)
        return;
    const int tab = tabs->currentIndex();
    bool ok = false;
    if (tab == bchTab)
        ok = selectedReps > 0 && current().n > 0;
    else if (tab == hammingTab)
        ok = Hamming::describe(hammingR->value(), hammingExtend->isChecked(), hammingShorten->value()).n > 0;
    createButton->setEnabled(ok);
}

void CreateMatrixDialog::buildHammingTab()
{
    auto* const page   = new QWidget(this);
    auto* const column = new QVBoxLayout(page);

    auto* const row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Проверочных символов r:"), page));
    hammingR = new QSpinBox(page);
    hammingR->setRange(Hamming::MIN_R, Hamming::MAX_R);
    hammingR->setValue(3);
    hammingR->setToolTip(tr("Код [2^r − 1, 2^r − 1 − r, 3]; расширенный — [2^r, 2^r − 1 − r, 4]"));
    row->addWidget(hammingR);
    row->addStretch(1);
    column->addLayout(row);

    auto* const options = new QHBoxLayout;
    hammingLabel = new QLabel(page);
    hammingLabel->setMinimumWidth(200);
    hammingExtend  = new QCheckBox(tr("Расширить"), page);
    hammingShorten = new QSpinBox(page);
    hammingShorten->setRange(0, 2046);
    options->addWidget(hammingLabel, 1);
    options->addWidget(hammingExtend);
    options->addWidget(new QLabel(tr("Укоротить на:"), page));
    options->addWidget(hammingShorten);
    column->addLayout(options);
    column->addStretch(1);

    connect(hammingR, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { showHamming(); });
    connect(hammingExtend, &QCheckBox::toggled, this, [this](bool) { showHamming(); });
    connect(hammingShorten, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { showHamming(); });

    hammingTab = tabs->addTab(page, tr("Код Хэмминга"));
    showHamming();
}

void CreateMatrixDialog::showHamming()
{
    const Hamming::Code code = Hamming::describe(hammingR->value(), hammingExtend->isChecked(), hammingShorten->value());
    hammingLabel->setText(code.n == 0 ? tr("укорочение больше k")
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
    rowExponents = QVector<int>(exponents.begin(), exponents.end());
    const int columns = TABLE_MAX_M - Bch::MIN_M + 1;

    table = new QTableWidget(rowExponents.size(), columns, page);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setFocusPolicy(Qt::NoFocus);
    table->setMouseTracking(true);
    table->viewport()->setMouseTracking(true);
    table->viewport()->installEventFilter(this);
    QStringList headers;
    for (int m = Bch::MIN_M; m <= TABLE_MAX_M; ++m) headers << QString::number(m);
    table->setHorizontalHeaderLabels(headers);
    QStringList rowHeaders;
    for (int e : rowExponents) rowHeaders << QString::number(e);
    table->setVerticalHeaderLabels(rowHeaders);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->verticalHeader()->setDefaultSectionSize(20);

    repsUpTo = QVector<QVector<int>>(columns, QVector<int>(rowExponents.size(), 0));
    for (int c = 0; c < columns; ++c) {
        int taken = 0;
        for (int r = 0; r < rowExponents.size(); ++r) {
            const auto& list = fields[c];
            const auto it = std::find_if(list.begin(), list.end(),
                                         [&](const Bch::MinimalPolynomial& p) { return p.exponent == rowExponents[r]; });
            auto* const item = new QTableWidgetItem(it == list.end() ? QStringLiteral("–") : it->octal);
            item->setTextAlignment(Qt::AlignCenter);
            if (it != list.end()) {
                ++taken;
                repsUpTo[c][r] = taken;
                item->setToolTip(tr("m%1(x), степень %2").arg(it->exponent).arg(it->degree));
            } else {
                item->setForeground(QColor(0xb4, 0xb2, 0xa9));
            }
            table->setItem(r, c, item);
        }
    }
    connect(table, &QTableWidget::cellClicked, this, [this](int row, int col) {
        if (repsUpTo[col][row] == 0)
            return;
        selectedM    = Bch::MIN_M + col;
        selectedReps = repsUpTo[col][row];
        highlight(row, col);
        updateCreateButton();
    });
    column->addWidget(table, 1);

    auto* const options = new QHBoxLayout;
    codeLabel = new QLabel(page);
    codeLabel->setMinimumWidth(200);
    extendBox  = new QCheckBox(tr("Расширить"), page);
    shortenBox = new QSpinBox(page);
    shortenBox->setRange(0, 1022);
    auto* const shortenLabel = new QLabel(tr("Укоротить на:"), page);
    options->addWidget(codeLabel, 1);
    options->addWidget(extendBox);
    options->addWidget(shortenLabel);
    options->addWidget(shortenBox);
    column->addLayout(options);
    connect(extendBox, &QCheckBox::toggled, this, [this](bool) { showCode(selectedM, selectedReps); updateCreateButton(); });
    connect(shortenBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { showCode(selectedM, selectedReps); updateCreateButton(); });

    bchTab = tabs->addTab(page, tr("БЧХ-код"));
}

bool CreateMatrixDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (table && watched == table->viewport()) {
        if (event->type() == QEvent::MouseMove) {
            const QPoint pos = static_cast<QMouseEvent*>(event)->pos();
            const QModelIndex index = table->indexAt(pos);
            if (index.isValid() && repsUpTo[index.column()][index.row()] > 0)
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
    const int selCol = selectedReps > 0 ? selectedM - Bch::MIN_M : -1;
    for (int c = 0; c < table->columnCount(); ++c)
        for (int r = 0; r < table->rowCount(); ++r) {
            QTableWidgetItem* const item = table->item(r, c);
            QColor color = Qt::transparent;
            if (c == selCol && repsUpTo[c][r] > 0 && repsUpTo[c][r] <= selectedReps)
                color = kSelected;
            if (c == column && repsUpTo[c][r] > 0 && repsUpTo[c][r] <= repsUpTo[column][row])
                color = kHighlight;
            item->setBackground(color == Qt::transparent ? QBrush() : QBrush(color));
        }
    if (row >= 0)
        showCode(Bch::MIN_M + column, repsUpTo[column][row]);
    else
        showCode(selectedM, selectedReps);
}

void CreateMatrixDialog::showCode(int m, int reps)
{
    if (reps <= 0) {
        codeLabel->clear();
        return;
    }
    const Bch::Code code = Bch::describe(m, reps, extendBox->isChecked(), shortenBox->value());
    if (code.n == 0) {
        codeLabel->setText(tr("укорочение больше k"));
        return;
    }
    codeLabel->setText(tr("n = %1, k = %2, δ = %3, t = %4").arg(code.n).arg(code.k)
                           .arg(code.designedDistance).arg(code.corrects));
}

Bch::Code CreateMatrixDialog::current() const
{
    if (selectedReps <= 0)
        return Bch::Code();
    return Bch::build(selectedM, selectedReps, extendBox->isChecked(), shortenBox->value());
}

void CreateMatrixDialog::accept()
{
    const int tab = tabs->currentIndex();
    if (tab == hammingTab) {
        const Hamming::Code code = Hamming::build(hammingR->value(), hammingExtend->isChecked(), hammingShorten->value());
        if (code.n == 0)
            return;
        result     = code.rows.join(QLatin1Char('\n'));
        resultName = tr("Хэмминг (%1,%2)").arg(code.n).arg(code.k);
        QDialog::accept();
        return;
    }
    const Bch::Code code = current();
    if (code.n == 0)
        return;
    result     = code.rows.join(QLatin1Char('\n'));
    resultName = tr("БЧХ (%1,%2)").arg(code.n).arg(code.k);
    QDialog::accept();
}
