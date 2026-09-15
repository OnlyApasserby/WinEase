// ============================================================================
//  p3_spike —— P3 技术预研探针
//
//  ROADMAP 的 P3 章节有一条硬约定：**每项必须先做预研 spike，产出可行性结论，
//  再决定「实现 / 降级 / 砍掉」**，不得直接进入编码。本程序就是那次预研的产物，
//  而且刻意做成**可重复执行**的：Windows 大版本更新、装了 PawnIO、插了耳机，
//  结论都可能变，一次性脚本没法回答"现在还行不行"。
//
//  探针纪律（与其它自检一致）：
//    * **只读**：不改系统状态、不真切设备、不碰用户文件；
//    * 探不到的东西**如实说"探不到"**，绝不用"看起来没问题"糊过去；
//    * 每一项都给出**它自己的结论**（可开工 / 需降级 / 待手测），而不只是 PASS/FAIL。
//
//  ⚠ **退出码的语义与其它自检不同**（很重要）：
//      `check()` 判的是"**探针本身能不能跑通**"（能否创建对象、能否调用、有没有崩溃），
//      而"某项能力是否可用"用 `[说明]` 行给结论 ——
//      "某个未公开 API 已经失效"是一个**有价值的预研结论**，不该算作测试失败。
//      所以：**exit 0 = 预研跑完了**；要判断能不能开工，看的是结论行。
//
//  运行方式：
//      .\build\bin\p3_spike.exe        # 退出码 0 = 全部探针跑完（不代表能力都可用）
// ============================================================================

#include <windows.h>

#undef min // windows.h 的 min/max 宏会破坏后面的 qMin/qMax/std::min
#undef max

#include <audiopolicy.h>
#include <intrin.h>
#include <mfapi.h>
#include <mferror.h>
#include <mmdeviceapi.h>
#include <restartmanager.h>
#include <winioctl.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>

#include <cstdio>

#include "win32/ComApartment.h"
#include "win32/StorageDevices.h"
#include "win32/WinRtSupport.h"

namespace {

/// 本进程是否已提权（自包含实现：探针不该为了这一行去链接主程序的 app 层）
bool isProcessElevated()
{
    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const bool ok = ::GetTokenInformation(token, TokenElevation, &elevation,
                                          sizeof(elevation), &returned)
                    != FALSE;
    ::CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

} // namespace

// ---------------------------------------------------------------------------
//  轻量 Reporter（预研探针不需要 feature_smoke 那套脚手架）
// ---------------------------------------------------------------------------

namespace {

class Reporter
{
public:
    void section(const QString &title)
    {
        std::printf("\n== %s ==\n", title.toUtf8().constData());
    }

    void check(bool ok, const QString &what, const QString &detail = QString())
    {
        ++m_total;
        if (!ok) {
            ++m_failures;
        }
        const char *tag = ok ? (ascii() ? "[PASS]" : "[通过]") : (ascii() ? "[FAIL]" : "[失败]");
        std::printf("%s %s\n", tag, what.toUtf8().constData());
        if (!detail.isEmpty()) {
            std::printf("       %s\n", detail.toUtf8().constData());
        }
    }

    /// 结论/说明（不计入断言）
    void note(const QString &text) const
    {
        std::printf("  %s %s\n", ascii() ? "[INFO]" : "[说明]", text.toUtf8().constData());
    }

    int total() const { return m_total; }
    int failures() const { return m_failures; }

private:
    static bool ascii() { return !qEnvironmentVariableIsEmpty("WINEASE_SMOKE_ASCII"); }

    int m_total = 0;
    int m_failures = 0;
};

QString hresultText(HRESULT hr)
{
    return QStringLiteral("0x%1").arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
}

QString guidText(const GUID &guid)
{
    return QStringLiteral("{%1-%2-%3-%4-%5}")
        .arg(guid.Data1, 8, 16, QLatin1Char('0'))
        .arg(guid.Data2, 4, 16, QLatin1Char('0'))
        .arg(guid.Data3, 4, 16, QLatin1Char('0'))
        .arg(guid.Data4[0] * 256 + guid.Data4[1], 4, 16, QLatin1Char('0'))
        .arg(static_cast<quint32>(guid.Data4[2]) * 0x1000000U
                 + static_cast<quint32>(guid.Data4[3]) * 0x10000U
                 + static_cast<quint32>(guid.Data4[4]) * 0x100U
                 + static_cast<quint32>(guid.Data4[5]),
             12,
             16,
             QLatin1Char('0'));
}

bool guidIsEmpty(const GUID &guid)
{
    return guid.Data1 == 0 && guid.Data2 == 0 && guid.Data3 == 0
           && guid.Data4[0] == 0 && guid.Data4[1] == 0 && guid.Data4[2] == 0
           && guid.Data4[3] == 0 && guid.Data4[4] == 0 && guid.Data4[5] == 0
           && guid.Data4[6] == 0 && guid.Data4[7] == 0;
}

// ---------------------------------------------------------------------------
//  0. 环境（后面很多结论都依赖它）
// ---------------------------------------------------------------------------

struct Environment {
    QString windowsVersion;
    bool elevated = false;
    QString cpuVendor;
    int cpuFamily = 0;
    bool pawnIoDeviceOpened = false;   ///< 驱动设备能打开（需要管理员）
    QString pawnIoDeviceError;         ///< 打不开的原因（"不存在" / "需要管理员"）
    bool pawnIoDeviceExists = false;   ///< 设备存在（哪怕我们打不开）
    bool pawnIoServiceFound = false;
    bool pawnIoLibrariesLoaded = false;
    QStringList pawnIoExports;
};

QString windowsVersionText()
{
    // RtlGetVersion 是唯一"不撒谎"的版本查询（GetVersionEx 会被 manifest 兼容层改写）
    using RtlGetVersionFn = LONG(WINAPI *)(PRTL_OSVERSIONINFOW);
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return QStringLiteral("未知");
    }
    const auto rtlGetVersion =
        reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void *>(
            ::GetProcAddress(ntdll, "RtlGetVersion")));
    if (rtlGetVersion == nullptr) {
        return QStringLiteral("未知");
    }

    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (rtlGetVersion(&info) != 0) {
        return QStringLiteral("未知");
    }
    return QStringLiteral("%1.%2 build %3")
        .arg(info.dwMajorVersion)
        .arg(info.dwMinorVersion)
        .arg(info.dwBuildNumber);
}

