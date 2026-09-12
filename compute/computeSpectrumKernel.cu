#include <string>
#include <vector>
#include <stdexcept>

#include "computeSpectrumKernel.cuh"

// Порождающая матрица в константной памяти.
// Имя намеренно не d_matrix: так называется параметр ядра длинных кодов, и
// раньше одно перекрывало другое.
__constant__ quint64 c_matrix[Constants::MAX_CONST_WORDS];

// Маски информационных множеств Брауэра–Циммермана, по MAX_BLOCKWORDS слов на
// множество. Все нити варпа читают одно и то же слово одной и той же маски,
// а это ровно тот доступ, под который константная память и сделана.
__constant__ quint64 c_masks[Constants::MASKS_CONST_WORDS];
static_assert(Constants::MASKS_CONST_WORDS == Constants::MAX_INFO_SETS * Constants::MAX_BLOCKWORDS,
              "маски раскладываются с шагом MAX_BLOCKWORDS");

// Строка матрицы из константной памяти. Короткие коды в неё помещаются всегда
// (хост это проверяет), поэтому выбирать тут не из чего.
__device__ __forceinline__ quint64 readConstMatrixWord(int row, int wordIdx, int wordsPerRow)
{
    return c_matrix[(size_t)row * wordsPerRow + wordIdx];
}

// Длинный код может не влезть в константную память — тогда матрица лежит в
// глобальной и приходит указателем. nullptr означает «она в константной».
//
// Чтение через __ldg здесь пробовалось и было убрано: на матрице в 180 КБ оно
// дало 0.48-0.53 с против 0.44-0.50 без него, то есть не помогло, а скорее
// чуть помешало. Матрица такого размера в кэш неизменяемых данных всё равно не
// помещается, а доступ разбросанный.
__device__ __forceinline__ quint64 readMatrixWord(const quint64* matrixGlobal, int row, int wordIdx, int wordsPerRow)
{
    return matrixGlobal ? matrixGlobal[(size_t)row * wordsPerRow + wordIdx]
                        : readConstMatrixWord(row, wordIdx, wordsPerRow);
}
// Правило единственности Брауэра–Циммермана. Слово собрано из r строк
// матрицы множества slot.setIndex, значит на его столбцах у слова ровно r
// единиц. Засчитывает слово то множество, на котором единиц меньше всего, а
// при равенстве — первое по порядку: прежние множества обязаны видеть больше
// r единиц, последующие — не меньше r. Иначе слово уже посчитано или будет
// посчитано другим множеством.
template <int WORDS>
__device__ __forceinline__ bool bzKeep(const quint64* codeword, int words, int r, MatrixSlot slot)
{
    for (int i = 0; i < slot.setCount; ++i) {
        if (i == slot.setIndex)
            continue;
        const quint64* mask = c_masks + i * Constants::MAX_BLOCKWORDS;
        int ones = 0;
        #pragma unroll
        for (int w = 0; w < words; ++w)
            ones += __popcll(codeword[w] & mask[w]);
        if (i < slot.setIndex ? ones <= r : ones < r)
            return false;
    }
    return true;
}

__device__ inline quint64 getBinome(const quint64* binomTable, int n, int k) {
    return binomTable[n * (Constants::MAX_SHORT_CODE_LENGTH + 1) + k];
}

// Функция для генерации битовых масок на GPU
__device__ inline quint64 generateBitMaskGPU(const quint64* binomTable, unsigned k, unsigned r, quint64 idx) {
    if (r == 0) return 0ULL;
    if (r > k) return 0ULL;

    quint64 mask = 0ULL;
    unsigned nextPos = 0;
    quint64 rank = idx;

    for (unsigned i = r; i > 0; --i) {
        unsigned j = nextPos;
        while (j <= k - i) {
            quint64 c = getBinome(binomTable, k - j - 1, i - 1);
            if (c <= rank) {
                rank -= c;
                ++j;
            }
            else break;
        }
        mask |= (1ULL << j);
        nextPos = j + 1;
    }
    return mask;
}

__device__ inline int bitPosFromSingleBit(quint64 x) {
    return __ffsll(x) - 1;
}

// Следующая маска с тем же числом единиц в возрастающем числовом порядке —
// приём Госпера. Деление из классической записи заменено сдвигом: младший
// установленный бит есть степень двойки, и его позиция и есть величина сдвига.
//
// Вызывать только при v != 0 и только когда следующая комбинация существует.
__device__ __forceinline__ quint64 gosperNext(quint64 v)
{
    const int     t  = __ffsll(v) - 1;      // позиция младшей единицы
    const quint64 rr = v + (1ULL << t);
    return rr | ((v ^ rr) >> (t + 2));
}

