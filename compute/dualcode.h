#pragma once
#include <QStringList>
#include <QVector>
#include <cstdint>
#include <gmpxx.h>
#include "types.h"


Matrix		 generatorToParity(const Matrix& gen);
SpectrumText computeSpectrumFromDual(quint64* dualSpectrum, int numOfCols, int numOfRows);

// То же преобразование Мак-Вильямс, но числами: A_w исходного кода по спектру
// проверочной матрицы (numOfRows — её строк). Что не влезает в 64 бита —
// насыщается до максимума: вложенному расчёту произведения нужны лишь
// лёгкие веса, а они малы.
QVector<quint64> spectrumFromDual(const quint64* dualSpectrum, int numOfCols, int numOfRows);