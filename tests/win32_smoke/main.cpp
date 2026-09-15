// ============================================================================
//  win32_smoke —— WinEaseWin32 平台能力层自检程序
//
//  为什么需要它：
//      平台层里大量 API（窗口样式、透明度、抓屏像素、注册表往返）**无法靠
//      "编译通过"证明可用**，必须真正调用一次并核对结果。
//      本程序就是这一层功能的运行时验收手段，退出码 0 表示全部通过。
//
//  设计原则：
//      * 只操作自己创建的窗口，不干扰用户正在使用的任何窗口
//      * 注册表只在 HKCU\Software\WinEase\SmokeTest 下读写，并在结束时清理
//      * 不创建消息循环（被测 API 都不需要）
// ============================================================================

#include "win32/WinEaseWin32.h"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QStringList>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>

#include <crtdbg.h>
#include <windows.h>

namespace W = WinEase::Win32;

namespace {

// ---------------------------------------------------------------------------
//  输出辅助
// ---------------------------------------------------------------------------

constexpr const char *kColorReset = "\x1b[0m";
constexpr const char *kColorGreen = "\x1b[32m";
constexpr const char *kColorRed = "\x1b[31m";
constexpr const char *kColorGray = "\x1b[90m";
constexpr const char *kColorCyan = "\x1b[36m";
constexpr const char *kColorYellow = "\x1b[33m";

void printLine(const QString &text)
{
    const QByteArray utf8 = text.toUtf8();
    std::fwrite(utf8.constData(), 1, static_cast<size_t>(utf8.size()), stdout);
    std::fputc('\n', stdout);
}

class Reporter
{
public:
    void section(const QString &title)
    {
        printLine(QStringLiteral("\n%1== %2 ==%3")
                      .arg(QString::fromUtf8(kColorCyan), title, QString::fromUtf8(kColorReset)));
    }

    void check(bool condition, const QString &name, const QString &detail = QString())
    {
        if (condition) {
            ++m_passed;
            printLine(QStringLiteral("  %1[通过]%2 %3").arg(QString::fromUtf8(kColorGreen),
                                                          QString::fromUtf8(kColorReset), name));
        } else {
            ++m_failed;
            printLine(QStringLiteral("  %1[失败]%2 %3").arg(QString::fromUtf8(kColorRed),
                                                          QString::fromUtf8(kColorReset), name));
        }
        if (!detail.isEmpty()) {
            printLine(QStringLiteral("         %1%2%3").arg(QString::fromUtf8(kColorGray),
                                                            detail, QString::fromUtf8(kColorReset)));
        }
    }

    void note(const QString &text)
    {
        printLine(QStringLiteral("  %1· %2%3").arg(QString::fromUtf8(kColorGray),
                                                  text, QString::fromUtf8(kColorReset)));
    }

    /// 诊断项：不计入失败数。
    /// 用于"环境配置问题"这类不是代码缺陷、但会让程序行为不可靠的情况。
    void warn(bool healthy, const QString &name, const QString &detail = QString())
    {
        if (healthy) {
            ++m_passed;
            printLine(QStringLiteral("  %1[通过]%2 %3").arg(QString::fromUtf8(kColorGreen),
                                                          QString::fromUtf8(kColorReset), name));
        } else {
            ++m_warnings;
            printLine(QStringLiteral("  %1[警告]%2 %3").arg(QString::fromUtf8(kColorYellow),
                                                          QString::fromUtf8(kColorReset), name));
        }
        if (!detail.isEmpty()) {
            printLine(QStringLiteral("         %1%2%3").arg(QString::fromUtf8(kColorGray),
                                                            detail, QString::fromUtf8(kColorReset)));
        }
    }

    int warnings() const { return m_warnings; }

