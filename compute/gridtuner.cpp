#include "gridtuner.h"

#include "computeSpectrumKernel.cuh"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// Блоков берём кратно числу мультипроцессоров: сетка меньше одного блока на
// мультипроцессор оставляет часть карты простаивать, а слишком дробная
// добавляет только накладные расходы на планирование.
const int BLOCKS_PER_SM[] = { 1, 2, 3, 4, 6, 8 };

// 1024 нити не рассматриваются намеренно: варианты ядра на 32 слова занимают
// под сотню регистров на нить, и такой блок просто не запускается. Замер это
// переживёт (неудачный вариант отбрасывается), но время потратит впустую.
const int THREADS[] = { 64, 128, 256, 512 };

// Столько времени должен занимать один замер. Меньше — тонет в шуме, больше —
// подбор становится заметен пользователю.
constexpr double TARGET_MEASURE_SEC = 0.010;

// Сколько раз подряд запускается ядро в одном замере. Верхний предел нужен,
// чтобы на очень быстром чанке подбор не растянулся на секунды.
constexpr int MAX_REPEATS = 64;

// Сколько раз мерить один вариант. Берётся лучшее время: помехи от других
// задач на карте всегда добавляют время, но никогда не убавляют.
constexpr int ATTEMPTS = 2;

// Запуск ядра repeats раз подряд. Возвращает время в секундах на один запуск
// или -1, если сетка не запустилась (обычно не хватает регистров под блок).
double timeChunk(const GridTuneTask& task, const LaunchGrid& grid,
                 int repeats, cudaStream_t stream)
{
    cudaEvent_t begin = nullptr;
    cudaEvent_t end   = nullptr;
    if (cudaEventCreate(&begin) != cudaSuccess)
        return -1.0;
    if (cudaEventCreate(&end) != cudaSuccess) {
        cudaEventDestroy(begin);
        return -1.0;
    }

    const uint64_t chunk = std::min(task.chunkSize, task.availableMasks);

    // Замеры размазываются по всему диапазону масок, а не повторяются на одном
    // месте. Стоимость чанка от места зависит: у кода Грея номер маски — это
    // она сама, и стартовый XOR нити стоит столько строк, сколько единиц в
    // маске. В начале диапазона их почти нет, а ровно на степени двойки —
    // всего пара, и замер там показывает, что лишние нити ничего не стоят.
    // Настоящий расчёт проходит все места подряд, поэтому и замер обязан.
    const uint64_t span   = task.availableMasks - chunk;
    const uint64_t stride = repeats > 1 ? span / uint64_t(repeats - 1) : 0;

    double seconds = -1.0;
    try {
        cudaEventRecord(begin, stream);

        for (int i = 0; i < repeats; ++i) {
            const uint64_t offset = stride * uint64_t(i);

            if (task.gray) {
                launchSpectrumKernelGrayShort(
                    grid.blocks, grid.threads, stream,
                    task.scratchSpectrum,
                    task.numOfCols, task.numOfRows, task.wordsPerRow,
                    offset, chunk);
            }
            else {
                launchSpectrumKernelShort(
                    task.scratchSpectrum, task.binomTable,
                    grid.blocks, grid.threads, stream,
                    task.numOfCols, task.numOfRows, task.wordsPerRow,
                    offset, chunk, task.numOfOnes);
            }
        }

        cudaEventRecord(end, stream);
        if (cudaEventSynchronize(end) == cudaSuccess) {
            float ms = 0.0f;
            if (cudaEventElapsedTime(&ms, begin, end) == cudaSuccess)
                seconds = double(ms) / 1000.0 / double(repeats);
        }
    }
    catch (...) {
        // Обёртка запуска бросает, если сетка не подошла. Это не ошибка
        // расчёта — просто вариант выбывает из перебора.
        cudaGetLastError();
        seconds = -1.0;
    }

    cudaEventDestroy(begin);
    cudaEventDestroy(end);
    return seconds;
}

// Лучшее из нескольких измерений; -1, если вариант не запускается.
double timeGrid(const GridTuneTask& task, const LaunchGrid& grid,
                int repeats, cudaStream_t stream)
{
    double best = -1.0;
    for (int attempt = 0; attempt < ATTEMPTS; ++attempt) {
        const double seconds = timeChunk(task, grid, repeats, stream);
        if (seconds < 0.0)
            return -1.0;
        if (best < 0.0 || seconds < best)
            best = seconds;
    }
    return best;
}

} // namespace