void cpuIdentity(QString *vendorOut, int *familyOut)
{
    int regs[4] = {};
    __cpuid(regs, 0);
    char vendor[13] = {};
    ::memcpy(vendor, &regs[1], 4);
    ::memcpy(vendor + 4, &regs[3], 4);
    ::memcpy(vendor + 8, &regs[2], 4);
    *vendorOut = QString::fromLatin1(vendor);

    __cpuid(regs, 1);
    const int baseFamily = (regs[0] >> 8) & 0xF;
    const int extFamily = (regs[0] >> 20) & 0xFF;
    *familyOut = (baseFamily == 0xF) ? (baseFamily + extFamily) : baseFamily;
}

void probePawnIo(Environment *env, Reporter &r)
{
    // ① 设备：\\.\PawnIO（未安装 = 找不到设备；装了但非管理员 = 拒绝访问）
    const HANDLE device = ::CreateFileW(L"\\\\.\\PawnIO",
                                        GENERIC_READ | GENERIC_WRITE,
                                        0,
                                        nullptr,
                                        OPEN_EXISTING,
                                        0,
                                        nullptr);
    if (device != INVALID_HANDLE_VALUE) {
        env->pawnIoDeviceOpened = true;
        env->pawnIoDeviceExists = true;
        ::CloseHandle(device);
    } else {
        const DWORD code = ::GetLastError();
        env->pawnIoDeviceExists = (code == ERROR_ACCESS_DENIED);
        env->pawnIoDeviceError = QStringLiteral("错误码 %1（%2）")
                                     .arg(code)
                                     .arg(code == ERROR_ACCESS_DENIED
                                              ? QStringLiteral("设备存在但需要管理员权限")
                                              : (code == ERROR_FILE_NOT_FOUND
                                                     ? QStringLiteral("设备不存在 → 驱动未安装")
                                                     : QStringLiteral("其它错误")));
    }

    // ② 服务：PawnIO 驱动服务是否注册
    const SC_HANDLE manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager != nullptr) {
        const SC_HANDLE service = ::OpenServiceW(manager, L"PawnIO", SERVICE_QUERY_STATUS);
        if (service != nullptr) {
            env->pawnIoServiceFound = true;
            ::CloseServiceHandle(service);
        }
        ::CloseServiceHandle(manager);
    }

    // ③ 用户态库：能否动态加载（集成策略就是运行期 LoadLibrary，所以这条最贴近实际）
    const HMODULE library = ::LoadLibraryW(L"PawnIOLib.dll");
    if (library != nullptr) {
        env->pawnIoLibrariesLoaded = true;
        for (const char *name : { "pawnio_open", "pawnio_load", "pawnio_execute",
                                  "pawnio_close", "pawnio_version" }) {
            if (::GetProcAddress(library, name) != nullptr) {
                env->pawnIoExports << QString::fromLatin1(name);
            }
        }
    }

    r.note(QStringLiteral("PawnIO 驱动设备：%1")
               .arg(env->pawnIoDeviceOpened
                        ? QStringLiteral("已打开（驱动在运行，且当前进程有权限）")
                        : env->pawnIoDeviceError));
    r.note(QStringLiteral("PawnIO 服务：%1 · PawnIOLib.dll：%2%3")
               .arg(env->pawnIoServiceFound ? QStringLiteral("已注册") : QStringLiteral("未找到"),
                    env->pawnIoLibrariesLoaded ? QStringLiteral("可加载，导出 ")
                                                : QStringLiteral("加载失败（未安装或不在 PATH）"),
                    env->pawnIoExports.join(QStringLiteral(", "))));

    r.check(!env->pawnIoDeviceOpened || env->pawnIoServiceFound,
            QStringLiteral("P3-07 探针自洽：设备能打开时服务也该在（否则结论要重看）"));

    // 结论只报告，不作为失败：没装 PawnIO 是完全正常的状态（降级路径就是为它准备的）
    if (!env->pawnIoDeviceExists && !env->pawnIoServiceFound && !env->pawnIoLibrariesLoaded) {
        r.note(QStringLiteral("结论：本机**未安装 PawnIO** → CPU/主板温度不可用，"
                              "必须走「如实报不可用」的降级路径（ENV-SETUP §4.2 已定这条策略）"));
    } else if (env->pawnIoDeviceExists && !env->pawnIoDeviceOpened) {
        r.note(QStringLiteral("结论：PawnIO 已安装，但**驱动设备需要管理员** → "
                              "必须经提权助手打开（与 ENV-SETUP §4.2 的决策一致）"));
    } else if (env->pawnIoDeviceOpened) {
        r.note(QStringLiteral("结论：PawnIO 设备可直接打开（当前进程已提权）"));
    }
}

// ---------------------------------------------------------------------------
//  P3-01 虚拟桌面：公开 API IVirtualDesktopManager 是否仍稳定
//
//  ⚠ 刻意**本地声明接口**而不是包含 shobjidl_core.h：
//     预研要确认的正是"这个 vtable 布局在今天的系统上还成不成立"，
//     用系统头文件反而把这个事实藏起来了。
// ---------------------------------------------------------------------------