    int passed() const { return m_passed; }
    int failed() const { return m_failed; }

private:
    int m_passed = 0;
    int m_failed = 0;
    int m_warnings = 0;
};

// ---------------------------------------------------------------------------
//  自检专用窗口（只操作自己的窗口，不影响用户）
// ---------------------------------------------------------------------------

constexpr const wchar_t *kTestWindowClass = L"WinEaseSmokeTestWindow";
constexpr const wchar_t *kTestWindowTitle = L"WinEase 自检窗口";

LRESULT CALLBACK testWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

HWND createTestWindow()
{
    static const ATOM windowClass = [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = testWindowProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = kTestWindowClass;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        return ::RegisterClassExW(&wc);
    }();

    if (windowClass == 0) {
        return nullptr;
    }

    return ::CreateWindowExW(0,
                             kTestWindowClass,
                             kTestWindowTitle,
                             WS_OVERLAPPEDWINDOW,
                             120, 120, 420, 320,
                             nullptr,
                             nullptr,
                             ::GetModuleHandleW(nullptr),
                             nullptr);
}

// ---------------------------------------------------------------------------
//  各组检查
// ---------------------------------------------------------------------------

void checkErrors(Reporter &r)
{
    r.section(QStringLiteral("错误码中文化（Win32Error）"));

    r.check(W::errorMessage(ERROR_ACCESS_DENIED).contains(QStringLiteral("访问")),
            QStringLiteral("ERROR_ACCESS_DENIED → 中文"),
            W::errorMessage(ERROR_ACCESS_DENIED));

    r.check(W::errorMessage(ERROR_FILE_NOT_FOUND).contains(QStringLiteral("找不到")),
            QStringLiteral("ERROR_FILE_NOT_FOUND → 中文"),
            W::errorMessage(ERROR_FILE_NOT_FOUND));

    r.check(W::errorMessage(ERROR_SHARING_VIOLATION).contains(QStringLiteral("占用")),
            QStringLiteral("ERROR_SHARING_VIOLATION → 中文"),
            W::errorMessage(ERROR_SHARING_VIOLATION));

    const QString described = W::describeFailure(QStringLiteral("读取文件"), ERROR_ACCESS_DENIED);
    r.check(described.contains(QStringLiteral("读取文件")) && described.contains(QStringLiteral("5")),
            QStringLiteral("describeFailure 组装文案"), described);

    r.check(W::hresultMessage(E_INVALIDARG).contains(QStringLiteral("参数")),
            QStringLiteral("E_INVALIDARG → 中文"),
            W::hresultMessage(E_INVALIDARG));

    // HRESULT_FROM_WIN32 包装应被还原为 Win32 错误码并复用中文表
    const HRESULT wrapped = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    r.check(W::hresultMessage(wrapped).contains(QStringLiteral("找不到")),
            QStringLiteral("HRESULT_FROM_WIN32 还原正确"),
            W::hresultMessage(wrapped));

    r.check(W::isAccessDenied(ERROR_ACCESS_DENIED) && !W::isAccessDenied(ERROR_FILE_NOT_FOUND),
            QStringLiteral("isAccessDenied 判定"));
    r.check(W::isRetryableError(ERROR_BUSY) && !W::isRetryableError(ERROR_FILE_NOT_FOUND),
            QStringLiteral("isRetryableError 判定"));
}

void checkCom(Reporter &r, const W::ComApartment &com)
{
    r.section(QStringLiteral("COM 套间（ComApartment）"));

    // 注意：守卫对象由 main() 持有并存活到进程结束。
    // 这正是 ComApartment 的契约——返回值一旦析构就会 CoUninitialize，
    // 后续所有 COM/WinRT 调用都会失败。
    r.check(com.succeeded(), QStringLiteral("初始化 COM 套间"), com.message());

    const W::ApartmentState state = W::currentApartmentState();
    r.check(state != W::ApartmentState::NotInitialized,
            QStringLiteral("套间状态探测：已初始化"),
            W::apartmentStateText(state));

    r.check(W::isThreadComReady(), QStringLiteral("线程已满足 COM 调用前提"));
    r.note(QStringLiteral("套间模型：%1（S_FALSE 表示 Qt 已提前初始化，属正常）")
               .arg(com.alreadyInitialized() ? QStringLiteral("沿用既有设置")
                                             : QStringLiteral("本次完成初始化")));
}

void checkMonitors(Reporter &r)
{
    r.section(QStringLiteral("显示器与 DPI（WindowUtils）"));

    const QList<W::MonitorInfo> all = W::monitors();
    r.check(!all.isEmpty(), QStringLiteral("枚举显示器"), QStringLiteral("共 %1 个").arg(all.size()));

    const W::MonitorInfo primary = W::primaryMonitor();
    r.check(primary.valid && primary.primary, QStringLiteral("主显示器识别"),
            QStringLiteral("%1  工作区 %2x%3 @ %4,%5  DPI %6")
                .arg(primary.deviceName)
                .arg(primary.workArea.width())
                .arg(primary.workArea.height())
                .arg(primary.workArea.x())
                .arg(primary.workArea.y())
                .arg(primary.dpi));

    r.check(primary.workArea.height() <= primary.geometry.height(),
            QStringLiteral("工作区不大于完整区域（任务栏已被排除）"),
            QStringLiteral("geometry %1x%2 / workArea %3x%4")
                .arg(primary.geometry.width())
                .arg(primary.geometry.height())
                .arg(primary.workArea.width())
                .arg(primary.workArea.height()));

    r.note(QStringLiteral("DPI 感知级别：%1").arg(W::dpiAwarenessText()));
    r.note(QStringLiteral("说明：自检程序已显式声明为按显示器 DPI 感知，"
                          "与 WinEase.exe 一致，因此下列坐标均为真实物理像素。"));

    // 逻辑 <-> 物理 换算往返。
    // 取整是固有损耗（640@150% → 427 → 641），因此断言容差为 1 像素。
    const QRect physical(200, 160, 640, 480);
    const QRect logical = W::toLogical(physical, 1.5);
    const QRect roundTrip = W::toPhysical(logical, 1.5);
    const bool roundTripOk = std::abs(roundTrip.x() - physical.x()) <= 1
                             && std::abs(roundTrip.y() - physical.y()) <= 1
                             && std::abs(roundTrip.width() - physical.width()) <= 1
                             && std::abs(roundTrip.height() - physical.height()) <= 1;
    r.check(roundTripOk, QStringLiteral("坐标换算往返误差 ≤ 1 像素（取整固有损耗）"),
            QStringLiteral("物理 %1,%2 %3x%4 → 逻辑 %5,%6 %7x%8 → 物理 %9,%10 %11x%12")
                .arg(physical.x()).arg(physical.y()).arg(physical.width()).arg(physical.height())
                .arg(logical.x()).arg(logical.y()).arg(logical.width()).arg(logical.height())
                .arg(roundTrip.x()).arg(roundTrip.y()).arg(roundTrip.width()).arg(roundTrip.height()));
}

void checkTestWindow(Reporter &r)
{
    r.section(QStringLiteral("窗口能力（WindowUtils，仅操作自检窗口）"));

    const HWND hwnd = createTestWindow();
    if (hwnd == nullptr) {
        r.check(false, QStringLiteral("创建自检窗口"), W::describeFailure(QStringLiteral("创建窗口"), W::lastError()));
        return;
    }

    // 不抢用户焦点地显示出来
    ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    r.check(W::isValidWindow(hwnd) && W::isWindowVisible(hwnd), QStringLiteral("窗口有效且可见"));
    r.check(W::windowTitle(hwnd) == QString::fromWCharArray(kTestWindowTitle),
            QStringLiteral("读取窗口标题"), W::windowTitle(hwnd));
    r.check(!W::windowClassName(hwnd).isEmpty(), QStringLiteral("读取窗口类名"),
            W::windowClassName(hwnd));
    r.check(W::isOwnWindow(hwnd), QStringLiteral("识别为自身进程窗口"));
    r.check(!W::isShellWindow(hwnd), QStringLiteral("非系统外壳窗口"));
    r.note(QStringLiteral("自身窗口的 isManageableWindow() = %1（按设计应为 false，"
                          "避免功能误操作自己的窗口）")
               .arg(W::isManageableWindow(hwnd) ? QStringLiteral("true") : QStringLiteral("false")));

    // ---------------- Z 序 ----------------
    r.check(!W::isTopMost(hwnd), QStringLiteral("初始状态非置顶"));
    W::setTopMost(hwnd, true);
    const bool becameTopMost = W::isTopMost(hwnd);
    W::setTopMost(hwnd, false);
    const bool restoredTopMost = !W::isTopMost(hwnd);
    r.check(becameTopMost && restoredTopMost, QStringLiteral("置顶 → 取消置顶 往返正确"));

    // ---------------- 透明度 ----------------
    const W::OpacitySnapshot opacitySnapshot = W::captureOpacityState(hwnd);
    r.check(opacitySnapshot.valid && !opacitySnapshot.wasLayered,
            QStringLiteral("透明度快照：初始为非分层窗口"));

    const bool opacityApplied = W::setOpacity(hwnd, 0.5);
    const qreal midOpacity = W::opacity(hwnd);
    r.check(opacityApplied && std::fabs(midOpacity - 0.5) < 0.02,
            QStringLiteral("设置透明度 0.5"),
            QStringLiteral("读回 %1").arg(midOpacity, 0, 'f', 3));

    const bool opacityRestored = W::restoreOpacityState(hwnd, opacitySnapshot);
    const bool layeredRemoved = (::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) == 0;
    r.check(opacityRestored && layeredRemoved && std::fabs(W::opacity(hwnd) - 1.0) < 0.001,
            QStringLiteral("透明度还原：读回 1.0 且 WS_EX_LAYERED 被清理"));

    // 越界值应被收敛到合法区间
    r.check(W::setOpacity(hwnd, 0.0), QStringLiteral("透明度 0.0 被收敛（不应变成全透明）"),
            QStringLiteral("读回 %1").arg(W::opacity(hwnd), 0, 'f', 3));
    W::restoreOpacityState(hwnd, opacitySnapshot);

    // ---------------- 扩展样式 ----------------
    const W::ExStyleSnapshot exSnapshot = W::captureExStyle(hwnd);
    r.check(exSnapshot.valid, QStringLiteral("捕获扩展样式快照"));

    W::setClickThrough(hwnd, true);
    const bool clickThroughOn = W::isClickThrough(hwnd);
    W::setClickThrough(hwnd, false);
    r.check(clickThroughOn && !W::isClickThrough(hwnd),
            QStringLiteral("点击穿透 开 → 关 往返正确"));

    W::setNoActivate(hwnd, true);
    const bool noActivateSet = (::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_NOACTIVATE) != 0;
    W::setNoActivate(hwnd, false);
    const bool noActivateCleared = (::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_NOACTIVATE) == 0;
    r.check(noActivateSet && noActivateCleared, QStringLiteral("不抢焦点 开 → 关 往返正确"));

    W::restoreExStyle(hwnd, exSnapshot);
    r.check((::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) == 0,
            QStringLiteral("扩展样式整体还原"));

    // ---------------- 移动 ----------------
    const QRect target(180, 200, 520, 400);
    const bool moved = W::moveWindow(hwnd, target);
    const QRect actual = W::windowRect(hwnd);
    const bool geometryOk = std::abs(actual.x() - target.x()) <= 1
                            && std::abs(actual.y() - target.y()) <= 1
                            && std::abs(actual.width() - target.width()) <= 2
                            && std::abs(actual.height() - target.height()) <= 2;
    r.check(moved && geometryOk, QStringLiteral("移动并调整窗口大小"),
            QStringLiteral("目标 %1,%2 %3x%4 → 实际 %5,%6 %7x%8")
                .arg(target.x()).arg(target.y()).arg(target.width()).arg(target.height())
                .arg(actual.x()).arg(actual.y()).arg(actual.width()).arg(actual.height()));

    // visualWindowRect 应剔除 Win10/11 的不可见投影边框，因此必须略小于 windowRect。
    // 这条断言同时是在守护"DWM 边界与 GetWindowRect 处于同一坐标系"这一前提。
    const QRect visual = W::visualWindowRect(hwnd);
    const bool visualSane = visual.isValid() && visual.width() > 0 && visual.height() > 0
                            && visual.width() <= actual.width()
                            && visual.height() <= actual.height();
    r.check(visualSane, QStringLiteral("视觉边界剔除不可见投影边框（且与 windowRect 同坐标系）"),
            QStringLiteral("windowRect %1x%2 / visualWindowRect %3x%4")
                .arg(actual.width()).arg(actual.height())
                .arg(visual.width()).arg(visual.height()));

    // ---------------- 中心定位 ----------------
    r.check(W::centerWindowOnMonitor(hwnd), QStringLiteral("居中到所在显示器工作区"),
            W::windowRect(hwnd).isValid()
                ? QStringLiteral("结果 %1,%2 %3x%4")
                      .arg(W::windowRect(hwnd).x()).arg(W::windowRect(hwnd).y())
                      .arg(W::windowRect(hwnd).width()).arg(W::windowRect(hwnd).height())
                : QString());

    ::DestroyWindow(hwnd);
    r.check(!W::isValidWindow(hwnd), QStringLiteral("窗口已销毁且句柄判定正确"));
}

void checkRegistry(Reporter &r)
{
    r.section(QStringLiteral("注册表读写（RegistryUtils）"));

    const QString subKey = QStringLiteral("Software\\WinEase\\SmokeTest");

    // 先清理可能残留的数据
    W::deleteKeyRecursively(W::RegistryRoot::CurrentUser, subKey);

    {
        W::RegistryKey key = W::RegistryKey::create(W::RegistryRoot::CurrentUser, subKey);
        if (!key.isValid()) {
            r.check(false, QStringLiteral("创建测试注册表项"), key.errorMessage());
            return;
        }

        r.check(key.setValue(QStringLiteral("StringValue"), QStringLiteral("中文测试值")),
                QStringLiteral("写入 REG_SZ（中文）"));
        r.check(key.setValue(QStringLiteral("DwordValue"), static_cast<uint>(12345)),
                QStringLiteral("写入 REG_DWORD"));
        r.check(key.setValue(QStringLiteral("QwordValue"),
                             QVariant(static_cast<qulonglong>(0x100000007ULL))),
                QStringLiteral("写入 REG_QWORD"));
        r.check(key.setValue(QStringLiteral("ExpandValue"),
                             QStringLiteral("%SystemRoot%\\System32"),
                             W::RegistryValueType::ExpandString),
                QStringLiteral("写入 REG_EXPAND_SZ"));
        r.check(key.setValue(QStringLiteral("ListValue"),
                             QStringList{ QStringLiteral("甲"), QStringLiteral("乙"),
                                          QStringLiteral("丙") }),
                QStringLiteral("写入 REG_MULTI_SZ"));

        const QStringList names = key.valueNames();
        r.check(names.contains(QStringLiteral("StringValue"))
                    && names.contains(QStringLiteral("ListValue")),
                QStringLiteral("枚举值名"), names.join(QStringLiteral(", ")));
    }

    {
        const W::RegistryKey key = W::RegistryKey::open(W::RegistryRoot::CurrentUser, subKey);
        r.check(key.isValid(), QStringLiteral("重新打开注册表项"));

        r.check(key.value(QStringLiteral("StringValue")).toString() == QStringLiteral("中文测试值"),
                QStringLiteral("REG_SZ 读回一致"),
                key.value(QStringLiteral("StringValue")).toString());

        r.check(key.value(QStringLiteral("DwordValue")).toUInt() == 12345U,
                QStringLiteral("REG_DWORD 读回一致"),
                QString::number(key.value(QStringLiteral("DwordValue")).toUInt()));

        r.check(key.value(QStringLiteral("QwordValue")).toULongLong() == 0x100000007ULL,
                QStringLiteral("REG_QWORD 读回一致"),
                QString::number(key.value(QStringLiteral("QwordValue")).toULongLong()));

        const QStringList list = key.value(QStringLiteral("ListValue")).toStringList();
        r.check(list == QStringList{ QStringLiteral("甲"), QStringLiteral("乙"), QStringLiteral("丙") },
                QStringLiteral("REG_MULTI_SZ 读回一致"), list.join(QStringLiteral("/")));

        const QString expanded = key.expandedValue(QStringLiteral("ExpandValue"));
        r.check(!expanded.contains(QLatin1Char('%')) && expanded.contains(QStringLiteral("System32")),
                QStringLiteral("REG_EXPAND_SZ 环境变量已展开"), expanded);

        r.check(key.value(QStringLiteral("不存在的键"), QStringLiteral("默认值")).toString()
                    == QStringLiteral("默认值"),
                QStringLiteral("读取不存在的值返回默认值"));

        r.check(key.hasValue(QStringLiteral("StringValue"))
                    && !key.hasValue(QStringLiteral("NoSuchValue")),
                QStringLiteral("hasValue 判定"));
    }

    // 便捷函数
    r.check(W::readValue(W::RegistryRoot::CurrentUser, subKey, QStringLiteral("StringValue")).toString()
                == QStringLiteral("中文测试值"),
            QStringLiteral("readValue 便捷函数"));
    r.check(W::keyExists(W::RegistryRoot::CurrentUser, subKey), QStringLiteral("keyExists 返回 true"));

    // 清理
    const bool removed = W::deleteKeyRecursively(W::RegistryRoot::CurrentUser, subKey);
    r.check(removed && !W::keyExists(W::RegistryRoot::CurrentUser, subKey),
            QStringLiteral("递归删除测试项（不留残留）"));
}

void checkProcesses(Reporter &r)
{
    r.section(QStringLiteral("进程查询（ProcessUtils）"));

    const quint32 selfPid = W::currentProcessId();
    r.check(selfPid != 0U, QStringLiteral("当前进程 PID"), QString::number(selfPid));

    const QString selfPath = W::currentProcessPath();
    r.check(!selfPath.isEmpty() && selfPath.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive),
            QStringLiteral("当前进程完整路径"), selfPath);