// Разворот младших k бит: бит p переходит в позицию k-1-p.
__device__ __forceinline__ quint64 reverseLowBits(quint64 v, int k)
{
    return __brevll(v) >> (64 - k);
}
// Функция для генерации следующего массива позиций из текущего
__device__ __forceinline__ bool nextPositions(int16_t* a, int k, int n)
{
    // a[0..k-1] — строго возрастающий массив позиций

    int i = k - 1;

    // Ищем самый правый элемент, который ещё можно увеличить
    while (i >= 0 && a[i] == n - k + i)
        --i;

    // Если такого нет — это последняя комбинация
    if (i < 0)
        return false;

    // Увеличиваем его
    ++a[i];

    for (int j = i + 1; j < k; ++j)
        a[j] = a[i] + (j - i);

    return true;
}
// Функция для получения отличающихся элементов между двумя массивами позиций
__device__ __forceinline__ void diffPositions(
    const int16_t* prevPositions,
    const int16_t* currPositions,
    int            numOfPositions,
    int16_t*       changedPositions,
    int&           numChanged
) {
    int i = 0, j = 0;
    numChanged = 0;

    while ( i < numOfPositions || j < numOfPositions ) {
        if ( j == numOfPositions || (i < numOfPositions && prevPositions[i] < currPositions[j]) ) {
            changedPositions[numChanged++] = prevPositions[i++];
        }
        else if (i == numOfPositions || currPositions[j] < prevPositions[i]) {
            changedPositions[numChanged++] = currPositions[j++];
        }
        else {
            ++i;
            ++j;
        }
    }
}
__host__ cudaError_t copyMasksToConstant(const quint64* h_masks, int setCount, int wordsPerRow)
{
    if (setCount < 1 || setCount > Constants::MAX_INFO_SETS
        || wordsPerRow < 1 || wordsPerRow > Constants::MAX_BLOCKWORDS)
        throw std::invalid_argument("маски множеств не помещаются в константную память");

    // Раскладка с шагом MAX_BLOCKWORDS; хвост каждой маски нулевой, чтобы ядро
    // с округлённым вверх числом слов читало нули, а не соседнюю маску.
    std::vector<quint64> packed(size_t(Constants::MASKS_CONST_WORDS), 0ULL);
    for (int i = 0; i < setCount; ++i)
        for (int w = 0; w < wordsPerRow; ++w)
            packed[size_t(i) * Constants::MAX_BLOCKWORDS + w] = h_masks[size_t(i) * wordsPerRow + w];
    return cudaMemcpyToSymbol(c_masks, packed.data(), packed.size() * sizeof(quint64), 0,
                              cudaMemcpyHostToDevice);
}

// Функция для копирования матрицы с хоста в константную память
__host__ cudaError_t copyMatrixToConstant( const quint64* h_matrix, size_t matrixSizeInWords ) {
    // Определяем число байт для копирования
    size_t bytes = matrixSizeInWords * Constants::WORD_SIZE;
    // Проверяем, что не вышли за пределы отведённой матрице константной памяти
    if (matrixSizeInWords > size_t(Constants::MAX_CONST_WORDS)) {
        // Раньше здесь бросался голый const char*, который никто не ловил.
        throw std::invalid_argument(
            "матрица не помещается в константную память видеокарты");
    }
    // Копируем матрицу в константную память
    return cudaMemcpyToSymbol(c_matrix, h_matrix, bytes, 0, cudaMemcpyHostToDevice);
}

// Проверка того, что запуск ядра вообще состоялся.
//
// Ошибку запуска нигде не проверяли, и провалившийся запуск проходил молча:
// ядро не выполнялось, спектр оставался недосчитанным, а программа
// рапортовала об успехе. Вылезло это при переборе параметров — на 1024 нитях
// в блоке спектр расходился с эталоном без единого сообщения.
static void checkLaunch(const char* what, int blocks, int threads, size_t sharedBytes)
{
    const cudaError_t err = cudaGetLastError();
    if (err == cudaSuccess)
        return;

    throw std::invalid_argument(
        std::string("не удалось запустить ") + what + ": " + cudaGetErrorString(err)
        + " (блоков "  + std::to_string(blocks)
        + ", нитей "   + std::to_string(threads)
        + ", разделяемой памяти " + std::to_string(sharedBytes) + " байт)");
}


// Проверка параметров запуска. Массивы в ядрах фиксированного размера, и выход
// за них — молчаливая порча памяти на устройстве, поэтому ловим до запуска.
// Раньше эти пределы проверялись только в интерфейсе, а Worker и ядра
// принимали что угодно.
static void validateLaunchParams(int wordsPerRow, int numOfCols)
{
    if (wordsPerRow > Constants::MAX_BLOCKWORDS)
        throw std::invalid_argument(
            "слишком длинная строка матрицы: не хватает MAX_BLOCKWORDS");
    if (numOfCols > Constants::MAX_COLS)
        throw std::invalid_argument("число столбцов больше MAX_COLS");
}