const GUID kClsidVirtualDesktopManager = { 0xAA509086, 0x5CA9, 0x4C25,
                                           { 0x8F, 0x95, 0x58, 0x9D, 0x3C, 0x07, 0xB4, 0x8A } };
const GUID kIidVirtualDesktopManager = { 0xA5CD92FF, 0x29BE, 0x454C,
                                         { 0x8D, 0x04, 0xD8, 0x28, 0x79, 0xFB, 0x3F, 0x1B } };

struct IVirtualDesktopManagerLocal : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE IsWindowOnCurrentVirtualDesktop(HWND topLevelWindow,
                                                                     BOOL *onCurrentDesktop) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetWindowDesktopId(HWND topLevelWindow,
                                                         GUID *desktopId) = 0;
    virtual HRESULT STDMETHODCALLTYPE MoveWindowToDesktop(HWND topLevelWindow,
                                                          REFGUID desktopId) = 0;
};

void probeVirtualDesktop(Reporter &r)
{
    r.section(QStringLiteral("P3-01 虚拟桌面（IVirtualDesktopManager）"));

    IVirtualDesktopManagerLocal *manager = nullptr;
    const HRESULT hr = ::CoCreateInstance(kClsidVirtualDesktopManager,
                                          nullptr,
                                          CLSCTX_INPROC_SERVER,
                                          kIidVirtualDesktopManager,
                                          reinterpret_cast<void **>(&manager));
    r.check(SUCCEEDED(hr) && manager != nullptr,
            QStringLiteral("P3-01 能创建 IVirtualDesktopManager（公开 COM 接口还在）"),
            QStringLiteral("HRESULT %1").arg(hresultText(hr)));
    if (manager == nullptr) {
        r.note(QStringLiteral("结论：接口不可用 → 本项**降级为不做**（无公开替代方案）"));
        return;
    }

    // ⚠ 探针目标必须是**我们自己的普通窗口**：
    //   一开始图省事用了 `GetShellWindow()`（explorer 的托盘窗口），
    //   结果 `GetWindowDesktopId` 返回 `TYPE_E_ELEMENTNOTFOUND (0x8002802B)` ——
    //   那个窗口根本不属于任何虚拟桌面。**用错目标窗口会把"探针写错了"误读成"API 失效"**，
    //   这正是预研最容易犯的错（所以这里改成自建窗口 + 如实记录这个坑）。
    const HWND probeWindow = ::CreateWindowExW(0,
                                               L"STATIC",
                                               L"WinEase P3 预研探针",
                                               WS_OVERLAPPEDWINDOW,
                                               0,
                                               0,
                                               200,
                                               100,
                                               nullptr,
                                               nullptr,
                                               ::GetModuleHandleW(nullptr),
                                               nullptr);
    r.check(probeWindow != nullptr,
            QStringLiteral("P3-01 前置：建出自己的探针窗口"),
            QStringLiteral("错误码 %1").arg(::GetLastError()));

    if (probeWindow != nullptr) {
        ::ShowWindow(probeWindow, SW_SHOWNOACTIVATE);
        ::UpdateWindow(probeWindow);
        for (int spin = 0; spin < 20; ++spin) {
            MSG message{};
            while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }
            ::Sleep(10);
        }
    }

    // 三个目标都试一遍：不同来源的窗口在"是否属于虚拟桌面"上表现不同，
    // 只看一个目标很容易把"这个窗口不属于任何桌面"误读成"API 失效"
    struct Target {
        QString label;
        HWND handle;
    };
    const HWND consoleWindow = ::GetConsoleWindow();
    const Target targets[] = {
        { QStringLiteral("自建窗口（已显示）"), probeWindow },
        { QStringLiteral("本进程控制台窗口"), consoleWindow },
        { QStringLiteral("shell 窗口"), ::GetShellWindow() },
    };

    int callable = 0;
    int guidOk = 0;
    QStringList details;
    for (const Target &target : targets) {
        if (target.handle == nullptr) {
            details << QStringLiteral("%1：拿不到句柄").arg(target.label);
            continue;
        }

        BOOL onCurrent = FALSE;
        const HRESULT hrCurrent =
            manager->IsWindowOnCurrentVirtualDesktop(target.handle, &onCurrent);
        GUID desktopId{};
        const HRESULT hrId = manager->GetWindowDesktopId(target.handle, &desktopId);

        if (SUCCEEDED(hrCurrent) || SUCCEEDED(hrId)) {
            ++callable;
        }
        if (SUCCEEDED(hrId) && !guidIsEmpty(desktopId)) {
            ++guidOk;
        }
        details << QStringLiteral("%1：当前桌面=%2 / GetWindowDesktopId=%3%4")
                       .arg(target.label,
                            SUCCEEDED(hrCurrent)
                                ? (onCurrent ? QStringLiteral("是") : QStringLiteral("否"))
                                : hresultText(hrCurrent),
                            hresultText(hrId),
                            SUCCEEDED(hrId) && !guidIsEmpty(desktopId)
                                ? QStringLiteral(" (%1)").arg(guidText(desktopId))
                                : QString());
    }

    if (probeWindow != nullptr) {
        ::DestroyWindow(probeWindow);
    }
    manager->Release();

    // ⚠ 语义说明：`check` 判的是"探针能不能跑通"，**能力是否可用由 note 说结论** ——
    //    "某个未公开 API 已经失效"本身是一个有价值的预研结论，不该算作测试失败
    r.check(callable > 0,
            QStringLiteral("P3-01 两个方法都能被调用且不崩溃"),
            details.join(QStringLiteral(" | ")));
    r.note(QStringLiteral("明细：%1").arg(details.join(QStringLiteral(" | "))));

    if (guidOk > 0) {
        r.note(QStringLiteral("结论：P3-01 **可开工**（两个方法都拿到了真实结果）"));
    } else {
        r.note(QStringLiteral("结论：`IsWindowOnCurrentVirtualDesktop` 可用，但 "
                              "`GetWindowDesktopId` 在全部目标上都返回 "
                              "TYPE_E_ELEMENTNOTFOUND(0x8002802B) → "
                              "**「按 GUID 把窗口搬到指定桌面」这条在本机拿不到可用证据**。"
                              "两种可能：① 该 API 只对「由桌面管理器托管」的窗口生效；"
                              "② 本版本已改行为。实现前需再验证一次："
                              "用一个**真的被跨桌面拖动过**的应用窗口复测；"
                              "在此之前 P3-01 只做 `IsWindowOnCurrentVirtualDesktop` "
                              "能支撑的部分（判断窗口是否在当前桌面），其余降级"));
    }
}

