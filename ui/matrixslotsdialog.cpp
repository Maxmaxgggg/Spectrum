#include "matrixslotsdialog.h"

#include <QDialogButtonBox>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
constexpr int CELL_PX = 58;
}

MatrixSlotsDialog::MatrixSlotsDialog(Mode mode, const QString& matrixText, QWidget* parent)
    : QDialog(parent), mode(mode), matrixText(matrixText)
{
    setWindowTitle(mode == Mode::Load ? tr("Загрузить матрицу") : tr("Сохранить матрицу"));

    auto* const layout = new QVBoxLayout(this);

    auto* const grid = new QGridLayout;
    grid->setSpacing(4);
    cells.resize(MatrixLibrary::SLOTS);
    for (int slot = 0; slot < MatrixLibrary::SLOTS; ++slot) {
        auto* const cell = new QToolButton(this);
        cell->setFixedSize(CELL_PX, CELL_PX);
        cell->setCheckable(true);
        cell->setAutoExclusive(true);
        cell->setAutoRaise(false);
        cell->installEventFilter(this);
        cell->setProperty("slot", slot);
        connect(cell, &QToolButton::clicked, this, [this, slot]() { select(slot); });
        connect(cell, &QToolButton::pressed, this, [this, slot]() { select(slot); });
        grid->addWidget(cell, slot / MatrixLibrary::COLS, slot % MatrixLibrary::COLS);
        cells[slot] = cell;
    }
    layout->addLayout(grid);

    auto* const nameRow = new QHBoxLayout;
    nameEdit = new QLineEdit(this);
    nameEdit->setReadOnly(mode == Mode::Load);
    nameEdit->setPlaceholderText(mode == Mode::Load ? QString() : tr("Название"));
    sizeLabel = new QLabel(this);
    sizeLabel->setMinimumWidth(80);
    nameRow->addWidget(nameEdit, 1);
    nameRow->addWidget(sizeLabel);
    layout->addLayout(nameRow);

    auto* const buttons = new QDialogButtonBox(this);
    okButton = buttons->addButton(mode == Mode::Load ? tr("Загрузить") : tr("Сохранить"),
                                  QDialogButtonBox::AcceptRole);
    if (mode == Mode::Load) {
        removeButton = buttons->addButton(tr("Удалить"), QDialogButtonBox::DestructiveRole);
        connect(removeButton, &QPushButton::clicked, this, &MatrixSlotsDialog::removeSelected);
    }
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &MatrixSlotsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &MatrixSlotsDialog::reject);
    layout->addWidget(buttons);

    library.load();
    rebuild();
    // При сохранении подпись по умолчанию — размер матрицы.
    if (mode == Mode::Save)
        nameEdit->setText(tr("Матрица %1").arg(MatrixLibrary::dimensions(matrixText)));
}

void MatrixSlotsDialog::rebuild()
{
    for (int slot = 0; slot < MatrixLibrary::SLOTS; ++slot) {
        const MatrixLibrary::Entry entry = library.at(slot);
        QToolButton* const cell = cells[slot];
        cell->setText(entry.slot < 0 ? QString() : MatrixLibrary::dimensions(entry.matrix));
        cell->setToolTip(entry.name);
        // В режиме загрузки пустые ячейки не выбираются — там нечего брать.
        cell->setEnabled(mode == Mode::Save || entry.slot >= 0);
    }
    okButton->setEnabled(selected >= 0 && (mode == Mode::Save || library.has(selected)));
    if (removeButton)
        removeButton->setEnabled(selected >= 0 && library.has(selected));
}

bool MatrixSlotsDialog::eventFilter(QObject* watched, QEvent* event)
{
    // Наведение — показать название; ушли — вернуть название выбранной.
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
        const QVariant slot = watched->property("slot");
        if (slot.isValid())
            showName(event->type() == QEvent::Enter ? slot.toInt() : selected);
    }
    return QDialog::eventFilter(watched, event);
}

void MatrixSlotsDialog::showName(int slot)
{
    if (mode == Mode::Save && slot == selected) {
        // В поле — то, что пользователь печатает; не затирать.
        return;
    }
    const MatrixLibrary::Entry entry = slot >= 0 ? library.at(slot) : MatrixLibrary::Entry();
    if (mode == Mode::Load) {
        nameEdit->setText(entry.name);
        sizeLabel->setText(entry.slot < 0 ? QString() : MatrixLibrary::dimensions(entry.matrix));
    } else {
        // Подсказка о занятой ячейке — в подписи размера, поле не трогаем.
        sizeLabel->setText(entry.slot < 0 ? QString() : tr("занято: %1").arg(entry.name));
    }
}

void MatrixSlotsDialog::select(int slot)
{
    selected = slot;
    cells[slot]->setChecked(true);
    if (mode == Mode::Load) {
        showName(slot);
    } else {
        const MatrixLibrary::Entry entry = library.at(slot);
        sizeLabel->setText(entry.slot < 0 ? QString() : tr("занято: %1").arg(entry.name));
    }
    okButton->setEnabled(mode == Mode::Save || library.has(slot));
    if (removeButton)
        removeButton->setEnabled(library.has(slot));
}

void MatrixSlotsDialog::accept()
{
    if (selected < 0)
        return;
    library.load();
    if (mode == Mode::Load) {
        const MatrixLibrary::Entry entry = library.at(selected);
        if (entry.slot < 0)
            return;
        chosen = entry.matrix;
        QDialog::accept();
        return;
    }
    if (library.has(selected)) {
        const MatrixLibrary::Entry old = library.at(selected);
        const auto answer = QMessageBox::question(
            this, tr("Сохранить матрицу"),
            tr("В этой ячейке уже лежит «%1» %2. Заменить?").arg(old.name, MatrixLibrary::dimensions(old.matrix)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }
    const QString name = nameEdit->text().trimmed();
    library.save(selected, name.isEmpty() ? tr("Матрица %1").arg(MatrixLibrary::dimensions(matrixText)) : name,
                 matrixText);
    QDialog::accept();
}

void MatrixSlotsDialog::removeSelected()
{
    if (selected < 0 || !library.has(selected))
        return;
    const MatrixLibrary::Entry entry = library.at(selected);
    const auto answer = QMessageBox::question(
        this, tr("Удалить матрицу"),
        tr("Удалить «%1» %2?").arg(entry.name, MatrixLibrary::dimensions(entry.matrix)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;
    library.load();
    library.remove(selected);
    rebuild();
    showName(selected);
}
