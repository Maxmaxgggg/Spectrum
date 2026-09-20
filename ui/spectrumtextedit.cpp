#include "spectrumtextedit.h"

#include <QMimeData>

SpectrumTextEdit::SpectrumTextEdit(QWidget* parent)
    : QPlainTextEdit(parent)
{
}

QChar SpectrumTextEdit::separator()
{
    return QChar(0x202F);   // узкий неразрывный пробел
}

QString SpectrumTextEdit::grouped(const QString& digits)
{
    QString out;
    out.reserve(digits.size() + digits.size() / 3);
    const int n = digits.size();
    for (int i = 0; i < n; ++i) {
        if (i > 0 && (n - i) % 3 == 0)
            out += separator();
        out += digits.at(i);
    }
    return out;
}

QMimeData* SpectrumTextEdit::createMimeDataFromSelection() const
{
    QString text = textCursor().selectedText();
    // В выделении абзацы разделены U+2029 — вернуть переводы строк.
    text.replace(QChar(0x2029), QLatin1Char('\n'));
    text.remove(separator());
    auto* const data = new QMimeData;
    data->setText(text);
    return data;
}