    const QString selfName = W::processName(selfPid);
    r.check(!selfName.isEmpty(), QStringLiteral("当前进程映像名"), selfName);

    const QList<W::ProcessInfo> all = W::processes();
    r.check(!all.isEmpty(), QStringLiteral("枚举全部进程"),
            QStringLiteral("共 %1 个进程").arg(all.size()));

    bool foundSelf = false;
    for (const W::ProcessInfo &info : all) {
        if (info.pid == selfPid) {
            foundSelf = true;
            break;
        }
    }
    r.check(foundSelf, QStringLiteral("枚举结果包含自身进程"));

    r.check(W::isProcessRunning(selfPid), QStringLiteral("自身进程判定为运行中"));
    r.check(!W::isProcessRunning(0xFFFFFFF0U), QStringLiteral("无效 PID 判定为未运行"));

    const QList<W::ProcessInfo> byName = W::findProcessesByName(selfName);
    bool foundByName = false;
    for (const W::ProcessInfo &info : byName) {
        if (info.pid == selfPid) {
            foundByName = true;
            break;
        }
    }
    r.check(foundByName, QStringLiteral("按名称模糊查找命中自身"),
            QStringLiteral("匹配 %1 个").arg(byName.size()));

    r.note(QStringLiteral("父进程 PID：%1").arg(W::parentProcessId(selfPid)));
    r.note(QStringLiteral("当前进程提权状态：%1")
               .arg(W::isProcessElevated(selfPid) ? QStringLiteral("管理员")
                                                  : QStringLiteral("普通权限")));
    r.check(W::isProcessElevated(selfPid) == W::isProcessElevated(),
            QStringLiteral("进程提权判定与进程级判定一致"));
}

