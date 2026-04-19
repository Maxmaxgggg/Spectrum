#pragma once
#include <QStringList>
#include <QVector>
#include <cstdint>
#include <gmpxx.h>
#include "types.h"


Matrix		 generatorToParity(const Matrix& gen);
SpectrumText computeSpectrumFromDual(quint64* dualSpectrum, int numOfCols, int numOfRows);