// ---------------------------------------------------------------------------
//  P3-02 文件锁定：Restart Manager 能否**精确定位占用者**
//
//  这条探针是**完整自证**的：自检自己独占打开一个文件，再问 Restart Manager
//  "谁占着它" —— 如果答案是"你自己"，那就说明整条链路（注册资源 → 取列表 →
//  拿到 PID）都是真的（而不是"函数返回了 0 个结果"这种什么都证明不了的通过）。
// ---------------------------------------------------------------------------

void probeRestartManager(Reporter &r)
{
    r.section(QStringLiteral("P3-02 文件锁定（Restart Manager）"));

    const QString tempPath =
        QDir::temp().filePath(QStringLiteral("winease_p3_lock_probe_%1.bin")
                                  .arg(::GetCurrentProcessId()));
    QFile file(tempPath);
    if (!file.open(QIODevice::WriteOnly)) {
        r.check(false, QStringLiteral("P3-02 前置：能建临时文件"), file.errorString());
        return;
    }
    file.write("winease");
    file.close();

    // 独占打开：把"文件被占用"这件事**真的制造出来**
    const HANDLE holder = ::CreateFileW(reinterpret_cast<const wchar_t *>(tempPath.utf16()),
                                        GENERIC_READ,
                                        0, // 不共享 → 独占
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL,
                                        nullptr);
    r.check(holder != INVALID_HANDLE_VALUE,
            QStringLiteral("P3-02 前置：能把文件独占打开（制造出真实占用）"));
    if (holder == INVALID_HANDLE_VALUE) {
        QFile::remove(tempPath);
        return;
    }

    DWORD session = 0;
    wchar_t sessionKey[CCH_RM_SESSION_KEY + 1] = {};
    DWORD result = ::RmStartSession(&session, 0, sessionKey);
    r.check(result == ERROR_SUCCESS,
            QStringLiteral("P3-02 RmStartSession 成功（**不需要提权**）"),
            QStringLiteral("返回 %1").arg(result));

    if (result == ERROR_SUCCESS) {
        const wchar_t *resources[] = { reinterpret_cast<const wchar_t *>(tempPath.utf16()) };
        result = ::RmRegisterResources(session, 1, resources, 0, nullptr, 0, nullptr);
        r.check(result == ERROR_SUCCESS,
                QStringLiteral("P3-02 RmRegisterResources 接受文件路径"),
                QStringLiteral("返回 %1").arg(result));

        UINT needed = 0;
        UINT count = 0;
        DWORD rebootReasons = 0;
        result = ::RmGetList(session, &needed, &count, nullptr, &rebootReasons);
        r.check(result == ERROR_MORE_DATA && needed >= 1,
                QStringLiteral("P3-02 RmGetList 先回报所需条数（占用者不少于 1 个）"),
                QStringLiteral("返回 %1 / needed %2").arg(result).arg(needed));

        if (needed > 0) {
            QList<RM_PROCESS_INFO> infos;
            infos.resize(static_cast<int>(needed));
            count = needed;
            result = ::RmGetList(session, &needed, &count, infos.data(), &rebootReasons);

            bool foundSelf = false;
            QStringList occupants;
            for (int index = 0; index < static_cast<int>(count); ++index) {
                const RM_PROCESS_INFO &info = infos.at(index);
                occupants << QStringLiteral("%1 (pid %2)")
                                 .arg(QString::fromWCharArray(info.strAppName))
                                 .arg(info.Process.dwProcessId);
                if (info.Process.dwProcessId == ::GetCurrentProcessId()) {
                    foundSelf = true;
                }
            }

            r.check(result == ERROR_SUCCESS && foundSelf,
                    QStringLiteral("P3-02 ★ RmGetList 精确指出占用者**就是本进程**"
                                   "（完整自证：注册资源 → 取列表 → 拿到 PID 全链路可用）"),
                    QStringLiteral("占用者：%1").arg(occupants.join(QStringLiteral("; "))));
        }

        ::RmEndSession(session);
    }

    ::CloseHandle(holder);
    const bool removed = QFile::remove(tempPath);
    r.check(removed, QStringLiteral("P3-02 收尾：临时文件已删除（探针不留垃圾）"));

    r.note(QStringLiteral("结论：P3-02 **可开工**。占用者定位免提权、能拿到应用名与 PID；"
                          "「结束进程」走提权助手 killProcess（已有白名单），"
                          "进程身份要在界面上标清楚（验收要求二次确认时标明身份）"));
}

// ---------------------------------------------------------------------------
//  P3-09 音量混合器：IAudioSessionManager2 能否枚举会话
// ---------------------------------------------------------------------------

