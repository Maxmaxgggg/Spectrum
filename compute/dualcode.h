#pragma once
#include <QStringList>
#include <QVector>
#include <cstdint>
#include <gmpxx.h>

QStringList generatorToParity(const QStringList& gen);
QStringList computeSpectrumFromDual(quint64* dualSpectrum, int numOfCols, int numOfRows);