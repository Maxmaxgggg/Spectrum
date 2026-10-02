#pragma once

// Видеокарта глазами интерфейса: есть ли она и каковы её пределы.
//
// Заголовок без CUDA: диалог настроек раньше подключал cuda_runtime.h ради
// двух вызовов, и вместе с ним заголовки CUDA тянул весь интерфейс.
struct GpuInfo
{
    bool available          = false;
    int  multiprocessors    = 0;
    int  maxThreadsPerBlock = 0;
};

// Первая видеокарта. available = false, если её нет или драйвер не отвечает.
GpuInfo queryGpu();