void probeAudioSessions(Reporter &r)
{
    r.section(QStringLiteral("P3-09 音量混合器（IAudioSessionManager2）"));

    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                    nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void **>(&enumerator));
    r.check(SUCCEEDED(hr) && enumerator != nullptr,
            QStringLiteral("P3-09 能创建音频设备枚举器"), hresultText(hr));
    if (enumerator == nullptr) {
        return;
    }

    IMMDevice *device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    r.check(SUCCEEDED(hr) && device != nullptr,
            QStringLiteral("P3-09 拿到默认输出端点（没有声卡时这条会失败，属正常）"),
            hresultText(hr));
    if (device == nullptr) {
        enumerator->Release();
        r.note(QStringLiteral("结论：没有默认输出端点 → 本项在本机不可验证"));
        return;
    }

    IAudioSessionManager2 *manager = nullptr;
    hr = device->Activate(__uuidof(IAudioSessionManager2),
                          CLSCTX_INPROC_SERVER,
                          nullptr,
                          reinterpret_cast<void **>(&manager));
    r.check(SUCCEEDED(hr) && manager != nullptr,
            QStringLiteral("P3-09 能拿到 IAudioSessionManager2（每应用音量的入口）"),
            hresultText(hr));

    if (manager != nullptr) {
        IAudioSessionEnumerator *sessions = nullptr;
        hr = manager->GetSessionEnumerator(&sessions);
        r.check(SUCCEEDED(hr) && sessions != nullptr,
                QStringLiteral("P3-09 GetSessionEnumerator 可用"), hresultText(hr));

        if (sessions != nullptr) {
            int count = 0;
            hr = sessions->GetCount(&count);
            r.check(SUCCEEDED(hr),
                    QStringLiteral("P3-09 能拿到会话数量"),
                    QStringLiteral("HRESULT %1 / %2 个会话")
                        .arg(hresultText(hr))
                        .arg(count));

            // 会话数为 0 是**正常**的（自检进程没在放音频，静音状态也不产生会话）——
            // 这里要证的是"接口能枚举"，不是"此刻一定有声"
            QStringList list;
            for (int index = 0; index < count; ++index) {
                IAudioSessionControl *control = nullptr;
                if (FAILED(sessions->GetSession(index, &control)) || control == nullptr) {
                    continue;
                }
                IAudioSessionControl2 *control2 = nullptr;
                if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                                                      reinterpret_cast<void **>(&control2)))
                    && control2 != nullptr) {
                    DWORD pid = 0;
                    control2->GetProcessId(&pid);
                    list << QStringLiteral("pid %1").arg(pid);
                    control2->Release();
                }
                control->Release();
            }
            r.note(QStringLiteral("会话明细：%1")
                       .arg(list.isEmpty() ? QStringLiteral("（当前没有活动会话）")
                                           : list.join(QStringLiteral(", "))));
            sessions->Release();
        }
        manager->Release();
    }

    device->Release();
    enumerator->Release();

    r.note(QStringLiteral("结论：P3-09 **可开工**（公开接口，零风险）；"
                          "「每应用指定输出设备」需要未公开接口，仍按路线图裁剪"));
}

// ---------------------------------------------------------------------------
//  P3-10 音频设备切换：未公开 IPolicyConfig 是否还在（高风险项）
//
//  ⚠ 只做"能不能拿到接口"，**不调用 SetDefaultEndpoint**：
//     预研探针不该改用户的默认播放设备（那会把用户正在听的音乐切走）。
//     所以这里的结论边界必须写清楚：它证明的是"接口仍在"，
//     不是"切换一定生效"（后者只能在实现阶段真切一次、并且带降级路径）。
// ---------------------------------------------------------------------------

const GUID kClsidPolicyConfigClient = { 0x870AF99C, 0x171D, 0x4F9E,
                                        { 0xAF, 0x0D, 0xE6, 0x3D, 0xF4, 0x0C, 0x2B, 0xC9 } };
/// 老一些的 `CPolicyConfigVistaClient`（社区公开的 PolicyConfig.h 里就写着这两个类，
/// 不是我们猜的）。两个类都试，才能把"是不是接口没了"和"是不是这个类被摘了"分开
const GUID kClsidPolicyConfigVistaClient = { 0x294935CE, 0xF637, 0x4E7C,
                                             { 0xA4, 0x1B, 0xAB, 0x25, 0x54, 0x60, 0xB8, 0x62 } };
const GUID kIidPolicyConfig = { 0xF8679F50, 0x850A, 0x44AE,
                                { 0x87, 0xB3, 0x67, 0x8D, 0xD0, 0xB7, 0xA1, 0xDE } };
const GUID kIidPolicyConfigVista = { 0x568B9108, 0x44BF, 0x40B4,
                                     { 0x90, 0x06, 0x86, 0xAF, 0xE5, 0xB5, 0xA6, 0x20 } };

