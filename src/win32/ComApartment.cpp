#include "win32/ComApartment.h"

#include "win32/Win32Error.h"

#include <objbase.h>

#include <utility>

namespace WinEase::Win32 {

// ---------------------------------------------------------------------------
//  ComApartment
// ---------------------------------------------------------------------------

ComApartment::ComApartment(ComApartment &&other) noexcept
    : m_result(other.m_result)
    , m_ownsInitialization(other.m_ownsInitialization)
{
    other.m_result = E_PENDING;
    other.m_ownsInitialization = false;
}

ComApartment &ComApartment::operator=(ComApartment &&other) noexcept
{
    if (this != &other) {
        // 先释放自身已持有的计数，避免泄漏
        if (m_ownsInitialization) {
            ::CoUninitialize();
        }
        m_result = other.m_result;
        m_ownsInitialization = other.m_ownsInitialization;
        other.m_result = E_PENDING;
        other.m_ownsInitialization = false;
    }
    return *this;
}

ComApartment::~ComApartment()
{
    if (m_ownsInitialization) {
        ::CoUninitialize();
        m_ownsInitialization = false;
    }
}

ComApartment ComApartment::initialize(ApartmentModel model)
{
    ComApartment guard;

    const DWORD flags = (model == ApartmentModel::SingleThreaded)
                            ? COINIT_APARTMENTTHREADED
                            : COINIT_MULTITHREADED;

    // 禁用 OLE1 兼容与 DDE：本项目不依赖它们，禁用可避免多余的消息处理开销
    const DWORD extra = COINIT_DISABLE_OLE1DDE;

    const HRESULT hr = ::CoInitializeEx(nullptr, flags | extra);
    guard.m_result = hr;

    // S_OK 与 S_FALSE 都表示"当前线程已可用于 COM"。
    // 两者都会递增内部计数，因此都必须成对调用 CoUninitialize 以保持平衡。
    guard.m_ownsInitialization = (hr == S_OK || hr == S_FALSE);
    return guard;
}

bool ComApartment::succeeded() const
{
    return m_result == S_OK || m_result == S_FALSE;
}

bool ComApartment::alreadyInitialized() const
{
    return m_result == S_FALSE;
}

QString ComApartment::message() const
{
    if (m_result == E_PENDING) {
        return QStringLiteral("尚未初始化 COM 套间");
    }
    if (m_result == S_OK) {
        return QStringLiteral("COM 套间初始化完成");
    }
    if (m_result == S_FALSE) {
        return QStringLiteral("当前线程已初始化过 COM 套间（沿用既有设置）");
    }
    if (m_result == RPC_E_CHANGED_MODE) {
        return QStringLiteral("COM 套间冲突：当前线程已被初始化为另一种套间模型，"
                              "依赖 STA 的功能将不可用");
    }
    return describeHresultFailure(QStringLiteral("初始化 COM 套间"), m_result);
}

// ---------------------------------------------------------------------------
//  状态探测
// ---------------------------------------------------------------------------

ApartmentState currentApartmentState()
{
    APTTYPE type = APTTYPE_CURRENT;
    APTTYPEQUALIFIER qualifier = APTTYPEQUALIFIER_NONE;

    const HRESULT hr = ::CoGetApartmentType(&type, &qualifier);
    if (hr == CO_E_NOTINITIALIZED) {
        return ApartmentState::NotInitialized;
    }
    if (FAILED(hr)) {
        return ApartmentState::Unknown;
    }

    switch (type) {
    case APTTYPE_STA:
    case APTTYPE_MAINSTA:
        return ApartmentState::SingleThreaded;
    case APTTYPE_MTA:
        return ApartmentState::MultiThreaded;
    case APTTYPE_NA:
    case APTTYPE_CURRENT:
        // APTTYPE_NA：中性套间，既可作为 STA 也可作为 MTA 使用；
        // 对 WinRT/COM 调用而言是可用状态，按 STA 归类以便上层判断"可用"。
        return ApartmentState::SingleThreaded;
    default:
        return ApartmentState::Unknown;
    }
}

bool isThreadComReady()
{
    const ApartmentState state = currentApartmentState();
    return state == ApartmentState::SingleThreaded || state == ApartmentState::MultiThreaded;
}

QString apartmentStateText(ApartmentState state)
{
    switch (state) {
    case ApartmentState::NotInitialized:
        return QStringLiteral("未初始化");
    case ApartmentState::SingleThreaded:
        return QStringLiteral("单线程套间 STA");
    case ApartmentState::MultiThreaded:
        return QStringLiteral("多线程套间 MTA");
    case ApartmentState::Unknown:
        break;
    }
    return QStringLiteral("未知");
}

} // namespace WinEase::Win32
