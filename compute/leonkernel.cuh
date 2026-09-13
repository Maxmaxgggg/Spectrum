#pragma once

// Ядро случайного поиска по информационным множествам (Леон) — см.
// leonsearch.h. Блок нитей = одна попытка: копия матрицы приводится к
// систематическому виду на случайном множестве, потом блок перебирает
// комбинации до rowsPerTrial строк и слова веса не больше maxWeight
// выкладывает в общий буфер. Дедупликация и подсчёт поимок — на хосте, как и
// у CPU-пути: таблица слов там.
//
// Два варианта одного ядра. Короткие матрицы живут в разделяемой памяти
// блока; что не влезло — в глобальной, у каждого блока свой кусок рабочего
// буфера. Второй вариант медленнее, но строки до 2048 бит ему по силам.

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>

struct LeonLaunch
{
    const uint64_t* matrix       = nullptr;   // k x wordsPerRow, в глобальной памяти
    int             rows         = 0;
    int             cols         = 0;
    int             wordsPerRow  = 0;
    int             rowsPerTrial = 0;
    int             maxWeight    = 0;
    uint64_t        firstTrial   = 0;         // номер первой попытки — затравка
    int             trials       = 0;         // блоков в запуске
    uint64_t*       outWords     = nullptr;   // capacity x wordsPerRow
    unsigned*       outCount     = nullptr;   // сколько слов выложено (может превысить capacity)
    unsigned        capacity     = 0;
    // Рабочий буфер варианта с матрицей в глобальной памяти:
    // trials x rows x leonPaddedWords(wordsPerRow) слов. Не нужен, если
    // матрица помещается в разделяемую память.
    uint64_t*       scratch      = nullptr;
};

// Слов в строке рабочей копии: число слов округляется вверх до одного из
// заготовленных вариантов ядра, хвост нулевой. Ноль — строка длиннее, чем
// ядро умеет (MAX_BLOCKWORDS).
int leonPaddedWords(int wordsPerRow);

// true — матрица помещается в разделяемую память блока и рабочий буфер в
// глобальной не нужен.
bool leonFitsShared(int rows, int cols, int wordsPerRow);

// Разделяемой памяти на блок для этой матрицы (с учётом варианта).
size_t leonSharedBytes(int rows, int cols, int wordsPerRow);

// Бросает std::invalid_argument, если запуск не удался.
void launchLeonTrials(const LeonLaunch& launch, int threadsPerBlock, cudaStream_t stream);