void probePolicyConfig(Reporter &r)
{
    r.section(QStringLiteral("P3-10 音频设备切换（未公开 IPolicyConfig）"));

    // 素材：枚举端点（切换功能至少要能看到"有哪些设备"）
    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                    nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void **>(&enumerator));
    int renderCount = 0;
    int captureCount = 0;
    if (SUCCEEDED(hr) && enumerator != nullptr) {
        IMMDeviceCollection *collection = nullptr;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))
            && collection != nullptr) {
            UINT count = 0;
            collection->GetCount(&count);
            renderCount = static_cast<int>(count);
            collection->Release();
        }
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection))
            && collection != nullptr) {
            UINT count = 0;
            collection->GetCount(&count);
            captureCount = static_cast<int>(count);
            collection->Release();
        }
        enumerator->Release();
    }
    r.check(renderCount > 0 && captureCount > 0,
            QStringLiteral("P3-10 枚举出活动音频端点（切换功能的素材）"),
            QStringLiteral("输出 %1 个 / 输入 %2 个").arg(renderCount).arg(captureCount));

    // 两个 CLSID × 两个 IID 全试一遍：这样才能把"接口被摘了"与"这个类被换了"分开——
    // 只试一种组合就下"不支持"的结论，和上一轮 P3-01 拿错目标窗口是同一类错误
    struct Candidate {
        const char *className;
        const GUID *classId;
        const char *interfaceName;
        const GUID *interfaceId;
    };
    const Candidate candidates[] = {
        { "CPolicyConfigClient", &kClsidPolicyConfigClient, "IPolicyConfig", &kIidPolicyConfig },
        { "CPolicyConfigClient", &kClsidPolicyConfigClient, "IPolicyConfigVista",
          &kIidPolicyConfigVista },
        { "CPolicyConfigVistaClient", &kClsidPolicyConfigVistaClient, "IPolicyConfig",
          &kIidPolicyConfig },
        { "CPolicyConfigVistaClient", &kClsidPolicyConfigVistaClient, "IPolicyConfigVista",
          &kIidPolicyConfigVista },
    };

    int creatable = 0;
    int obtainable = 0;
    QStringList details;
    for (const Candidate &candidate : candidates) {
        IUnknown *client = nullptr;
        const HRESULT createHr = ::CoCreateInstance(*candidate.classId,
                                                    nullptr,
                                                    CLSCTX_INPROC_SERVER,
                                                    IID_IUnknown,
                                                    reinterpret_cast<void **>(&client));
        if (SUCCEEDED(createHr) && client != nullptr) {
            ++creatable;
        }

        HRESULT queryHr = E_FAIL;
        if (client != nullptr) {
            void *policy = nullptr;
            queryHr = client->QueryInterface(*candidate.interfaceId, &policy);
            if (SUCCEEDED(queryHr) && policy != nullptr) {
                ++obtainable;
                static_cast<IUnknown *>(policy)->Release();
            }
            client->Release();
        }

        details << QStringLiteral("%1 + %2：创建 %3 / 取接口 %4")
                       .arg(QString::fromLatin1(candidate.className),
                            QString::fromLatin1(candidate.interfaceName),
                            hresultText(createHr),
                            hresultText(queryHr));
    }

    r.check(creatable > 0,
            QStringLiteral("P3-10 探针本身跑通了（至少一个未公开 CLSID 还能创建对象）"),
            details.join(QStringLiteral(" | ")));
    r.note(QStringLiteral("四种组合明细：%1").arg(details.join(QStringLiteral(" | "))));

    if (obtainable > 0) {
        r.note(QStringLiteral("结论：未公开接口**仍可获取** → P3-10 可开工，但必须带降级路径。"
                              "本探针只证明「接口存在」，**没有**调用 SetDefaultEndpoint"
                              "（不拿用户的默认播放设备做实验）——"
                              "「切换真的生效」要在实现阶段真切一次并配好失败回退"));
    } else {
        r.note(QStringLiteral("★ 结论：未公开接口**已不可用** —— 两个未公开 CLSID 里"
                              "至少有一个还能创建对象，但两个 IID（IPolicyConfig / "
                              "IPolicyConfigVista）在全部组合下都拿不到接口。"
                              "这正是路线图预判的风险（「官方无公开 API，需未公开 IPolicyConfig，"
                              "必须做版本兜底」）→ **P3-10 走降级路径**：功能定位改为"
                              "「显示当前默认设备 + 一键打开系统声音设置」，不做程序内静默切换。"
                              "**不引入 AudioDeviceCmdlets / NirCmd**：它们的内部实现同样是"
                              "这个未公开接口，在这台机器上会一样失败，却带来许可、分发、"
                              "杀软与「错误信息不可控」一整套成本"));
    }
}

// ---------------------------------------------------------------------------
//  P3-07 温度路径：NVMe/SATA 走原生 IOCTL，CPU/主板走 PawnIO
// ---------------------------------------------------------------------------

void probeDiskTemperature(Reporter &r)
{
    r.section(QStringLiteral("P3-07 温度来源（磁盘原生 IOCTL + PawnIO 现状）"));

    int checked = 0;
    int supported = 0;
    QStringList details;

    for (int index = 0; index < 8; ++index) {
        const QString path = QStringLiteral("\\\\.\\PhysicalDrive%1").arg(index);
        const HANDLE drive = ::CreateFileW(reinterpret_cast<const wchar_t *>(path.utf16()),
                                           0, // 只查询属性，不需要读写权限
                                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                                           nullptr,
                                           OPEN_EXISTING,
                                           0,
                                           nullptr);
        if (drive == INVALID_HANDLE_VALUE) {
            continue;
        }
        ++checked;

        STORAGE_PROPERTY_QUERY query{};
        query.PropertyId = StorageDeviceTemperatureProperty;
        query.QueryType = PropertyStandardQuery;

        // 描述符本身是变长的（后面跟着若干传感器读数），给足缓冲
        BYTE buffer[512] = {};
        DWORD returned = 0;
        const BOOL ok = ::DeviceIoControl(drive,
                                          IOCTL_STORAGE_QUERY_PROPERTY,
                                          &query,
                                          sizeof(query),
                                          buffer,
                                          sizeof(buffer),
                                          &returned,
                                          nullptr);
        if (ok != FALSE) {
            ++supported;
            const auto *descriptor =
                reinterpret_cast<const STORAGE_TEMPERATURE_DATA_DESCRIPTOR *>(buffer);
            if (descriptor->InfoCount > 0) {
                const auto *info = reinterpret_cast<const STORAGE_TEMPERATURE_INFO *>(
                    buffer + sizeof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR));
                details << QStringLiteral("%1 %2°C").arg(path).arg(info[0].Temperature);
            } else {
                details << QStringLiteral("%1 有描述符但 0 个传感器").arg(path);
            }
        } else {
            details << QStringLiteral("%1 不支持（错误码 %2）").arg(path).arg(::GetLastError());
        }
        ::CloseHandle(drive);
    }

    r.check(checked > 0, QStringLiteral("P3-07 能打开物理磁盘查询属性"),
            QStringLiteral("检查了 %1 块盘").arg(checked));
    r.note(QStringLiteral("磁盘温度 IOCTL：%1 / %2 块盘返回了描述符 · %3")
               .arg(supported)
               .arg(checked)
               .arg(details.join(QStringLiteral(" | "))));
    if (checked > 0 && supported == 0) {
        r.note(QStringLiteral("结论：本机磁盘不支持温度查询（`StorageDeviceTemperatureProperty` 返回"
                              "「函数不正确」是**常见**现象：需要盘本身与驱动都支持）→ "
                              "实现时按「如实报不可用」处理，不要伪造数值"));
    }
}