void checkScreenCapture(Reporter &r)
{
    r.section(QStringLiteral("抓屏与取色（ScreenCapture）"));

    const QRect desktop = W::virtualDesktopRect();
    r.check(desktop.isValid() && desktop.width() > 0 && desktop.height() > 0,
            QStringLiteral("虚拟桌面区域"),
            QStringLiteral("%1,%2 %3x%4")
                .arg(desktop.x()).arg(desktop.y()).arg(desktop.width()).arg(desktop.height()));

    // 抓一小块（快）
    const QRect smallRect(0, 0, 32, 32);
    const W::CaptureResult small = W::captureScreenRect(smallRect);
    r.check(small.isValid(), QStringLiteral("抓取 32x32 屏幕区域"), small.error);
    if (small.isValid()) {
        r.check(small.image.size() == QSize(32, 32), QStringLiteral("抓取图像尺寸正确"),
                QStringLiteral("%1x%2").arg(small.image.width()).arg(small.image.height()));
    }

    // 抓主显示器（验证尺寸匹配）
    const W::MonitorInfo primary = W::primaryMonitor();
    const W::CaptureResult full = W::captureMonitor(primary);
    r.check(full.isValid(), QStringLiteral("抓取主显示器"), full.error);
    if (full.isValid()) {
        r.check(full.image.size() == primary.geometry.size(),
                QStringLiteral("主显示器抓取尺寸与几何一致"),
                QStringLiteral("图像 %1x%2 / 几何 %3x%4")
                    .arg(full.image.width()).arg(full.image.height())
                    .arg(primary.geometry.width()).arg(primary.geometry.height()));
    }

    // 越界索引应优雅失败而不是崩溃
    const W::CaptureResult outOfRange = W::captureMonitorAt(9999);
    r.check(!outOfRange.isValid() && !outOfRange.error.isEmpty(),
            QStringLiteral("越界显示器索引优雅失败"), outOfRange.error);

    // 取色
    bool colorOk = false;
    const QColor color = W::colorAt(QPoint(10, 10), &colorOk);
    r.check(colorOk && color.isValid(), QStringLiteral("单点取色"),
            QStringLiteral("RGB(%1, %2, %3)").arg(color.red()).arg(color.green()).arg(color.blue()));

    // 取色探测（带放大预览块）
    const W::PixelProbe probe = W::probePixel(QPoint(100, 100), 4);
    r.check(probe.valid && probe.patch.size() == QSize(9, 9),
            QStringLiteral("取色探测返回 9x9 预览块"),
            probe.valid ? QStringLiteral("中心色 RGB(%1, %2, %3)")
                              .arg(probe.color.red()).arg(probe.color.green()).arg(probe.color.blue())
                        : probe.error);

    // 前台窗口抓取（可能被最小化，失败不算错，只报告）
    const HWND foreground = W::foregroundWindow();
    if (W::isValidWindow(foreground)) {
        const W::CaptureResult windowCapture = W::captureWindow(foreground);
        r.note(QStringLiteral("前台窗口抓取：%1（%2）")
                   .arg(windowCapture.isValid() ? QStringLiteral("成功") : QStringLiteral("失败"),
                        windowCapture.isValid() ? QStringLiteral("%1x%2")
                                                      .arg(windowCapture.image.width())
                                                      .arg(windowCapture.image.height())
                                                : windowCapture.error));
    }
}

// ---------------------------------------------------------------------------
//  批次 B 模块
// ---------------------------------------------------------------------------

void checkShellUtils(Reporter &r)
{
    r.section(QStringLiteral("Shell 能力（ShellUtils）"));

    // ---------------- 系统文件夹 ----------------
    const QString desktop = W::knownFolderPath(W::KnownFolder::Desktop);
    r.check(!desktop.isEmpty() && QDir(desktop).exists(), QStringLiteral("已知文件夹：桌面"), desktop);

    const QString downloads = W::knownFolderPath(W::KnownFolder::Downloads);
    r.check(!downloads.isEmpty(), QStringLiteral("已知文件夹：下载"), downloads);

    // ---------------- 文件类型 ----------------
    const QString selfPath = QCoreApplication::applicationFilePath();
    const QString typeName = W::fileTypeDescription(selfPath);
    r.check(!typeName.isEmpty(), QStringLiteral("读取文件类型描述"), typeName);

    const QString contentType = W::fileContentType(QStringLiteral("dummy.txt"));
    r.check(!contentType.isEmpty(), QStringLiteral("从注册表读取 Content Type（.txt）"), contentType);

    // ---------------- 图标与缩略图 ----------------
    const W::ThumbnailResult icon = W::fileIcon(selfPath, 256);
    r.check(icon.isValid() && icon.fromShellIcon, QStringLiteral("取可执行文件高清图标（256px）"),
            icon.isValid() ? QStringLiteral("%1x%2").arg(icon.image.width()).arg(icon.image.height())
                           : icon.error);

    const W::ThumbnailResult iconFallback = W::fileIcon(QStringLiteral("notepad.exe"), 48);
    r.check(iconFallback.isValid(), QStringLiteral("对不存在的文件按扩展名取图标"),
            iconFallback.isValid() ? QStringLiteral("ok") : iconFallback.error);

    // 文本文件通常没有缩略图，这里验证"优雅失败"而不是崩溃
    const QString textFile = QDir::temp().filePath(QStringLiteral("winease_smoke_thumb.txt"));
    {
        QFile file(textFile);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write("WinEase smoke test");
        }
    }
    const W::ThumbnailResult thumbnail = W::fileThumbnail(textFile, 128);
    r.note(QStringLiteral("文本文件缩略图：%1（无缩略图提供程序时失败属正常）")
               .arg(thumbnail.isValid() ? QStringLiteral("成功") : thumbnail.error));

    // 预览接口必须在缩略图失败时回退到图标
    const W::ThumbnailResult preview = W::filePreview(textFile, 128);
    r.check(preview.isValid() && preview.fromShellIcon,
            QStringLiteral("预览接口在无缩略图时回退为图标"));

    // ---------------- 回收站删除 ----------------
    QString deleteError;
    const bool recycled = W::moveToRecycleBin(QStringList{ textFile }, &deleteError);
    r.check(recycled, QStringLiteral("删除到回收站（IFileOperation）"), deleteError);
    r.check(!QFile::exists(textFile), QStringLiteral("源文件已移出原位置"));
}

