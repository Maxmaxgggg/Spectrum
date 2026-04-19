#include <save.h>

// Функция для чтения матрицы из файла
Matrix readMatrixFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    QTextStream in(&f);
    Matrix result;

    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (!line.isEmpty())
            result << line;
    }

    return result;
}
bool writeMatrixFile(const QString& path, const Matrix& matrix)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;

    QTextStream out(&f);

    for (const QString& line : matrix) {
        out << line << '\n';
    }

    return true;
}
// Функция для проверки равенства двух матриц
bool matricesEqual(const Matrix& a, const Matrix& b)
{
    if (a.size() != b.size())
        return false;

    for (int i = 0; i < a.size(); ++i) {
        if (a[i].trimmed() != b[i].trimmed())
            return false;
    }

    return true;
}

// Функция для поиска в folderPath матрицы currentMatrix
bool folderHasMatchingMtrx(const QString& folderPath, const Matrix& currentMatrix)
{
    QDir dir(folderPath);

    // Собираем все файлы с расширением .mtrx в папке
    QFileInfoList files = dir.entryInfoList(
        QStringList() << "*.mtrx",
        QDir::Files
    );

    // Проходимся по всем найденым файлам и ищем совпадение
    for (const QFileInfo& file : files) {
        Matrix fileMatrix = readMatrixFile(file.absoluteFilePath());

        if (matricesEqual(fileMatrix, currentMatrix))
            return true;
    }

    return false;
}

// Функция для currentMatrix во всех папках рядом с .exe файлом
QString findFirstMatchingFolder(const QString& basePath,
    const Matrix& currentMatrix)
{
    QDir root(basePath);

    QFileInfoList folders = root.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot
    );

    for (const QFileInfo& folder : folders) {
        if (folderHasMatchingMtrx(folder.absoluteFilePath(), currentMatrix))
            return folder.fileName(); // имя папки
    }

    return "";
}


// Функция для генерации названия матрицы
QString buildBaseName(const Matrix& matrix)
{

    return QString::fromUtf8("Матрица_%1x%2").arg(matrix[0].length()).arg(matrix.length());
}
// Функция для генерации уникального имени папки спектра
QString makeUniqueFolderName(const QString& basePath,
    const QString& baseName)
{
    QDir dir(basePath);

    QString name = baseName;
    int counter = 1;

    while (dir.exists(name)) {
        name = baseName + "_" + QString::number(counter++);
    }

    return name;
}