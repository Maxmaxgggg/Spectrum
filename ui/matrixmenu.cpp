#include "matrixmenu.h"

#include "creatematrixdialog.h"
#include "matrixslotsdialog.h"

#include <QAction>
#include <QIcon>
#include <QWidget>

namespace {
const char ICON_LOAD[]   = ":/ui/icons/newspaper.png";
const char ICON_SAVE[]   = ":/ui/icons/newspaper--plus.png";
const char ICON_CREATE[] = ":/ui/icons/gear.png";
}

MatrixMenu::MatrixMenu(QAction* loadAction, QAction* saveAction, QAction* createAction, QWidget* dialogParent)
    : QObject(dialogParent)
    , m_loadAction(loadAction)
    , m_saveAction(saveAction)
    , m_createAction(createAction)
    , m_dialogParent(dialogParent)
{
    Q_ASSERT(loadAction && saveAction && createAction);
    loadAction->setIcon(QIcon(ICON_LOAD));
    saveAction->setIcon(QIcon(ICON_SAVE));
    createAction->setIcon(QIcon(ICON_CREATE));
    connect(loadAction,   &QAction::triggered, this, &MatrixMenu::load);
    connect(saveAction,   &QAction::triggered, this, &MatrixMenu::save);
    connect(createAction, &QAction::triggered, this, &MatrixMenu::create);
}

void MatrixMenu::setMatrixSource(std::function<QString()> source)
{
    m_matrixSource = std::move(source);
}

void MatrixMenu::setActionsEnabled(bool enabled)
{
    m_loadAction->setEnabled(enabled);
    m_saveAction->setEnabled(enabled);
    m_createAction->setEnabled(enabled);
}

void MatrixMenu::load()
{
    MatrixSlotsDialog dialog(MatrixSlotsDialog::Mode::Load, QString(), m_dialogParent);
    if (dialog.exec() == QDialog::Accepted && !dialog.chosenMatrix().isEmpty())
        emit matrixChosen(dialog.chosenMatrix());
}

void MatrixMenu::save()
{
    const QString text = m_matrixSource ? m_matrixSource() : QString();
    if (text.trimmed().isEmpty())
        return;
    MatrixSlotsDialog dialog(MatrixSlotsDialog::Mode::Save, text, m_dialogParent);
    dialog.exec();
}

void MatrixMenu::create()
{
    CreateMatrixDialog dialog(m_dialogParent);
    if (dialog.exec() == QDialog::Accepted && !dialog.matrixText().isEmpty())
        emit matrixChosen(dialog.matrixText());
}