// XOR строки матрицы из разделяемой памяти в кодовое слово.
//
// При чётном числе слов читаем по два за раз (16 байт), а не по одному:
// профилировщик показывал, что более половины простоя варпов приходится на
// очередь MIO, куда идут обращения к разделяемой памяти. Вдвое меньше
// обращений — вдвое меньше давление на эту очередь.
//
// Выравнивание обеспечено: гистограмма в разделяемой памяти дополнена до
// чётного числа слов, поэтому строка матрицы начинается с адреса, кратного 16,
// а само кодовое слово объявлено с __align__(16).
template <int WORDS>
__device__ __forceinline__ void xorRowFromShared(quint64* codeword,
                                                 const quint64* row,
                                                 int words)
{
    if (WORDS > 0 && (WORDS % 2) == 0) {
        ulonglong2*       dst = reinterpret_cast<ulonglong2*>(codeword);
        const ulonglong2* src = reinterpret_cast<const ulonglong2*>(row);
        #pragma unroll
        for (int w = 0; w < WORDS / 2; ++w) {
            dst[w].x ^= src[w].x;
            dst[w].y ^= src[w].y;
        }
    } else {
        // Нечётное число слов и запасной путь с рантаймовым размером.
        #pragma unroll
        for (int w = 0; w < words; ++w)
            codeword[w] ^= row[w];
    }
}

// Число слов, известное на этапе компиляции: округляем вверх до ближайшего
// заготовленного варианта. Лишние слова дозаполняются нулями, что безопасно —
// биты за numOfCols в матрице всегда нули, а XOR и popcount с нулём ничего не
// меняют. Ноль означает «подходящего варианта нет, берём размер из аргумента».
static int pickWordCount(int wordsPerRow)
{
    static const int sizes[] = { 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 28, 32 };
    for (int candidate : sizes)
        if (candidate >= wordsPerRow) return candidate;
    return 0;
}


// Определение ниже; здесь оно нужно обёртке запуска.
template <int WORDS>
__global__ void computeSpectrumKernelShortT(
    quint64* d_spectrum, const quint64* d_binomTable,
    int n, int k, int blockCount,
    quint64 chunkOffset, quint64 chunkSize, quint64 r, MatrixSlot slot);

template <int WORDS>
__global__ void computeSpectrumKernelGrayShortT(
    quint64* d_spectrum, int n, int k, int blockCount,
    quint64 chunkOffset, quint64 chunkSize);

__host__ void launchSpectrumKernelShort(
    quint64* d_spectrum,
    const quint64* d_binomTable,
    int numOfBlocks,
    int threadsPerBlock,
    cudaStream_t stream,
    int n,
    int k,
    int blockCount,
    quint64 chunkOffset,
    quint64 chunkSize,
    quint64 r,
    MatrixSlot slot)
{
    validateLaunchParams(blockCount, n);

    // Выбираем вариант ядра, у которого число слов известно на этапе
    // компиляции. Округляем вверх до ближайшего заготовленного: лишние слова
    // заполняются нулями, а XOR и popcount с нулём результата не меняют.
    const int words = pickWordCount(blockCount);

    // Гистограмма плюс копия матрицы: она уезжает в разделяемую память, потому
    // что нити варпа читают разные строки, а константная память такое дробит.
    // Гистограмма дополнена до чётного числа слов: так строка матрицы ложится
    // на адрес, кратный 16, и читается по два слова за раз.
    const size_t sharedBytes =
        (size_t)(((n + 2) & ~1) + k * (words > 0 ? words : blockCount)) * sizeof(quint64);

    // Одной строкой намеренно: перенос со слэшем внутри define читается хуже,
    // чем длинная строка.
    #define LAUNCH_SHORT(W) computeSpectrumKernelShortT<W><<<numOfBlocks, threadsPerBlock, sharedBytes, stream>>>(d_spectrum, d_binomTable, n, k, blockCount, chunkOffset, chunkSize, r, slot)

    switch (words) {
        case  1: LAUNCH_SHORT( 1); break;   case  2: LAUNCH_SHORT( 2); break;
        case  3: LAUNCH_SHORT( 3); break;   case  4: LAUNCH_SHORT( 4); break;
        case  5: LAUNCH_SHORT( 5); break;   case  6: LAUNCH_SHORT( 6); break;
        case  7: LAUNCH_SHORT( 7); break;   case  8: LAUNCH_SHORT( 8); break;
        case 10: LAUNCH_SHORT(10); break;   case 12: LAUNCH_SHORT(12); break;
        case 14: LAUNCH_SHORT(14); break;   case 16: LAUNCH_SHORT(16); break;
        case 20: LAUNCH_SHORT(20); break;   case 24: LAUNCH_SHORT(24); break;
        case 28: LAUNCH_SHORT(28); break;   case 32: LAUNCH_SHORT(32); break;
        // Запасной путь: размер берётся из аргумента, кодовое слово живёт в
        // локальной памяти. Сюда попасть не должно.
        default: LAUNCH_SHORT( 0); break;
    }
    #undef LAUNCH_SHORT

    checkLaunch("ядро коротких кодов", numOfBlocks, threadsPerBlock, sharedBytes);
}