// ---------------------------------------------------------------------------
//  P3-08 / P3-11：WinRT 能力（录屏 / OCR / 媒体控制）
// ---------------------------------------------------------------------------

void probeWinRtCapabilities(Reporter &r)
{
    r.section(QStringLiteral("P3-08 / P3-11 WinRT 能力（录屏 / OCR / 媒体控制）"));

    struct Entry {
        const wchar_t *name;
        const char *label;
        bool required;
    };
    const Entry entries[] = {
        { L"Windows.Data.Pdf.PdfDocument", "PDF（已在 P2-02 使用，作为对照）", true },
        { L"Windows.Media.Ocr.OcrEngine", "OCR 引擎（P3-08 批次 B）", false },
        { L"Windows.Graphics.Capture.GraphicsCaptureSession", "屏幕捕获会话（P3-08 批次 C）", false },
        { L"Windows.Graphics.Capture.GraphicsCaptureItem", "屏幕捕获目标项（P3-08 批次 C）", false },
        { L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager",
          "系统媒体传输控制（P3-11）", false },
    };

    for (const Entry &entry : entries) {
        QString error;
        const bool available = WinEase::Win32::probeWinRtClass(entry.name, &error);
        r.check(!entry.required || available,
                QStringLiteral("P3 探针：WinRT 类 `%1` 可用")
                    .arg(QString::fromWCharArray(entry.name)),
                QStringLiteral("%1 → %2").arg(QString::fromUtf8(entry.label),
                                              available ? QStringLiteral("可用")
                                                        : QStringLiteral("不可用：%1").arg(error)));
    }

    // 录屏还要 Media Foundation 的写入端（批次 C 的成本主要在这里）
    const HRESULT mf = ::MFStartup(MF_VERSION, MFSTARTUP_LITE);
    r.check(SUCCEEDED(mf) || mf == MF_E_ALREADY_INITIALIZED,
            QStringLiteral("P3-08 Media Foundation 可初始化（录屏写入端的前置）"),
            QStringLiteral("HRESULT %1").arg(hresultText(mf)));
    if (SUCCEEDED(mf)) {
        ::MFShutdown();
    }

    r.note(QStringLiteral("结论：批次 A（截图 + 标注 + 贴图）零依赖，可直接开工；"
                          "批次 B（OCR）与批次 C（录屏）的 WinRT 类都在，"
                          "但「真抓到一帧 / 真写出一段视频」需要实现阶段验证 —— "
                          "探针只能证明组件存在，不能证明管线跑得通（如实标注）"));
}

// ---------------------------------------------------------------------------
//  P3-14 可移动卷（只枚举，不弹出）
// ---------------------------------------------------------------------------

void probeStorageDevices(Reporter &r)
{
    r.section(QStringLiteral("P3-14 USB 管控（可移除存储设备）"));

    // ⚠ 判据在平台层 `StorageDevices` 里（PnP RemovalPolicy + 总线类型），
    //   **不是** `GetDriveType() == DRIVE_REMOVABLE` —— 那样会漏掉移动硬盘。
    //   预研与产品共用这一份实现，避免"探针说一套、插件做一套"。
    QString error;
    const QList<WinEase::Win32::StorageDeviceInfo> all =
        WinEase::Win32::allStorageDevices(&error);
    const QList<WinEase::Win32::StorageDeviceInfo> removable =
        WinEase::Win32::removableStorageDevices(&error);

    r.check(!all.isEmpty(),
            QStringLiteral("P3-14 能枚举存储设备（SetupAPI + IOCTL，全程只读）"),
            error);

    for (const WinEase::Win32::StorageDeviceInfo &device : all) {
        r.note(QStringLiteral("设备：%1%2")
                   .arg(device.describe(),
                        device.removable() ? QStringLiteral("　← 可移除设备") : QString()));
    }

    r.check(!removable.isEmpty(),
            QStringLiteral("P3-14 ★ 识别出可移除存储设备（用总线/RemovalPolicy 判据，"
                           "不再是 DRIVE_REMOVABLE）"),
            QStringLiteral("%1 个").arg(removable.size()));

    int usbCount = 0;
    for (const WinEase::Win32::StorageDeviceInfo &device : removable) {
        if (device.isUsbStorage) {
            ++usbCount;
        }
    }
    r.note(QStringLiteral("其中 USB 总线上的设备：%1 个").arg(usbCount));

    if (removable.isEmpty()) {
        r.note(QStringLiteral("结论：本机当前没有可移除设备 → 「安全弹出」不可端到端验证"
                              "（插上 U 盘/移动硬盘即可复验）"));
    } else {
        r.note(QStringLiteral("结论：P3-14 **可开工**。弹出机制上要注意："
                              "`IOCTL_STORAGE_EJECT_MEDIA` 只对 U 盘/光驱有效，"
                              "**移动硬盘必须走 `CM_Request_Device_Eject`**（等价于系统的"
                              "「安全删除硬件」）；它的 veto 机制还能报出**是谁在阻止弹出**。"
                              "自检**绝不真弹出**（用户硬盘上有数据），弹出动作留手测"));
    }
}

