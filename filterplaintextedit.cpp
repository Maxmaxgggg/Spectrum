#include "filterplaintextedit.h"
#include "qmimedata.h"

FilterPlainTextEdit::FilterPlainTextEdit(QWidget *parent)
    : QPlainTextEdit(parent){}

void FilterPlainTextEdit::insertFromMimeData(const QMimeData *source)
{
    // Вставляем только 0, 1 и символ новой строки
    QString text = source->text();
    text.remove(reInvalid);
    QPlainTextEdit::insertPlainText(text);
}

void FilterPlainTextEdit::keyPressEvent(QKeyEvent *e)
{
    // Разрешаем служебные клавиши и навигацию
    if (e->matches(QKeySequence::Copy)
        || e->matches(QKeySequence::Paste)
        || e->matches(QKeySequence::Cut)
        || e->matches(QKeySequence::SelectAll)
        || e->key() == Qt::Key_Backspace
        || e->key() == Qt::Key_Delete
        || e->key() == Qt::Key_Left
        || e->key() == Qt::Key_Right
        || e->key() == Qt::Key_Up
        || e->key() == Qt::Key_Down
        || e->key() == Qt::Key_Home
        || e->key() == Qt::Key_End
        || e->key() == Qt::Key_Return
        || e->key() == Qt::Key_Enter
        ) {
        QPlainTextEdit::keyPressEvent(e);
        return;
    }
    // Фильтруем вводимые символы
    const QString text = e->text();
    if (text == QLatin1String("0") || text == QLatin1String("1") || text == QLatin1String("\n")) {
        QPlainTextEdit::keyPressEvent(e);
    }
    // Иначе игнорируем
}



