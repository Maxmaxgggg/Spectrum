#include "gpuinfo.h"

#include <cuda_runtime.h>

GpuInfo queryGpu()
{
    GpuInfo info;
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess || count <= 0)
        return info;
    cudaDeviceProp prop{};
    if (cudaGetDeviceProperties(&prop, 0) != cudaSuccess)
        return info;
    info.available          = true;
    info.multiprocessors    = prop.multiProcessorCount;
    info.maxThreadsPerBlock = prop.maxThreadsPerBlock;
    return info;
}
