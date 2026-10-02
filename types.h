#pragma once

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

#include <limits>

using Matrix = QStringList;
using SpectrumFloat = QVector<float>;

// Спектр кода: сколько кодовых слов приходится на каждый вес, индекс — вес.
//
// Числа точные. Спектр, пересчитанный из дуального (Мак-Вильямс), у длинного
// кода бывает больше 2^64: тогда в counts насыщение, а точное значение —
// десятичной строкой в exact. Пока таких чисел нет, exact пуст.
//
// Раньше спектр шёл от расчёта к окну строками «вес - число», и окно,
// график и тесты разбирали их обратно. Строки остались только там, где
// спектр копируется и хранится между запусками, — lines() и fromLines().
struct SpectrumCounts
{
    QVector<quint64> counts;
    QVector<QString> exact;

    int size() const { return counts.size(); }

    // Ни одного ненулевого веса.
    bool isEmpty() const
    {
        for (quint64 v : counts)
            if (v != 0)
                return false;
        return true;
    }

    // Точное число слов веса w десятичной строкой.
    QString decimal(int w) const
    {
        if (w < exact.size() && !exact.at(w).isEmpty())
            return exact.at(w);
        return QString::number(counts.at(w));
    }

    // Число слов веса w для графика: точность там не нужна, нужен порядок.
    double value(int w) const
    {
        if (w < exact.size() && !exact.at(w).isEmpty())
            return exact.at(w).toDouble();
        return double(counts.at(w));
    }

    SpectrumFloat plotValues() const
    {
        SpectrumFloat out;
        out.reserve(size());
        for (int w = 0; w < size(); ++w)
            out.append(float(value(w)));
        return out;
    }

    // Строки «вес - число» для ненулевых весов.
    QStringList lines() const
    {
        QStringList out;
        for (int w = 0; w < size(); ++w)
            if (counts.at(w) != 0)
                out.append(QString::number(w) + QStringLiteral(" - ") + decimal(w));
        return out;
    }

    // Обратно из строк «вес - число». Нечитаемые строки пропускаются.
    static SpectrumCounts fromLines(const QStringList& lines)
    {
        SpectrumCounts s;
        for (const QString& line : lines) {
            const int dash = line.indexOf(QStringLiteral(" - "));
            if (dash < 0)
                continue;
            bool weightOk = false;
            const int w = line.left(dash).trimmed().toInt(&weightOk);
            const QString number = line.mid(dash + 3).trimmed();
            bool digits = !number.isEmpty();
            for (const QChar c : number)
                digits = digits && c.isDigit();
            if (!weightOk || w < 0 || !digits)
                continue;
            if (w >= s.size())
                s.counts.resize(w + 1);
            bool fits = false;
            const quint64 v = number.toULongLong(&fits);
            if (fits) {
                s.counts[w] = v;
                continue;
            }
            // Длиннее 64 бит — точное значение строкой, в counts насыщение.
            s.counts[w] = std::numeric_limits<quint64>::max();
            s.exact.resize(s.size());
            s.exact[w] = number;
        }
        if (!s.exact.isEmpty())
            s.exact.resize(s.size());
        return s;
    }
};
Q_DECLARE_METATYPE(SpectrumCounts)