// WORDS — число 64-битных слов в строке матрицы, известное на этапе компиляции.
// Ноль означает «берём из аргумента».
//
// Разница принципиальная. При динамическом размере codeword индексируется
// переменной, а массив с динамическим индексом в регистрах держать нельзя, и
// компилятор кладёт его в локальную память, то есть в DRAM. Профилировщик
// показывал ровно это: загрузка DRAM 73 %, вычислителей 16 %, при 40 занятых
// регистрах из 255. Каждый XOR и каждый popcount кодового слова ходил в память.
//
// При известном WORDS цикл разворачивается, массив живёт в регистрах, и
// обращений к памяти не остаётся вовсе.
template <int WORDS>
__global__ void computeSpectrumKernelShortT(
    quint64* d_spectrum,
    const quint64* d_binomTable,
    int n,
    int k,
    int blockCount,
    quint64 chunkOffset,
    quint64 chunkSize,
    quint64 r,
    MatrixSlot slot)
{
    const int words = WORDS > 0 ? WORDS : blockCount;

    // Разделяемая память делится на две части: гистограмма и копия матрицы.
    //
    // Матрица лежит в константной памяти, а та оптимизирована под чтение всеми
    // нитями варпа одного адреса. У нас же у каждой нити своя маска, то есть
    // своя строка матрицы, и запрос дробится на столько обращений, сколько
    // различных адресов в варпе — до 32-кратной сериализации. Разделяемая
    // память разложена по банкам и расхождение адресов переносит нормально.
    extern __shared__ quint64 s_mem[];
    quint64* const s_spectrum = s_mem;
    // Гистограмма дополнена до чётного числа слов, чтобы строка матрицы
    // начиналась с адреса, кратного 16, и её можно было читать по два слова.
    quint64* const s_matrix   = s_mem + ((n + 2) & ~1);

    const int tid = threadIdx.x;

    for (int i = tid; i <= n; i += blockDim.x) s_spectrum[i] = 0ULL;
    // Шаг копии — words, а не blockCount: если WORDS округлён вверх, лишние
    // слова заполняются нулями. Это безопасно, потому что биты за numOfCols в
    // матрице всегда нули, и XOR с нулём ничего не меняет.
    for (int i = tid; i < k * words; i += blockDim.x) {
        const int row = i / words;
        const int w   = i % words;
        s_matrix[i] = (w < blockCount) ? readConstMatrixWord(slot.rowBase + row, w, blockCount) : 0ULL;
    }
    __syncthreads();

    const quint64 globalThreadIdx = (quint64)blockIdx.x * blockDim.x + tid;
    const quint64 totalThreads    = (quint64)gridDim.x * blockDim.x;

    const quint64 combosPerThread = (chunkSize + totalThreads - 1) / totalThreads;
    const quint64 start           = globalThreadIdx * combosPerThread;

    // Нить без работы не выходит из ядра: ей ещё стоять на барьере и
    // участвовать в редукции. Раньше здесь был return, и корректность держалась
    // на том, что у нити 0 наименьший индекс в блоке, — если бы вышла она,
    // блок молча потерял бы весь накопленный спектр.
    if (start < chunkSize) {
        quint64 end = start + combosPerThread;
        if (end > chunkSize) end = chunkSize;
        const quint64 count = end - start;

        __align__(16) quint64 codeword[WORDS > 0 ? WORDS : Constants::MAX_BLOCKWORDS];
        #pragma unroll
        for (int b = 0; b < words; ++b) codeword[b] = 0ULL;

        // Комбинации нумеруются лексикографически по возрастанию позиций —
        // этот порядок задаёт смысл chunkOffset и менять его нельзя, иначе
        // старые чекпоинты станут указывать не туда.
        //
        // Развернув биты маски (позиция p -> k-1-p), получаем ту же
        // последовательность в убывающем ЧИСЛОВОМ порядке. А по числовому
        // порядку умеет шагать приём Госпера — за несколько операций вместо
        // разбора ранга по таблице биномов, то есть до k обращений в память.
        //
        // Поэтому берём последний ранг диапазона (ему отвечает наименьший
        // числовой индекс) и идём Госпером вперёд. Внутри нити порядок обхода
        // получается обратным, но для гистограммы это безразлично: набор
        // комбинаций тот же самый.
        quint64 revMask = reverseLowBits(
            generateBitMaskGPU(d_binomTable, (unsigned)k, (unsigned)r,
                               chunkOffset + end - 1), k);

        // Бит p развёрнутой маски соответствует строке k-1-p.
        quint64 temp = revMask;
        while (temp) {
            const int p = bitPosFromSingleBit(temp & -temp);
            temp &= (temp - 1);
            xorRowFromShared<WORDS>(codeword, &s_matrix[(k - 1 - p) * words], words);
        }

        // Подряд идущие кодовые слова часто имеют одинаковый вес, поэтому
        // копим серию и сбрасываем её одной атомарной операцией.
        //
        // Это не микрооптимизация: у вырожденных матриц вес внутри слоя не
        // меняется вообще. Скажем, у единичной матрицы строка i это e_i,
        // кодовое слово равно самой маске, и все C(n,r) слов слоя имеют вес
        // ровно r — весь блок бил атомарными операциями в одну ячейку общей
        // памяти, и железо их сериализовало. Замерено: снятие конкуренции
        // ускоряло такой расчёт втрое, до уровня случайной матрицы.
        // Вес -1 означает «не засчитывать»: слово уже учтено или будет учтено
        // другим множеством Брауэра–Циммермана. В обычном расчёте множество
        // одно, и проверка не вызывается.
        int     runWeight = 0;
        #pragma unroll
            for (int w = 0; w < words; ++w) runWeight += __popcll(codeword[w]);
        if (slot.setCount > 1 && !bzKeep<WORDS>(codeword, words, int(r), slot))
            runWeight = -1;
        quint64 runLength = 1;

        // При r = 0 и r = k комбинация всего одна, и цикл не выполняется —
        // gosperNext на нулевой маске звать нельзя.
        for (quint64 i = 1; i < count; ++i) {
            const quint64 nextRev = gosperNext(revMask);

            // Вошедшие и вышедшие строки XOR-ятся одинаково: XOR сам себе
            // обратен, разделять их незачем.
            quint64 changed = revMask ^ nextRev;
            while (changed) {
                const int p = bitPosFromSingleBit(changed & -changed);
                changed &= (changed - 1);
                xorRowFromShared<WORDS>(codeword, &s_matrix[(k - 1 - p) * words], words);
            }

            int weight = 0;
            #pragma unroll
            for (int w = 0; w < words; ++w) weight += __popcll(codeword[w]);
            if (slot.setCount > 1 && !bzKeep<WORDS>(codeword, words, int(r), slot))
                weight = -1;

            if (weight == runWeight) {
                ++runLength;
            } else {
                if (runWeight >= 0)
                    atomicAdd(&s_spectrum[runWeight], runLength);
                runWeight = weight;
                runLength = 1;
            }

            revMask = nextRev;
        }
        if (runWeight >= 0)
            atomicAdd(&s_spectrum[runWeight], runLength);
    }

    __syncthreads();

    // Редукция всеми нитями блока. Раньше её целиком делала нить 0: при
    // n = 2048 это 2049 атомарных операций подряд, пока остальные простаивают.
    for (int i = tid; i <= n; i += blockDim.x) {
        const quint64 v = s_spectrum[i];
        if (v) atomicAdd(&d_spectrum[i], v);
    }
}
// Обертка для ядра для расчета частичных спектров длинных кодов
template <int WORDS>
__global__ void computeSpectrumKernelLongT(
    uint64_t* d_spectrum, const uint64_t* matrixGlobal,
    int numCols, int numRows, int wordsPerRow, uint64_t chunkSize,
    int16_t* d_startPositions, uint64_t masksPerThread,
    uint64_t numStartMasks, uint64_t numOfOnes,
    uint64_t* d_maskCounter, bool stageMatrix, MatrixSlot slot);

