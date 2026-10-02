#pragma once
#include <QStringList>
#include <QVector>
#include <cstdint>
#include <vector>
#include <gmpxx.h>
#include "types.h"

// Проверочная матрица кода по порождающей: строки — базис дуального кода.
Matrix generatorToParity(const Matrix& gen);

// Спектр кода по спектру дуального — преобразование Мак-Вильямс:
//
//     A_i = 2^(−dualDimension) · Σ_x B_x · K_i(x),
//
// где K_i — многочлены Кравчука для длины length, а у дуального кода
// 2^dualDimension слов. Числа точные: у длинного кода они бывают больше 2^64,
// поэтому большие целые. Индекс — вес, размер length + 1.
std::vector<mpz_class> macWilliams(const quint64* dualSpectrum, int length, int dualDimension);

// Числа спектра в 64 битах. Что не помещается, насыщается до максимума:
// вложенному расчёту произведения нужны лишь лёгкие веса, а они малы.
QVector<quint64> saturatedCounts(const std::vector<mpz_class>& spectrum);

// Строки «вес - число» для ненулевых весов, числа точные.
SpectrumText spectrumText(const std::vector<mpz_class>& spectrum);
