#include "taskbarprogress.h"

#include <QWidget>

#ifdef Q_OS_WIN
    #include <windows.h>
    #include <shobjidl.h>
#endif

#ifdef Q_OS_WIN
namespace {
inline ITaskbarList3* asTaskbar(void* p) { return static_cast<ITaskbarList3*>(p); }
} // namespace
#endif

TaskbarProgress::TaskbarProgress(QWidget* window)
    : m_window(window)
{
#ifdef Q_OS_WIN
    ITaskbarList3* list = nullptr;
    const HRESULT hr = CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_ALL,
                                        IID_ITaskbarList3, reinterpret_cast<void**>(&list));
    if (FAILED(hr) || !list)
        return;

    // HrInit обязателен: без него методы вернут ошибку.
    if (FAILED(list->HrInit())) {
        list->Release();
        return;
    }
    m_taskbar = list;
#else
    Q_UNUSED(window);
#endif
}

TaskbarProgress::~TaskbarProgress()
{
#ifdef Q_OS_WIN
    if (m_taskbar) {
        asTaskbar(m_taskbar)->Release();
        m_taskbar = nullptr;
    }
#endif
}

bool TaskbarProgress::isAvailable() const
{
    return m_taskbar != nullptr;
}

void TaskbarProgress::setPercent(int percent)
{
#ifdef Q_OS_WIN
    if (!m_taskbar || !m_window)
        return;

    const HWND hwnd = reinterpret_cast<HWND>(m_window->winId());
    if (percent <= 0 || percent >= 100) {
        asTaskbar(m_taskbar)->SetProgressState(hwnd, TBPF_NOPROGRESS);
    }
    else {
        asTaskbar(m_taskbar)->SetProgressState(hwnd, TBPF_NORMAL);
        asTaskbar(m_taskbar)->SetProgressValue(hwnd, percent, 100);
    }
#else
    Q_UNUSED(percent);
#endif
}

void TaskbarProgress::clear()
{
    setPercent(0);
}