void checkSystemInfo(Reporter &r)
{
    r.section(QStringLiteral("系统信息（SystemInfo）"));

    // ---------------- 内存 ----------------
    const W::MemoryInfo memory = W::memoryInfo();
    r.check(memory.valid && memory.totalBytes > 0, QStringLiteral("内存状态"),
            QStringLiteral("总计 %1 GB / 可用 %2 GB / 占用 %3%")
                .arg(memory.totalBytes / (1024ULL * 1024ULL * 1024ULL))
                .arg(memory.availableBytes / (1024ULL * 1024ULL * 1024ULL))
                .arg(memory.usagePercent, 0, 'f', 1));

    // ---------------- CPU ----------------
    W::CpuSampler cpu;
    const W::CpuSampler::Sample firstCpu = cpu.sample();
    r.check(!firstCpu.valid && !firstCpu.error.isEmpty(),
            QStringLiteral("CPU 首次采样仅建立基准"), firstCpu.error);

    ::Sleep(600); // PDH 的 % Processor Time 需要采样间隔
    const W::CpuSampler::Sample secondCpu = cpu.sample();
    r.check(secondCpu.valid && secondCpu.overallPercent >= 0.0 && secondCpu.overallPercent <= 100.0,
            QStringLiteral("CPU 使用率采样"),
            secondCpu.valid ? QStringLiteral("总体 %1% / %2 个核心")
                                  .arg(secondCpu.overallPercent, 0, 'f', 1)
                                  .arg(secondCpu.perCorePercent.size())
                            : secondCpu.error);

    // ---------------- 网络 ----------------
    W::NetworkSampler network;
    const W::NetworkSampler::Sample firstNet = network.sample();
    r.check(!firstNet.valid, QStringLiteral("网络首次采样仅建立基准"), firstNet.error);

    ::Sleep(600);
    const W::NetworkSampler::Sample secondNet = network.sample();
    r.check(secondNet.valid, QStringLiteral("网络吞吐采样"),
            secondNet.valid ? QStringLiteral("↓ %1 B/s  ↑ %2 B/s")
                                  .arg(secondNet.rxBytesPerSecond)
                                  .arg(secondNet.txBytesPerSecond)
                            : secondNet.error);

    // ---------------- GPU ----------------
    W::GpuSampler gpu;
    gpu.sample();
    ::Sleep(300);
    const W::GpuSampler::Sample gpuSample = gpu.sample();
    // GPU 计数器在虚拟机/无独显环境下不可用，属于可接受的降级
    r.note(QStringLiteral("GPU 利用率：%1")
               .arg(gpuSample.valid ? QStringLiteral("%1%").arg(gpuSample.utilizationPercent, 0, 'f', 1)
                                    : QStringLiteral("不可用（%1）").arg(gpuSample.error)));

    // ---------------- 磁盘温度 ----------------
    const QList<W::DiskTemperature> disks = W::diskTemperatures();
    r.check(!disks.isEmpty(), QStringLiteral("枚举物理磁盘"), QStringLiteral("共 %1 个").arg(disks.size()));
    for (const W::DiskTemperature &disk : disks) {
        if (disk.valid) {
            r.note(QStringLiteral("磁盘温度：%1 — %2°C").arg(disk.model).arg(disk.celsius, 0, 'f', 0));
        } else {
            r.note(QStringLiteral("磁盘温度不可用：%1 — %2").arg(disk.model, disk.error));
        }
    }

    // ---------------- 其他 ----------------
    r.check(W::uptimeSeconds() > 0, QStringLiteral("系统运行时间"),
            QStringLiteral("%1 秒").arg(W::uptimeSeconds()));

    const W::BatteryInfo battery = W::batteryInfo();
    if (battery.present) {
        r.note(QStringLiteral("电池：%1%（%2）")
                   .arg(battery.percent)
                   .arg(battery.onAcPower ? QStringLiteral("外接电源") : QStringLiteral("电池供电")));
    } else {
        r.note(QStringLiteral("电池：无（台式机或未安装电池）"));
    }
}

void checkCoreAudio(Reporter &r)
{
    r.section(QStringLiteral("音频端点（CoreAudio）"));

    QString error;
    W::AudioEndpoint endpoint = W::AudioEndpoint::defaultEndpoint(W::AudioDirection::Render, &error);
    r.check(endpoint.isValid(), QStringLiteral("绑定默认输出端点"),
            endpoint.isValid() ? endpoint.deviceName() : error);

    if (!endpoint.isValid()) {
        return;
    }

    // 必须有音量，否则下面的往返测试没有意义
    const float originalVolume = endpoint.volume(&error);
    r.check(originalVolume >= 0.0F && originalVolume <= 1.0F, QStringLiteral("读取主音量"),
            QStringLiteral("%1%").arg(originalVolume * 100.0F, 0, 'f', 1));

    // 往返测试后必须恢复用户原值，避免测试改动用户环境
    const float target = (originalVolume > 0.5F) ? 0.30F : 0.70F;
    const bool applied = endpoint.setVolume(target, &error);
    const float readBack = endpoint.volume();
    r.check(applied && std::fabs(readBack - target) < 0.02F, QStringLiteral("设置音量并读回一致"),
            QStringLiteral("目标 %1% / 读回 %2%")
                .arg(target * 100.0F, 0, 'f', 1)
                .arg(readBack * 100.0F, 0, 'f', 1));

    const bool restored = endpoint.setVolume(originalVolume, &error);
    r.check(restored && std::fabs(endpoint.volume() - originalVolume) < 0.02F,
            QStringLiteral("音量已还原为原值"),
            QStringLiteral("%1%").arg(endpoint.volume() * 100.0F, 0, 'f', 1));

    // 静音往返（同样恢复原状）
    const bool originalMuted = endpoint.isMuted();
    endpoint.setMuted(!originalMuted, &error);
    const bool toggled = endpoint.isMuted();
    endpoint.setMuted(originalMuted, &error);
    r.check(toggled != originalMuted && endpoint.isMuted() == originalMuted,
            QStringLiteral("静音开关往返正确且已还原"));

    // 麦克风端点（可能不存在，属于可接受的降级）
    W::AudioEndpoint capture = W::AudioEndpoint::defaultEndpoint(W::AudioDirection::Capture, &error);
    r.note(QStringLiteral("默认输入端点：%1")
               .arg(capture.isValid() ? capture.deviceName() : QStringLiteral("不可用（%1）").arg(error)));
}

// ---------------------------------------------------------------------------
//  音频会话（P3-09）
// ---------------------------------------------------------------------------

/// 会话音量/静音的还原守卫：无论中间断言成败，析构时一定把原值写回。
/// 纪律与 CoreAudio 的主音量往返一致：**改了就一定要还原**，
/// 否则用户会莫名其妙发现某个应用变小声了。
class SessionStateGuard
{
public:
    SessionStateGuard(QString instanceId, float volume, bool muted, bool muteValid)
        : m_instanceId(std::move(instanceId))
        , m_volume(volume)
        , m_muted(muted)
        , m_muteValid(muteValid)
    {
    }

    ~SessionStateGuard()
    {
        QString error;
        if (m_volume >= 0.0F) {
            W::setAudioSessionVolume(m_instanceId, m_volume, &error);
        }
        if (m_muteValid) {
            W::setAudioSessionMuted(m_instanceId, m_muted, &error);
        }
    }

    SessionStateGuard(const SessionStateGuard &) = delete;
    SessionStateGuard &operator=(const SessionStateGuard &) = delete;

private:
    QString m_instanceId;
    float m_volume = -1.0F;
    bool m_muted = false;
    bool m_muteValid = false;
};

/// 按实例 id 重新读一次真实状态（断言必须落在"系统里现在是什么值"上）
bool readSession(const QString &instanceId, W::AudioSessionInfo *out, QString *errorOut)
{
    QString error;
    const QList<W::AudioSessionInfo> sessions = W::audioSessions(&error);
    for (const W::AudioSessionInfo &session : sessions) {
        if (!instanceId.isEmpty() && session.instanceId == instanceId) {
            if (out != nullptr) {
                *out = session;
            }
            return true;
        }
    }
    if (errorOut != nullptr) {
        *errorOut = error.isEmpty() ? QStringLiteral("该会话已不在列表里") : error;
    }
    return false;
}

