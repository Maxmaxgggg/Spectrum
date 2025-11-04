#pragma once
#include <QStringList>
#include <QVector>
#include <cstdint>
#include <gmpxx.h>


QStringList generatorToParity(const QStringList& gen);
bool        computeSpectrumFromDual(quint64* dualSpectrum, int n, int k);