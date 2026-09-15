#include "win32/WinRtSupport.h"

#include "win32/ComApartment.h"
#include "win32/Win32Error.h"

// C++/WinRT 头文件随 Windows SDK 提供（Include\10.0.x\cppwinrt\winrt），不是第三方库。
// base.h 必须最先包含。
// 注意：本工程已启用 QT_NO_KEYWORDS，因此 winrt 头里出现的 signals / slots
// 不会与 Qt 的关键字宏冲突 —— 这正是 P0-6 的意义所在。
#include <winrt/base.h>

#include <roapi.h>

#include <exception>

namespace WinEase::Win32 {

bool ensureWinRtReady(QString *errorOut)
{
    const ApartmentState state = currentApartmentState();

    if (state == ApartmentState::SingleThreaded || state == ApartmentState::MultiThreaded) {
        return true;
    }

    if (errorOut != nullptr) {
        *errorOut = (state == ApartmentState::NotInitialized)
                        ? QStringLiteral("当前线程尚未初始化 COM，无法调用 Windows 运行时 API"
                                         "（请在 main() 中调用 ComApartment::initialize）")
                        : QStringLiteral("无法确定当前线程的 COM 套间状态，Windows 运行时不可用");
    }
    return false;
}

bool probeWinRtClass(const wchar_t *runtimeClassName, QString *errorOut)
{
    if (runtimeClassName == nullptr || runtimeClassName[0] == L'\0') {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("运行时类名为空");
        }
        return false;
    }

    if (!ensureWinRtReady(errorOut)) {
        return false;
    }

    const QString displayName = QString::fromWCharArray(runtimeClassName);

    try {
        const winrt::hstring className{ runtimeClassName };

        // 请求 IID_IUnknown：静态类（如 PdfDocument）的激活工厂实现的是自己的
        // 静态接口（IPdfDocumentStatics）而**不是** IActivationFactory，
        // 因此请求 IActivationFactory 会对可用类型误报 E_NOINTERFACE。
        const HSTRING classHandle = reinterpret_cast<HSTRING>(winrt::get_abi(className));
        void *factory = nullptr;
        const HRESULT hr = ::RoGetActivationFactory(classHandle,
                                                    winrt::guid_of<::IUnknown>(),
                                                    &factory);
        if (factory != nullptr) {
            // 成功时返回的是引用计数为 1 的接口指针，必须释放
            static_cast<::IUnknown *>(factory)->Release();
            factory = nullptr;
        }

        if (SUCCEEDED(hr)) {
            return true;
        }

        if (errorOut != nullptr) {
            *errorOut = describeHresultFailure(QStringLiteral("探测 Windows 运行时类「%1」").arg(displayName),
                                               hr);
        }
        return false;
    } catch (const winrt::hresult_error &error) {
        if (errorOut != nullptr) {
            *errorOut = describeWinRtFailure(QStringLiteral("探测 Windows 运行时类「%1」").arg(displayName),
                                             static_cast<long>(error.code().value));
        }
        return false;
    } catch (...) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("探测 Windows 运行时类「%1」时发生未知异常").arg(displayName);
        }
        return false;
    }
}

bool runWinRt(const QString &action, QString *errorOut, const std::function<void()> &body)
{
    if (!ensureWinRtReady(errorOut)) {
        return false;
    }

    try {
        body();
        return true;
    } catch (const winrt::hresult_error &error) {
        if (errorOut != nullptr) {
            // 只用 HRESULT 组装信息，避免依赖尚待验证的 hstring → QString 转换
            *errorOut = describeWinRtFailure(action, static_cast<long>(error.code().value));
        }
        return false;
    } catch (const std::exception &error) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("%1 失败：%2").arg(action, QString::fromUtf8(error.what()));
        }
        return false;
    } catch (...) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("%1 失败：发生未知异常").arg(action);
        }
        return false;
    }
}

QString describeWinRtFailure(const QString &action, long hresult)
{
    return describeHresultFailure(action, static_cast<HRESULT>(hresult));
}

} // namespace WinEase::Win32