void checkAudioSessions(Reporter &r)
{
    r.section(QStringLiteral("音频会话（AudioSessions）"));

    // ---- 0) 分界线：这一段只动会话音量，系统主音量必须一动不动 ----
    QString error;
    W::AudioEndpoint endpoint = W::AudioEndpoint::defaultEndpoint(W::AudioDirection::Render, &error);
    r.check(endpoint.isValid(), QStringLiteral("绑定默认输出端点"),
            endpoint.isValid() ? endpoint.deviceName() : error);
    if (!endpoint.isValid()) {
        return;
    }
    const float mainVolumeBefore = endpoint.volume(&error);
    const bool mainMutedBefore = endpoint.isMuted();

    // ---- 1) 设备 id / 名称：id 是热插拔判据，必须稳定且非空 ----
    const QString deviceIdFirst = W::defaultRenderDeviceId();
    const QString deviceIdSecond = W::defaultRenderDeviceId();
    r.check(!deviceIdFirst.isEmpty() && deviceIdFirst == deviceIdSecond,
            QStringLiteral("默认输出设备 id 稳定且非空"), deviceIdFirst);
    r.check(!W::defaultRenderDeviceName().isEmpty(),
            QStringLiteral("默认输出设备友好名非空"), W::defaultRenderDeviceName());

    // ---- 2) 枚举（含过期会话，是否隐藏交给上层）----
    QString enumError;
    const QList<W::AudioSessionInfo> sessions = W::audioSessions(&enumError);
    r.check(enumError.isEmpty(), QStringLiteral("枚举默认输出设备上的音频会话"),
            enumError.isEmpty() ? QStringLiteral("共 %1 个会话").arg(sessions.size()) : enumError);
    for (const W::AudioSessionInfo &session : sessions) {
        QString probeText = QStringLiteral("（音量读不到，跳过写入探测）");
        if (session.volume >= 0.0F) {
            QString probeError;
            const bool writable = W::setAudioSessionVolume(session.instanceId, session.volume,
                                                          &probeError);
            probeText = writable ? QStringLiteral("写入路径可寻")
                                 : QStringLiteral("写入路径找不到：%1").arg(probeError);
        }
        r.note(QStringLiteral("%1 · %2 · pid=%3 · systemSounds=%4 · expired=%5 · 音量 %6 · id=%7 · %8")
                   .arg(session.describe(), session.stateText)
                   .arg(session.pid)
                   .arg(session.systemSounds ? 1 : 0)
                   .arg(session.expired ? 1 : 0)
                   .arg(session.volume >= 0.0F
                            ? QStringLiteral("%1%").arg(session.volume * 100.0F, 0, 'f', 0)
                            : QStringLiteral("读不到（%1）").arg(session.volumeError),
                        session.instanceId, probeText));
    }

    // ---- 3) 三态纪律 + 过期不可调（纯字段一致性，不需要会话存在）----
    int violations = 0;
    QStringList violationTexts;
    for (const W::AudioSessionInfo &session : sessions) {
        const bool volumeUnreadable = (session.volume < 0.0F);
        if (volumeUnreadable != !session.volumeError.isEmpty()) {
            ++violations;
            violationTexts << QStringLiteral("%1：音量与原因不一致").arg(session.describe());
        }
        if (!session.muteValid && session.muted) {
            ++violations;
            violationTexts << QStringLiteral("%1：静音状态读不到却报 true").arg(session.describe());
        }
        if (session.expired && session.adjustable()) {
            ++violations;
            violationTexts << QStringLiteral("%1：过期会话却标记为可调").arg(session.describe());
        }
        // 系统声音的判据是 IsSystemSoundsSession()，**不能要求 pid == 0**：
        // 实测本机该会话挂在真实进程（pid=2372）上。这里只断言"不把它当应用展示"。
        if (session.systemSounds) {
            if (!session.processName.isEmpty() || !session.executablePath.isEmpty()) {
                ++violations;
                violationTexts << QStringLiteral("%1：系统声音会话不该带进程信息")
                                      .arg(session.describe());
            }
            if (session.describe() != QStringLiteral("系统声音")) {
                ++violations;
                violationTexts << QStringLiteral("系统声音会话的描述不是「系统声音」");
            }
        }
        if (session.stateText.isEmpty()) {
            ++violations;
            violationTexts << QStringLiteral("有会话状态文本为空");
        }
    }
    r.check(violations == 0, QStringLiteral("三态纪律：读不到必带原因 / 过期会话不可调"),
            violationTexts.isEmpty() ? QStringLiteral("检查了 %1 个会话").arg(sessions.size())
                                     : violationTexts.join(QStringLiteral("；")));

    // ---- 4) fail-closed：定位不到就失败并给原因，不许"差不多改一份" ----
    const QString bogusId = QStringLiteral("WinEase::不存在的会话::%1").arg(::GetCurrentProcessId());
    QString missError;
    r.check(!W::setAudioSessionVolume(bogusId, 0.5F, &missError) && !missError.isEmpty(),
            QStringLiteral("定位不到会话时如实失败（fail-closed）"), missError);

    QStringList batchFailures;
    const int bogusBatch = W::applySessionVolume({ bogusId }, 0.5F, &batchFailures);
    r.check(bogusBatch == 0 && batchFailures.size() == 1,
            QStringLiteral("批量写入：找不到的会话计入失败而不是静默跳过"),
            batchFailures.join(QStringLiteral("；")));

    // ---- 5) 往返：挑一份**当前可调**的真实会话，改完必须还原 ----
    // 优先挑非系统声音的会话（系统声音会话调了会让所有提示音变小声，影响面更大）
    int pickIndex = -1;
    for (int index = 0; index < sessions.size(); ++index) {
        const W::AudioSessionInfo &session = sessions.at(index);
        if (!session.adjustable() || session.volume < 0.0F || !session.muteValid
            || session.systemSounds) {
            continue;
        }
        pickIndex = index;
        break;
    }
    if (pickIndex < 0) {
        // 没有任何可调的第三方会话：如实报前置，不静默跳过（"没东西可测"不等于通过）
        int fallback = -1;
        for (int index = 0; index < sessions.size(); ++index) {
            if (sessions.at(index).adjustable() && sessions.at(index).volume >= 0.0F
                && sessions.at(index).muteValid) {
                fallback = index;
                break;
            }
        }
        r.check(fallback >= 0,
                QStringLiteral("前置：系统里存在一份可读写的音频会话（用于往返测试）"),
                fallback >= 0
                    ? QStringLiteral("仅系统声音会话可测；播放任意应用音频可覆盖第三方会话")
                    : QStringLiteral("本机当前没有任何可读写的音频会话 —— 请播放任意音频后重跑本套自检"));
        if (fallback < 0) {
            return;
        }
        pickIndex = fallback;
        r.note(QStringLiteral("只有系统声音会话可测（风险：会有极短暂的提示音音量变化）"));
    }

    const W::AudioSessionInfo picked = sessions.at(pickIndex);
    r.note(QStringLiteral("往返测试对象：%1（音量 %2%）")
               .arg(picked.describe())
               .arg(picked.volume * 100.0F, 0, 'f', 1));

    const SessionStateGuard guard(picked.instanceId, picked.volume, picked.muted, picked.muteValid);

    // 5.1 实例 id 跨枚举稳定：插件靠它在两轮轮询之间对上"同一个会话"
    W::AudioSessionInfo reread;
    r.check(readSession(picked.instanceId, &reread, &error)
                && reread.instanceId == picked.instanceId,
            QStringLiteral("会话实例 id 跨枚举稳定"), error);

    // 5.2 音量往返
    const float target = (picked.volume > 0.5F) ? 0.30F : 0.70F;
    const bool applied = W::setAudioSessionVolume(picked.instanceId, target, &error);
    W::AudioSessionInfo afterSet;
    const bool readBackOk = readSession(picked.instanceId, &afterSet, &error);
    r.check(applied && readBackOk && std::fabs(afterSet.volume - target) < 0.02F,
            QStringLiteral("设置会话音量并读回一致"),
            QStringLiteral("目标 %1% / 读回 %2%")
                .arg(target * 100.0F, 0, 'f', 1)
                .arg(readBackOk ? afterSet.volume * 100.0F : -1.0F, 0, 'f', 1));

    // 5.3 越界入参必须收敛（否则 SetMasterVolume 会收到非法值）
    W::setAudioSessionVolume(picked.instanceId, 1.5F, &error);
    W::AudioSessionInfo afterClamp;
    const bool clampOk = readSession(picked.instanceId, &afterClamp, &error);
    r.check(clampOk && std::fabs(afterClamp.volume - 1.0F) < 0.02F,
            QStringLiteral("音量入参越界收敛到 [0,1]"),
            QStringLiteral("传入 150% → 读回 %1%")
                .arg(clampOk ? afterClamp.volume * 100.0F : -1.0F, 0, 'f', 1));

    // 5.4 静音往返
    const bool muteApplied = W::setAudioSessionMuted(picked.instanceId, !picked.muted, &error);
    W::AudioSessionInfo afterMute;
    const bool muteReadOk = readSession(picked.instanceId, &afterMute, &error);
    r.check(muteApplied && muteReadOk && afterMute.muted == !picked.muted,
            QStringLiteral("设置会话静音并读回一致"),
            QStringLiteral("目标 %1 / 读回 %2")
                .arg(!picked.muted ? QStringLiteral("静音") : QStringLiteral("取消静音"),
                     muteReadOk ? (afterMute.muted ? QStringLiteral("静音") : QStringLiteral("取消静音"))
                                : QStringLiteral("读不到")));

    // 5.5 还原
    W::setAudioSessionVolume(picked.instanceId, picked.volume, &error);
    W::setAudioSessionMuted(picked.instanceId, picked.muted, &error);
    W::AudioSessionInfo afterRestore;
    const bool restoreOk = readSession(picked.instanceId, &afterRestore, &error);
    r.check(restoreOk && std::fabs(afterRestore.volume - picked.volume) < 0.02F
                && afterRestore.muted == picked.muted,
            QStringLiteral("会话音量与静音已还原为原值"),
            QStringLiteral("音量 %1% / 静音 %2")
                .arg(restoreOk ? afterRestore.volume * 100.0F : -1.0F, 0, 'f', 1)
                .arg(afterRestore.muted ? QStringLiteral("是") : QStringLiteral("否")));

    // 5.6 批量入口：同一批会话必须落在同一次枚举快照上（一个真 id + 一个假 id）
    QStringList mixedFailures;
    const int mixed = W::applySessionVolume({ picked.instanceId, bogusId }, picked.volume,
                                            &mixedFailures);
    r.check(mixed == 1 && mixedFailures.size() == 1,
            QStringLiteral("批量写入逐条报失败（成功 1 笔 / 失败 1 条）"),
            QStringLiteral("成功 %1 笔；失败：%2").arg(mixed).arg(mixedFailures.join(QStringLiteral("；"))));

    // ---- 6) 分界线断言：整段下来系统主音量一个字都没动 ----
    r.check(std::fabs(endpoint.volume() - mainVolumeBefore) < 0.001F
                && endpoint.isMuted() == mainMutedBefore,
            QStringLiteral("★ 调会话音量不影响系统主音量（两条链路的分界线）"),
            QStringLiteral("主音量 %1% → %2%")
                .arg(mainVolumeBefore * 100.0F, 0, 'f', 1)
                .arg(endpoint.volume() * 100.0F, 0, 'f', 1));
}

