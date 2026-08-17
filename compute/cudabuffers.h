#pragma once

#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <utility>

#include <cuda_runtime.h>
#include <QString>

// Владеющие обёртки над буферами и объектами CUDA.
//
// Раньше всё это освобождалось вручную, причём двумя почти одинаковыми
// блоками в конце computeSpectrum — для успешного завершения и для отмены.
// Блоки успели разойтись: на пути отмены не освобождались d_spectrum и
// d_binomTable. Теперь освобождение привязано к времени жизни объекта и не
// зависит от того, каким путём мы вышли из функции.

// Ошибка CUDA. Раньше CUDA_CALL звал std::abort() — приложение молча умирало,
// не показав пользователю ничего.
class CudaError : public std::runtime_error
{
public:
    CudaError(cudaError_t code, const char* file, int line)
        : std::runtime_error(cudaGetErrorString(code))
        , m_code(code), m_file(file), m_line(line) {}

    cudaError_t code() const { return m_code; }

    QString message() const
    {
        return QStringLiteral("Ошибка CUDA: %1 (%2:%3)")
            .arg(QString::fromLatin1(cudaGetErrorString(m_code)))
            .arg(QString::fromLatin1(m_file))
            .arg(m_line);
    }

private:
    cudaError_t m_code;
    const char* m_file;
    int         m_line;
};

#ifdef CUDA_CALL
    #undef CUDA_CALL
#endif
#define CUDA_CALL(call)                                        \
    do {                                                       \
        cudaError_t err_ = (call);                             \
        if (err_ != cudaSuccess)                               \
            throw CudaError(err_, __FILE__, __LINE__);         \
    } while (0)

// Память на видеокарте.
template <typename T>
class DeviceBuffer
{
public:
    DeviceBuffer() = default;
    ~DeviceBuffer() { reset(); }

    DeviceBuffer(const DeviceBuffer&)            = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;

    DeviceBuffer(DeviceBuffer&& o) noexcept : m_ptr(o.m_ptr), m_count(o.m_count)
    {
        o.m_ptr = nullptr; o.m_count = 0;
    }
    DeviceBuffer& operator=(DeviceBuffer&& o) noexcept
    {
        if (this != &o) {
            reset();
            m_ptr = o.m_ptr; m_count = o.m_count;
            o.m_ptr = nullptr; o.m_count = 0;
        }
        return *this;
    }

    void allocate(size_t count)
    {
        reset();
        CUDA_CALL(cudaMalloc(reinterpret_cast<void**>(&m_ptr), count * sizeof(T)));
        m_count = count;
    }

    void fillZero()
    {
        if (m_ptr) CUDA_CALL(cudaMemset(m_ptr, 0, m_count * sizeof(T)));
    }

    void reset()
    {
        if (m_ptr) { cudaFree(m_ptr); m_ptr = nullptr; m_count = 0; }
    }

    T*       get()   const { return m_ptr; }
    size_t   count() const { return m_count; }
    size_t   bytes() const { return m_count * sizeof(T); }
    explicit operator bool() const { return m_ptr != nullptr; }

private:
    T*     m_ptr   = nullptr;
    size_t m_count = 0;
};

// Память на хосте. Для GPU-расчёта нужна pinned (иначе не работает
// асинхронное копирование), для CPU-расчёта — обычная: cudaMallocHost
// требует контекста CUDA, которого без видеокарты нет.
template <typename T>
class HostBuffer
{
public:
    enum class Kind { Paged, Pinned };

    HostBuffer() = default;
    ~HostBuffer() { reset(); }

    HostBuffer(const HostBuffer&)            = delete;
    HostBuffer& operator=(const HostBuffer&) = delete;

    HostBuffer(HostBuffer&& o) noexcept
        : m_ptr(o.m_ptr), m_count(o.m_count), m_kind(o.m_kind)
    {
        o.m_ptr = nullptr; o.m_count = 0;
    }
    HostBuffer& operator=(HostBuffer&& o) noexcept
    {
        if (this != &o) {
            reset();
            m_ptr = o.m_ptr; m_count = o.m_count; m_kind = o.m_kind;
            o.m_ptr = nullptr; o.m_count = 0;
        }
        return *this;
    }

    void allocate(size_t count, Kind kind)
    {
        reset();
        m_kind = kind;
        if (kind == Kind::Pinned) {
            CUDA_CALL(cudaMallocHost(reinterpret_cast<void**>(&m_ptr), count * sizeof(T)));
        } else {
            m_ptr = static_cast<T*>(std::calloc(count, sizeof(T)));
            if (!m_ptr) throw std::bad_alloc();
        }
        m_count = count;
    }

    void fillZero()
    {
        if (m_ptr) std::memset(m_ptr, 0, m_count * sizeof(T));
    }

    void reset()
    {
        if (!m_ptr) return;
        if (m_kind == Kind::Pinned) cudaFreeHost(m_ptr);
        else                        std::free(m_ptr);
        m_ptr = nullptr; m_count = 0;
    }

    T*       get()   const { return m_ptr; }
    size_t   count() const { return m_count; }
    size_t   bytes() const { return m_count * sizeof(T); }
    T&       operator[](size_t i) const { return m_ptr[i]; }
    explicit operator bool() const { return m_ptr != nullptr; }

private:
    T*     m_ptr   = nullptr;
    size_t m_count = 0;
    Kind   m_kind  = Kind::Paged;
};

class CudaStream
{
public:
    CudaStream() = default;
    ~CudaStream() { reset(); }

    CudaStream(const CudaStream&)            = delete;
    CudaStream& operator=(const CudaStream&) = delete;

    void create() { reset(); CUDA_CALL(cudaStreamCreate(&m_stream)); }
    void reset()  { if (m_stream) { cudaStreamDestroy(m_stream); m_stream = nullptr; } }

    cudaStream_t get() const { return m_stream; }
    explicit operator bool() const { return m_stream != nullptr; }

private:
    cudaStream_t m_stream = nullptr;
};

class CudaEvent
{
public:
    CudaEvent() = default;
    ~CudaEvent() { reset(); }

    CudaEvent(const CudaEvent&)            = delete;
    CudaEvent& operator=(const CudaEvent&) = delete;

    void create() { reset(); CUDA_CALL(cudaEventCreate(&m_event)); }
    void reset()  { if (m_event) { cudaEventDestroy(m_event); m_event = nullptr; } }

    cudaEvent_t get() const { return m_event; }
    explicit operator bool() const { return m_event != nullptr; }

private:
    cudaEvent_t m_event = nullptr;
};
