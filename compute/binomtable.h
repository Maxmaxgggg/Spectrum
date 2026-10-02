#pragma once

#include <limits>
#include <stdexcept>
#include <vector>

#include <QtGlobal>

// Таблица биномиальных коэффициентов C(n, r) для n = 0..maxN, r = 0..maxR.
//
// Раньше это был quint64** — массив указателей на массивы, который выделялся
// вручную и освобождался парной функцией. Хранение стало плоским: строка n
// начинается со смещения n * stride, где stride = maxR + 1.
//
// Раскладка выбрана не случайно: при maxR = MAX_SHORT_CODE_LENGTH она
// побайтово совпадает с той, которую ждёт ядро для коротких кодов
// (binomTable[n * (MAX_SHORT_CODE_LENGTH + 1) + k]), поэтому на видеокарту
// таблица уезжает одним memcpy, без промежуточного «уплощения».
class BinomTable
{
public:
    BinomTable() = default;

    BinomTable(quint64 maxN, quint64 maxR)
        : m_stride(maxR + 1)
        , m_data((maxN + 1) * (maxR + 1), 0ULL)
    {
        for (quint64 n = 0; n <= maxN; ++n) {
            for (quint64 r = 0; r <= maxR; ++r) {
                quint64& cell = m_data[n * m_stride + r];
                if (r == 0) {
                    cell = 1;
                } else if (r > n) {
                    cell = 0;                      // сочетаний не существует
                } else {
                    // C(n,r) = C(n-1,r-1) + C(n-1,r)
                    const quint64 a = m_data[(n - 1) * m_stride + r - 1];
                    const quint64 b = m_data[(n - 1) * m_stride + r];
                    if (a > std::numeric_limits<quint64>::max() - b)
                        throw std::overflow_error(
                            "переполнение при построении таблицы биномиальных коэффициентов");
                    cell = a + b;
                }
            }
        }
    }

    quint64 operator()(quint64 n, quint64 r) const { return m_data[n * m_stride + r]; }

    const quint64* data()   const { return m_data.data(); }
    size_t         bytes()  const { return m_data.size() * sizeof(quint64); }
    quint64        stride() const { return m_stride; }
    bool           isEmpty() const { return m_data.empty(); }

private:
    quint64              m_stride = 0;
    std::vector<quint64> m_data;
};