__host__ void launchSpectrumKernelLong(
    int numBlocks,
    int threadsPerBlock,
    cudaStream_t stream,
    uint64_t* d_spectrum,
    const uint64_t* matrixGlobal,
    int numCols,
    int numRows,
    int wordsPerRow,
    uint64_t chunkSize,
    int16_t* d_startPositions,
    uint64_t masksPerThread,
    uint64_t numStartMasks,
    uint64_t numOfOnes,
    uint64_t* d_maskCounter,
    MatrixSlot slot
) {
    validateLaunchParams(wordsPerRow, numCols);
    if (numOfOnes > Constants::MAX_POSITIONS)
        throw std::invalid_argument(
            "число складываемых строк больше MAX_POSITIONS");

    // Матрицу выгодно держать в разделяемой памяти: нити варпа читают разные
    // строки, а и константная память такой запрос дробит, и глобальная тут не
    // лучший вариант. Но у длинных кодов матрица бывает до полумегабайта, и
    // тогда она туда просто не помещается — в этом случае читаем как раньше.
    const int words = pickWordCount(wordsPerRow);

    // Матрицу выгодно держать в разделяемой памяти: нити варпа читают разные
    // строки, а константная память такой запрос дробит. Но у длинных кодов
    // матрица бывает до полумегабайта, и тогда она туда не помещается — в этом
    // случае читаем как раньше.
    const size_t histogramBytes = (size_t)((numCols + 2) & ~1) * sizeof(uint64_t);
    const size_t matrixBytes =
        (size_t)numRows * (words > 0 ? words : wordsPerRow) * sizeof(uint64_t);

    const bool   stageMatrix = (histogramBytes + matrixBytes) <= Constants::MAX_SHARED_BYTES;
    const size_t sharedBytes = histogramBytes + (stageMatrix ? matrixBytes : 0);

    #define LAUNCH_LONG(W) computeSpectrumKernelLongT<W><<<numBlocks, threadsPerBlock, sharedBytes, stream>>>(d_spectrum, matrixGlobal, numCols, numRows, wordsPerRow, chunkSize, d_startPositions, masksPerThread, numStartMasks, numOfOnes, d_maskCounter, stageMatrix, slot)

    switch (words) {
        case  1: LAUNCH_LONG( 1); break;   case  2: LAUNCH_LONG( 2); break;
        case  3: LAUNCH_LONG( 3); break;   case  4: LAUNCH_LONG( 4); break;
        case  5: LAUNCH_LONG( 5); break;   case  6: LAUNCH_LONG( 6); break;
        case  7: LAUNCH_LONG( 7); break;   case  8: LAUNCH_LONG( 8); break;
        case 10: LAUNCH_LONG(10); break;   case 12: LAUNCH_LONG(12); break;
        case 14: LAUNCH_LONG(14); break;   case 16: LAUNCH_LONG(16); break;
        case 20: LAUNCH_LONG(20); break;   case 24: LAUNCH_LONG(24); break;
        case 28: LAUNCH_LONG(28); break;   case 32: LAUNCH_LONG(32); break;
        default: LAUNCH_LONG( 0); break;
    }
    #undef LAUNCH_LONG

    checkLaunch("ядро длинных кодов", numBlocks, threadsPerBlock, sharedBytes);
}
// Шаблон по числу слов — как и в коротких ядрах, чтобы codeword жил в
// регистрах, а не в локальной памяти.
//
// Массивы позиций a/old_a/changed остаются в локальной памяти: их размер
// зависит от числа единиц в маске, известного только в рантайме, да и
// nextPositions принимает указатель. Но горячие данные здесь именно codeword —
// его XOR-ят на каждой изменившейся позиции и считают popcount на каждой маске.
template <int WORDS>
__global__ void computeSpectrumKernelLongT(
    uint64_t* d_spectrum,
    const uint64_t* matrixGlobal,
    int             numCols,
    int             numRows,
    int             wordsPerRow,
    uint64_t        chunkSize,
    int16_t* d_startPositions,
    uint64_t        masksPerThread,
    uint64_t        numStartMasks,
    uint64_t        numOfOnes,
    uint64_t*       d_maskCounter,
    bool            stageMatrix,
    MatrixSlot      matrixSlot
)
{
    const int words = WORDS > 0 ? WORDS : wordsPerRow;

    extern __shared__ uint64_t s_mem[];
    uint64_t* const s_spectrum = s_mem;
    // Копия матрицы идёт следом за гистограммой. Если она не влезла, хост
    // передаёт stageMatrix = false, и этой части просто нет.
    // Гистограмма дополнена до чётного числа слов — см. короткие ядра.
    uint64_t* const s_matrix   = s_mem + ((numCols + 2) & ~1);

    int tid = threadIdx.x;
    uint64_t gtid =
        (uint64_t)blockIdx.x * blockDim.x + (uint64_t)tid;

    /* -------- init shared histogram -------- */
    for (int i = tid; i <= numCols; i += blockDim.x)
        s_spectrum[i] = 0ULL;

    /* -------- копия матрицы, если она туда влезла -------- */
    if (stageMatrix) {
        for (int i = tid; i < numRows * words; i += blockDim.x) {
            const int row = i / words;
            const int w   = i % words;
            s_matrix[i] = (w < wordsPerRow)
                        ? readMatrixWord(matrixGlobal, matrixSlot.rowBase + row, w, wordsPerRow) : 0ULL;
        }
    }

    __syncthreads();

    // Условие одинаково для всего блока, поэтому ветвление здесь не разводит
    // нити варпа — компилятор выносит проверку из цикла.
    const uint64_t* const matrixSrc = stageMatrix ? s_matrix : nullptr;

    /* -------- activity predicate -------- */
    uint64_t startRank = gtid * masksPerThread;

    bool threadIsActive =
        (gtid < numStartMasks) &&
        (startRank < chunkSize);

    /* -------- per-thread work -------- */
    if (threadIsActive) {

        const int16_t* slot =
            d_startPositions + gtid * Constants::MAX_POSITIONS;

        // positions
        int16_t a[Constants::MAX_POSITIONS];
        for (int i = 0; i < numOfOnes; ++i)
            a[i] = slot[i];

        // codeword
        __align__(16) uint64_t codeword[WORDS > 0 ? WORDS : Constants::MAX_BLOCKWORDS];
        #pragma unroll
        for (int w = 0; w < words; ++w)
            codeword[w] = 0ULL;

        /* ---- first mask ---- */
        for (int i = 0; i < numOfOnes; ++i) {
            int row = a[i];
            if (matrixSrc)
                xorRowFromShared<WORDS>(codeword, &matrixSrc[(size_t)row * words], words);
            else
                for (int w = 0; w < wordsPerRow; ++w)   // именно wordsPerRow
                    codeword[w] ^= readMatrixWord(matrixGlobal, matrixSlot.rowBase + row, w, wordsPerRow);
        }

        int weight = 0;
        #pragma unroll
        for (int w = 0; w < words; ++w)
            weight += __popcll(codeword[w]);

        if (matrixSlot.setCount <= 1 || bzKeep<WORDS>(codeword, words, int(numOfOnes), matrixSlot))
            atomicAdd(&s_spectrum[weight], 1ULL);
        #ifdef _DEBUG
        atomicAdd(d_maskCounter, 1ULL);
        #endif
        /* ---- iterations ---- */
        uint64_t iters = masksPerThread;
        if (startRank + iters > chunkSize)
            iters = chunkSize - startRank;

        int16_t old_a[Constants::MAX_POSITIONS];

        for (uint64_t it = 1; it < iters; ++it) {

            for (int i = 0; i < numOfOnes; ++i)
                old_a[i] = a[i];

            if (!nextPositions(a, numOfOnes, numRows))
                break;

            int16_t changed[2 * Constants::MAX_POSITIONS];
            int numChanged;
            diffPositions(old_a, a, numOfOnes, changed, numChanged);

            if (numChanged > numOfOnes) {
                #pragma unroll
        for (int w = 0; w < words; ++w)
                    codeword[w] = 0ULL;

                for (int i = 0; i < numOfOnes; ++i) {
                    int row = a[i];
                    if (matrixSrc)
                        xorRowFromShared<WORDS>(codeword, &matrixSrc[(size_t)row * words], words);
                    else
                        for (int w = 0; w < wordsPerRow; ++w)   // именно wordsPerRow
                            codeword[w] ^= readMatrixWord(matrixGlobal, matrixSlot.rowBase + row, w, wordsPerRow);
                }
            }
            else {
                for (int t = 0; t < numChanged; ++t) {
                    int row = changed[t];
                    if (matrixSrc)
                        xorRowFromShared<WORDS>(codeword, &matrixSrc[(size_t)row * words], words);
                    else
                        for (int w = 0; w < wordsPerRow; ++w)   // именно wordsPerRow
                            codeword[w] ^= readMatrixWord(matrixGlobal, matrixSlot.rowBase + row, w, wordsPerRow);
                }
            }

            weight = 0;
            #pragma unroll
        for (int w = 0; w < words; ++w)
                weight += __popcll(codeword[w]);

            if (matrixSlot.setCount <= 1 || bzKeep<WORDS>(codeword, words, int(numOfOnes), matrixSlot))
                atomicAdd(&s_spectrum[weight], 1ULL);
            #ifdef _DEBUG
            atomicAdd(d_maskCounter, 1ULL);
            #endif
        }
    }

    /* -------- REQUIRED barrier -------- */
    __syncthreads();

    /* -------- merge shared -> global -------- */
    for (int i = tid; i <= numCols; i += blockDim.x) {
        uint64_t v = s_spectrum[i];
        if (v)
            atomicAdd(&d_spectrum[i], v);
    }
}