LaunchGrid tuneLaunchGrid(const GridTuneTask& task, cudaStream_t stream)
{
    LaunchGrid none;

    if (!task.scratchSpectrum || task.availableMasks == 0 || task.chunkSize == 0)
        return none;

    cudaDeviceProp props{};
    int device = 0;
    if (cudaGetDevice(&device) != cudaSuccess)
        return none;
    if (cudaGetDeviceProperties(&props, device) != cudaSuccess)
        return none;

    std::vector<int> blockCandidates;
    for (int perSm : BLOCKS_PER_SM)
        blockCandidates.push_back(perSm * props.multiProcessorCount);

    std::vector<int> threadCandidates;
    for (int threads : THREADS) {
        if (threads <= props.maxThreadsPerBlock)
            threadCandidates.push_back(threads);
    }
    if (blockCandidates.empty() || threadCandidates.empty())
        return none;

    // Отправная точка — середина обоих списков: с неё оценивается объём
    // задачи и от неё пляшет первый проход.
    LaunchGrid probe;
    probe.blocks  = blockCandidates[blockCandidates.size() / 2];
    probe.threads = threadCandidates[threadCandidates.size() / 2];

    const auto startedAt = std::chrono::steady_clock::now();

    // Первый запуск после старта всегда дороже остальных: подгружается
    // модуль, прогревается карта. Меряем вхолостую.
    timeChunk(task, probe, 1, stream);

    const double chunkSec = timeChunk(task, probe, 1, stream);
    if (chunkSec <= 0.0)
        return none;

    // Оценка всего расчёта по одному чанку. Она грубая — стоимость маски
    // зависит от числа складываемых строк, — но чтобы отличить задачу на
    // секунду от задачи на час, точности хватает с запасом.
    const double estimatedSec = chunkSec * double(task.totalMasks)
                                         / double(std::min(task.chunkSize, task.availableMasks));
    if (estimatedSec < task.minWorthSeconds)
        return none;

    int repeats = int(TARGET_MEASURE_SEC / chunkSec);
    repeats = std::max(1, std::min(repeats, MAX_REPEATS));

    LaunchGrid best;
    double bestTime = -1.0;

    if (task.verbose) {
        std::printf("подбор: чанк %llu масок, %.3f мс на чанк, %d повторов на замер\n",
                    (unsigned long long)std::min(task.chunkSize, task.availableMasks),
                    chunkSec * 1000.0, repeats);
    }

    // Настройки пользователя — первый кандидат и заодно нижняя планка.
    if (task.userGrid.isValid()) {
        const double seconds = timeGrid(task, task.userGrid, repeats, stream);
        if (task.verbose)
            std::printf("  %4d x %4d : %.3f мс  (из настроек)\n",
                        task.userGrid.blocks, task.userGrid.threads, seconds * 1000.0);
        if (seconds > 0.0) {
            bestTime = seconds;
            best     = task.userGrid;
        }
    }

    // Проходы по очереди: сначала блоки при фиксированных нитях, потом нити
    // при найденных блоках, потом блоки ещё раз. Полную решётку не перебираем
    // — это дороже, а по замерам --sweep такой обход подходит к настоящему
    // оптимуму на 10-25 %, чего с лихвой хватает против 24 % отставания
    // умолчания.
    auto sweepBlocks = [&](int threads) {
        for (int blocks : blockCandidates) {
            const LaunchGrid grid{ blocks, threads };
            const double seconds = timeGrid(task, grid, repeats, stream);
            if (task.verbose)
                std::printf("  %4d x %4d : %s\n", blocks, threads,
                            seconds < 0.0 ? "не запустилось"
                                          : (std::to_string(seconds * 1000.0) + " мс").c_str());
            if (seconds < 0.0)
                continue;
            if (bestTime < 0.0 || seconds < bestTime) {
                bestTime = seconds;
                best     = grid;
            }
        }
    };
    auto sweepThreads = [&](int blocks) {
        for (int threads : threadCandidates) {
            const LaunchGrid grid{ blocks, threads };
            const double seconds = timeGrid(task, grid, repeats, stream);
            if (task.verbose)
                std::printf("  %4d x %4d : %s\n", blocks, threads,
                            seconds < 0.0 ? "не запустилось"
                                          : (std::to_string(seconds * 1000.0) + " мс").c_str());
            if (seconds < 0.0)
                continue;
            if (bestTime < 0.0 || seconds < bestTime) {
                bestTime = seconds;
                best     = grid;
            }
        }
    };

    sweepBlocks(probe.threads);
    if (!best.isValid())
        return none;

    sweepThreads(best.blocks);
    sweepBlocks(best.threads);

    if (task.verbose) {
        const double spent = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - startedAt).count();
        std::printf("подбор занял %.3f с, выбрано %d x %d\n", spent, best.blocks, best.threads);
    }

    return best;
}