void checkDisplayControl(Reporter &r)
{
    r.section(QStringLiteral("显示器控制（DisplayControl）"));

    const QList<W::MonitorInfo> all = W::monitors();
    r.check(!all.isEmpty(), QStringLiteral("枚举显示器供 DDC/CI 使用"));

    // ---------------- DDC/CI 亮度 ----------------
    const W::DisplayValueCapability brightness = W::queryBrightness(0);
    if (brightness.supported) {
        r.check(true, QStringLiteral("查询亮度能力"),
                QStringLiteral("当前 %1（范围 %2~%3）")
                    .arg(brightness.current).arg(brightness.minimum).arg(brightness.maximum));
    } else {
        r.note(QStringLiteral("亮度 DDC/CI 不支持：%1").arg(brightness.error));
    }

    // ---------------- gamma / 色温 ----------------
    r.check(W::isGammaSupported(0), QStringLiteral("显示器 0 支持 gamma 调节"));

    const W::GammaSnapshot baseline = W::captureGamma(0);
    r.check(baseline.valid && baseline.ramp.size() == 768, QStringLiteral("捕获 gamma 基准"),
            baseline.valid ? QStringLiteral("768 级采样值") : baseline.error);
    if (!baseline.valid) {
        return;
    }

    // 色温增益计算（纯计算，无副作用）
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    W::colorTemperatureGains(6500, &red, &green, &blue);
    r.check(red == 1.0 && green > 0.9 && green <= 1.0 && blue < green && blue > 0.5,
            QStringLiteral("6500K 增益接近中性且最大通道归一到 1.0"),
            QStringLiteral("R %1 / G %2 / B %3")
                .arg(red, 0, 'f', 3).arg(green, 0, 'f', 3).arg(blue, 0, 'f', 3));

    W::colorTemperatureGains(3500, &red, &green, &blue);
    r.check(red == 1.0 && blue < green, QStringLiteral("3500K 增益偏暖（蓝通道最低）"),
            QStringLiteral("R %1 / G %2 / B %3")
                .arg(red, 0, 'f', 3).arg(green, 0, 'f', 3).arg(blue, 0, 'f', 3));

    // 真实应用一次色温再立刻还原（屏幕上会有不足 100ms 的短暂色偏，属预期）
    QString error;
    const bool applied = W::applyColorTemperature(0, 5200, baseline, &error);
    r.check(applied, QStringLiteral("应用色温 5200K（屏幕会短暂变色，随后立即还原）"), error);

    const bool restored = W::restoreGamma(baseline, &error);
    r.check(restored, QStringLiteral("还原原始 gamma"), error);
}

/// 仅用于 STL 互操作诊断：把 std::wstring 转回 QString。
/// 刻意不复用平台层的辅助函数——本检查要验证的是"跨模块传递 STL 类型"本身是否可靠。
QString fromWinRtProbeImpl(const std::wstring &text)
{
    return QString::fromWCharArray(text.c_str(), static_cast<int>(text.size()));
}