// Обертка для ядра для расчета полного спектра с использованием кода Грея для кодов с k < 64
__host__ void launchSpectrumKernelGrayShort(
    int numOfBlocks,
    int threadsPerBlock,
    cudaStream_t stream,
    quint64* d_spectrum,
    int n,
    int k,
    int blockCount,
    quint64 chunkOffset,   // индекс Gray-элемента начала чанка
    quint64 chunkSize      // сколько Gray-элементов в чанке
) {
    validateLaunchParams(blockCount, n);
    const int words = pickWordCount(blockCount);
    // Гистограмма плюс копия матрицы — см. ядро простого XOR.
    // Гистограмма дополнена до чётного числа слов: так строка матрицы ложится
    // на адрес, кратный 16, и читается по два слова за раз.
    const size_t sharedBytes =
        (size_t)(((n + 2) & ~1) + k * (words > 0 ? words : blockCount)) * sizeof(quint64);

    #define LAUNCH_GRAY(W) computeSpectrumKernelGrayShortT<W><<<numOfBlocks, threadsPerBlock, sharedBytes, stream>>>(d_spectrum, n, k, blockCount, chunkOffset, chunkSize)

    switch (words) {
        case  1: LAUNCH_GRAY( 1); break;   case  2: LAUNCH_GRAY( 2); break;
        case  3: LAUNCH_GRAY( 3); break;   case  4: LAUNCH_GRAY( 4); break;
        case  5: LAUNCH_GRAY( 5); break;   case  6: LAUNCH_GRAY( 6); break;
        case  7: LAUNCH_GRAY( 7); break;   case  8: LAUNCH_GRAY( 8); break;
        case 10: LAUNCH_GRAY(10); break;   case 12: LAUNCH_GRAY(12); break;
        case 14: LAUNCH_GRAY(14); break;   case 16: LAUNCH_GRAY(16); break;
        case 20: LAUNCH_GRAY(20); break;   case 24: LAUNCH_GRAY(24); break;
        case 28: LAUNCH_GRAY(28); break;   case 32: LAUNCH_GRAY(32); break;
        default: LAUNCH_GRAY( 0); break;
    }
    #undef LAUNCH_GRAY

    checkLaunch("ядро кода Грея", numOfBlocks, threadsPerBlock, sharedBytes);
}

