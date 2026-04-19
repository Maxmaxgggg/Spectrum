#pragma once
#include <qstringlist.h>
#include <qfile.h>
#include <qtextstream.h>
#include <qdir.h>

#include "types.h"

Matrix readMatrixFile(const QString& path);

bool writeMatrixFile(const QString& path, const Matrix& matrix);

bool matricesEqual(const Matrix& a, const Matrix& b);


bool folderHasMatchingMtrx(const QString& folderPath, const Matrix& currentMatrix);

QString findFirstMatchingFolder(const QString& basePath,
    const Matrix& currentMatrix);

QString buildBaseName(const Matrix& matrix);

QString makeUniqueFolderName(const QString& basePath,
    const QString& baseName);