void checkWinRt(Reporter &r)
{
    r.section(QStringLiteral("C++/WinRT 支撑（WinRtSupport）"));

    QString error;
    r.check(W::ensureWinRtReady(&error), QStringLiteral("线程满足 WinRT 调用前提"), error);

    // 真实调用 WinRT 运行时：验证 winrt 头在本工程编译选项下确实可用
    // （这正是 P0-6 启用 QT_NO_KEYWORDS 要解决的目标）
    r.check(W::probeWinRtClass(L"Windows.Data.Pdf.PdfDocument", &error),
            QStringLiteral("探测 WinRT 类：Windows.Data.Pdf.PdfDocument 可用"), error);

    error.clear();
    r.check(!W::probeWinRtClass(L"Windows.No.Such.RuntimeClass", &error) && !error.isEmpty(),
            QStringLiteral("不存在的 WinRT 类返回 false 且不崩溃"), error);

    // ---------------- 跨模块 STL 互操作诊断 ----------------
    //
    // 这是"环境配置问题"而非代码缺陷：Qt 与我们的程序若编译配置不一致
    // （典型：release-only 的 Qt + Debug 构建的程序），
    // MSVC 下 STL 类型的布局与 CRT 堆都不兼容，
    // 凡是"Qt 返回/接收 STL 类型"的接口都会内存损坏。
    // 症状极其隐蔽：数值看似正常、退出时才崩溃。
    // 这里主动探测并给出明确指引，避免后续被这种问题浪费大量时间。
    const QString probe = QStringLiteral("WinEase 中文测试");
    const std::wstring probeWide = probe.toStdWString();
    const QString probeBack = fromWinRtProbeImpl(probeWide);
    const bool stlHealthy = (probeBack == probe);
    r.warn(stlHealthy, QStringLiteral("跨模块 STL 互操作（Qt 返回 std::wstring）"),
           stlHealthy
               ? QStringLiteral("正常（构建配置与 Qt 一致）")
               : QStringLiteral("异常：原文 %1 单元 → 宽字符 %2 单元 → 回读 %3 单元。"
                                "几乎必然是构建配置与 Qt 不一致——Qt 为 release-only 安装时，"
                                "必须用 Release / RelWithDebInfo 构建，"
                                "Debug 会因 STL 布局与堆不兼容而内存损坏。")
                     .arg(probe.size()).arg(probeWide.size()).arg(probeBack.size()));

    // ---------------- 异常边界（本模块存在的主要意义） ----------------
    QString caught;
    const bool ok = W::runWinRt(QStringLiteral("模拟失败调用"), &caught, [] {
        throw std::runtime_error("simulated failure");
    });
    r.check(!ok && !caught.isEmpty() && caught.contains(QStringLiteral("模拟失败调用")),
            QStringLiteral("C++ 异常被异常边界捕获并转为中文错误"), caught);

    QString caughtUnknown;
    const bool okUnknown = W::runWinRt(QStringLiteral("模拟未知异常"), &caughtUnknown, [] {
        throw 42; // 非 std::exception
    });
    r.check(!okUnknown && !caughtUnknown.isEmpty(),
            QStringLiteral("非标准异常同样被捕获（不会终止进程）"), caughtUnknown);

    QString successError;
    r.check(W::runWinRt(QStringLiteral("正常调用"), &successError, [] {}),
            QStringLiteral("正常调用返回成功"));
}

} // namespace

// ---------------------------------------------------------------------------
//  崩溃原因可视化
//
//  平台层里 Win32 / COM / WinRT 的失败方式五花八门：C++ 异常、CRT 非法参数、
//  Debug 断言。默认情况下它们都表现为"进程无输出地退出（STATUS_BREAKPOINT）"，
//  完全无法定位。这里统一接管，把原因打出来再退出，
//  这样任何一次崩溃都能立刻知道发生在哪一类问题上。
// ---------------------------------------------------------------------------

void installCrashDiagnostics()
{
    std::set_terminate([] {
        printLine(QStringLiteral("%1[致命] 未捕获的异常导致进程终止%2")
                      .arg(QString::fromUtf8(kColorRed), QString::fromUtf8(kColorReset)));
        const std::exception_ptr failure = std::current_exception();
        try {
            if (failure != nullptr) {
                std::rethrow_exception(failure);
            }
        } catch (const std::exception &error) {
            printLine(QStringLiteral("        类型：std::exception"));
            printLine(QStringLiteral("        信息：%1").arg(QString::fromUtf8(error.what())));
        } catch (...) {
            printLine(QStringLiteral("        类型：非标准异常（可能是 WinRT/SEH 异常）"));
        }
        std::fflush(stdout);
        std::_Exit(70);
    });

    _set_invalid_parameter_handler([] (const wchar_t *expression, const wchar_t *function,
                                       const wchar_t *file, unsigned int line, uintptr_t) {
        printLine(QStringLiteral("%1[致命] CRT 检测到非法参数%2")
                      .arg(QString::fromUtf8(kColorRed), QString::fromUtf8(kColorReset)));
        printLine(QStringLiteral("        函数：%1").arg(QString::fromWCharArray(function != nullptr ? function : L"?")));
        printLine(QStringLiteral("        文件：%1 行 %2")
                      .arg(QString::fromWCharArray(file != nullptr ? file : L"?"))
                      .arg(line));
        printLine(QStringLiteral("        表达式：%1").arg(QString::fromWCharArray(expression != nullptr ? expression : L"?")));
        std::fflush(stdout);
        std::_Exit(71);
    });

    // Debug 下的断言默认会触发断点，改成打到控制台
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDOUT);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDOUT);
}

int main(int argc, char *argv[])
{
    // 控制台切到 UTF-8，保证中文输出不乱码
    ::SetConsoleOutputCP(CP_UTF8);

    // 与 WinEase.exe 保持一致：显式声明"按显示器 DPI 感知 v2"。
    // 必须在创建任何窗口 / 初始化 Qt 之前调用。
    // 原因：DPI 不感知的进程里，GetWindowRect 返回被 Windows 虚拟化后的像素，
    // 而 DWMWA_EXTENDED_FRAME_BOUNDS 始终返回物理像素，两者坐标系不一致。
    // 自检程序要测的是**生产配置**，所以这里主动对齐 WinEase.exe 的行为。
    if (::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) == FALSE) {
        // 已有清单声明或系统不支持时忽略，后续仅记录实际感知级别
    }

    // 关闭输出缓冲：一旦后面某处崩溃，也能看到崩溃前最后一条输出
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    installCrashDiagnostics();

    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("win32_smoke"));

    // COM 套间守卫必须存活到进程结束（契约见 ComApartment.h）：
    // 一旦析构就会 CoUninitialize，之后 Core Audio / Shell / WinRT 全部失败。
    const W::ComApartment com = W::ComApartment::initialize();

    printLine(QStringLiteral("WinEase 平台能力层自检（WinEaseWin32）"));
    printLine(QStringLiteral("测试目标：仅自检窗口与 HKCU\\Software\\WinEase，不影响用户环境"));

    Reporter reporter;
    checkErrors(reporter);
    checkCom(reporter, com);
    checkMonitors(reporter);
    checkTestWindow(reporter);
    checkRegistry(reporter);
    checkProcesses(reporter);
    checkScreenCapture(reporter);
    checkShellUtils(reporter);
    checkSystemInfo(reporter);
    checkCoreAudio(reporter);
    checkAudioSessions(reporter);
    checkDisplayControl(reporter);
    checkWinRt(reporter);

    const int total = reporter.passed() + reporter.failed();
    printLine(QString());
    printLine(QStringLiteral("──────────── 汇总 ────────────"));
    printLine(QStringLiteral("  通过 %1 / %2").arg(reporter.passed()).arg(total));
    if (reporter.warnings() > 0) {
        printLine(QStringLiteral("  %1警告 %2（环境配置类问题，不影响功能代码正确性）%3")
                      .arg(QString::fromUtf8(kColorYellow))
                      .arg(reporter.warnings())
                      .arg(QString::fromUtf8(kColorReset)));
    }
    if (reporter.failed() > 0) {
        printLine(QStringLiteral("  %1失败 %2%3")
                      .arg(QString::fromUtf8(kColorRed))
                      .arg(reporter.failed())
                      .arg(QString::fromUtf8(kColorReset)));
    }
    printLine(QStringLiteral("  结果：%1")
                  .arg(reporter.failed() == 0 ? QStringLiteral("全部通过")
                                              : QStringLiteral("存在失败项")));

    return reporter.failed() == 0 ? 0 : 1;
}