// Шаблон по числу слов — по той же причине, что и в ядре простого XOR:
// при динамическом размере codeword уезжает в локальную память, то есть в DRAM.
template <int WORDS>
__global__ void computeSpectrumKernelGrayShortT(
    quint64* d_spectrum,
    int n,
    int k,
    int blockCount,
    quint64 chunkOffset,   // начало (в Gray-порядке)
    quint64 chunkSize
) {
    const int words = WORDS > 0 ? WORDS : blockCount;
    // Разделяемая память: гистограмма и копия матрицы. Нити варпа читают разные
    // строки, а константная память дробит такой запрос на отдельные обращения.
    extern __shared__ quint64 s_mem[];
    quint64* const s_spectrum = s_mem;
    // Гистограмма дополнена до чётного числа слов, чтобы строка матрицы
    // начиналась с адреса, кратного 16, и её можно было читать по два слова.
    quint64* const s_matrix   = s_mem + ((n + 2) & ~1);

    const int tid = threadIdx.x;

    // 1) инициализация shared
    for (int i = tid; i <= n; i += blockDim.x) s_spectrum[i] = 0ULL;
    for (int i = tid; i < k * words; i += blockDim.x) {
        const int row = i / words;
        const int w   = i % words;
        s_matrix[i] = (w < blockCount) ? readConstMatrixWord(row, w, blockCount) : 0ULL;
    }
    __syncthreads();

    const quint64 globalThreadIdx = (quint64)blockIdx.x * blockDim.x + tid;
    const quint64 totalThreads    = (quint64)gridDim.x * blockDim.x;

    // 2) строгое равномерное разбиение [0..chunkSize)
    const quint64 base       = chunkSize / totalThreads;
    const quint64 rem        = chunkSize % totalThreads;
    const quint64 startLocal = globalThreadIdx * base
                             + (globalThreadIdx < rem ? globalThreadIdx : rem);
    const quint64 cnt        = base + (globalThreadIdx < rem ? 1 : 0);

    // Нить без работы не выходит: барьер и редукция ниже общие для блока.
    if (cnt > 0) {
        const quint64 endLocal = startLocal + cnt; // exclusive

        // 3) подготовка local codeword
        __align__(16) quint64 codeword[WORDS > 0 ? WORDS : Constants::MAX_BLOCKWORDS];
        #pragma unroll
        for (int b = 0; b < words; ++b) codeword[b] = 0ULL;

        // 4) маска для k бит
        const quint64 maskAll = (k >= 64) ? ~0ULL : ((1ULL << k) - 1ULL);

        auto gray_of = [] __device__(quint64 i) -> quint64 { return (i ^ (i >> 1)); };

        // 5) начальная маска
        quint64 mask = (gray_of(chunkOffset + startLocal) & maskAll);

        // 6) полный XOR для начальной маски
        quint64 temp = mask;
        while (temp) {
            const int pos = bitPosFromSingleBit(temp & (~temp + 1ULL));
            temp &= (temp - 1ULL);
            xorRowFromShared<WORDS>(codeword, &s_matrix[pos * words], words);
        }

        // 7) аккумулируем вес. Как и в ядре простого XOR, копим серию
        // одинаковых весов и сбрасываем её одной атомарной операцией.
        int     runWeight = 0;
        #pragma unroll
            for (int w = 0; w < words; ++w) runWeight += __popcll(codeword[w]);
        quint64 runLength = 1;

        // 8) основной цикл по локальному диапазону (без перекрытий).
        // У соседних кодов Грея различается ровно один бит, поэтому цикл по
        // изменившимся битам делает здесь один проход.
        for (quint64 local = startLocal + 1; local < endLocal; ++local) {
            const quint64 next_mask = (gray_of(chunkOffset + local) & maskAll);

            quint64 changed = mask ^ next_mask;
            while (changed) {
                const int pos = bitPosFromSingleBit(changed & (~changed + 1ULL));
                changed &= (changed - 1ULL);
                xorRowFromShared<WORDS>(codeword, &s_matrix[pos * words], words);
            }

            int weight = 0;
            #pragma unroll
            for (int w = 0; w < words; ++w) weight += __popcll(codeword[w]);

            if (weight == runWeight) {
                ++runLength;
            } else {
                atomicAdd(&s_spectrum[runWeight], runLength);
                runWeight = weight;
                runLength = 1;
            }
            mask = next_mask;
        }
        atomicAdd(&s_spectrum[runWeight], runLength);
    }

    __syncthreads();

    // 9) редукция shared -> global, всеми нитями блока
    for (int i = tid; i <= n; i += blockDim.x) {
        const quint64 v = s_spectrum[i];
        if (v) atomicAdd(&d_spectrum[i], v);
    }
}