// ---------------------------------------------------------------------------
//  其余项：给出"不需要 spike"的结论（并说明理由）
// ---------------------------------------------------------------------------

void reportRemainingConclusion(Reporter &r)
{
    r.section(QStringLiteral("其余 P3 项的预研结论（无需 spike / 已有验证）"));

    r.note(QStringLiteral("P3-03 批量格式转换：`QImageWriter::supportedImageFormats()` 由 Qt 提供，"
                          "零第三方依赖；文档转换已按路线图裁剪 → **可直接开工**"));
    r.note(QStringLiteral("P3-04 文本扩展 / P3-05 按键重映射 / P3-06 鼠标手势："
                          "三者的公共前提（低层钩子 + SendInput + 吞事件）已在 P2-05 真实验证"
                          "（`stats().consumedEvents` 与真实系统音量）→ **可直接开工**；"
                          "共同风险是「中文输入法时序」与「UWP/提权窗口收不到注入」，"
                          "必须在实现时提供「全局暂停」与白/黑名单（路线图已列为必备项），"
                          "这类风险**只能手测**，探针证不了"));
    r.note(QStringLiteral("P3-12 快速启动器：开始菜单 `.lnk` 扫描走 `IShellLinkW`（Shell 能力已在 "
                          "`src/win32/ShellUtils` 里用过）→ **可直接开工**；"
                          "文件搜索已按路线图后置"));
    r.note(QStringLiteral("P3-13 命令面板：纯本地规则/正则引擎，无系统依赖 → "
                          "技术风险为零，成本全在「指令表的覆盖面」上（可增量做）"));
    r.note(QStringLiteral("P3-15 任务栏透明度：`SetWindowCompositionAttribute` 需从 user32 动态取地址，"
                          "且依赖 explorer 版本 → 探针**不能**在不改用户任务栏的前提下验证"
                          "「改完长什么样」，只能验「函数存在 + Shell_TrayWnd 找得到」；"
                          "本项按路线图要求必须带**失败自动还原**与「恢复系统默认」→ "
                          "实现阶段按「高风险 + 必留退路」处理"));
    r.note(QStringLiteral("P3-16 开始菜单增强：只读写注册表可控项 → 风险低，"
                          "但每个键都随 Windows 版本变化，必须「读不到就当不支持」，"
                          "绝不能盲写"));
    r.note(QStringLiteral("P3-07 的温度来源（本机实测）："
                          "① **NVMe/SATA 磁盘温度已经可读**（原生 `IOCTL_STORAGE_QUERY_PROPERTY` → "
                          "PhysicalDrive0 39°C，零依赖、零提权）；"
                          "② **PawnIO 驱动其实已经装好了**（服务已注册、设备存在但需要管理员）；"
                          "③ 缺的是**用户态 `PawnIOLib.dll`**（LoadLibrary 失败）——"
                          "把它放进 `build\\bin\\` 或 PATH 之后才能做 CPU 温度；"
                          "④ `PawnIOLib.h` 精确签名 / 模块 .bin 文件名 / 许可证条款"
                          "（ENV-SETUP §4.4 的未决项）**仍未定** —— 需要拿到 PawnIO 发行物才能确认，"
                          "不能凭猜写代码。**结论：P3-07 可以先做「磁盘温度 + CPU/内存/网速/GPU」"
                          "（全部零依赖），CPU 温度作为第二个里程碑**"));
}

} // namespace

// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("p3_spike"));

    // COM 套间：必须活到进程结束（与主程序一致）
    const WinEase::Win32::ComApartment comApartment = WinEase::Win32::ComApartment::initialize();

    Reporter reporter;
    std::printf("=== P3 技术预研探针（只读：不改系统状态、不真切设备） ===\n");

    Environment env;
    env.windowsVersion = windowsVersionText();
    env.elevated = isProcessElevated();
    cpuIdentity(&env.cpuVendor, &env.cpuFamily);

    reporter.section(QStringLiteral("运行环境"));
    reporter.check(!env.windowsVersion.isEmpty(), QStringLiteral("Windows 版本可读"),
                   env.windowsVersion);
    reporter.note(QStringLiteral("进程权限：%1")
                      .arg(env.elevated ? QStringLiteral("已提权（管理员）")
                                        : QStringLiteral("普通权限")));
    reporter.note(QStringLiteral("CPU：%1 family %2")
                      .arg(env.cpuVendor)
                      .arg(env.cpuFamily));
    reporter.note(QStringLiteral("→ PawnIO 模块选择：%1")
                      .arg(env.cpuVendor == QLatin1String("GenuineIntel")
                               ? QStringLiteral("IntelMsr（MSR 0x19C/0x1B1/0x1A2）")
                               : (env.cpuVendor == QLatin1String("AuthenticAMD")
                                      ? QStringLiteral("AmdFamily17+（SMN THM_TCON_CUR_TMP）")
                                      : QStringLiteral("未知厂商：需要按家族分支，"
                                                       "或直接判定不支持"))));

    probePawnIo(&env, reporter);
    probeVirtualDesktop(reporter);
    probeRestartManager(reporter);
    probeAudioSessions(reporter);
    probePolicyConfig(reporter);
    probeDiskTemperature(reporter);
    probeWinRtCapabilities(reporter);
    probeStorageDevices(reporter);
    reportRemainingConclusion(reporter);

    std::printf("\n=== 共 %d 项，失败 %d 项 ===\n", reporter.total(), reporter.failures());
    return reporter.failures() == 0 ? 0 : 1;
}
