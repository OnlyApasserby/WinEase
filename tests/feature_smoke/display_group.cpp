#include "display_group.h"

#include <windows.h>

#undef min // windows.h 的 min/max 宏会破坏后面的 qMin/qMax/std::min
#undef max

#include <magnification.h> // MagGetWindowSource（放大倍率的硬证据）

#include "win32/CoreAudio.h"
#include "win32/DisplayControl.h"
#include "win32/Magnifier.h"
#include "win32/WindowUtils.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QThread>
#include <QTime>
#include <QTimeEdit>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace FeatureSmoke {

namespace {

namespace Win32 = WinEase::Win32;

const QString kMicId = QStringLiteral("media.mic_mute");

// 面板控件对象名（跨 DLL 边界只能靠运行期元对象找，见踩坑 #18）
const QString kMicPanelName = QStringLiteral("micMutePanel");
const QString kMicStateLabel = QStringLiteral("micMuteStateLabel");
const QString kMicDeviceLabel = QStringLiteral("micMuteDeviceLabel");
const QString kMicToggleButton = QStringLiteral("micMuteToggleButton");
const QString kMicNotifyCheck = QStringLiteral("micMuteNotifyCheck");
const QString kMicHintLabel = QStringLiteral("micMuteHintLabel");

/// 徽标文字（与插件约定一致）：静音红「静」/ 可用绿「麦」。
/// 自检这里**独立写死**期望值（而不是去引用插件里的常量）：
/// 托盘上那两个字是给用户看的字，改动了就该有人重新确认一遍。
const QString kBadgeMuted = QStringLiteral("静");
const QString kBadgeLive = QStringLiteral("麦");

const QString kMagId = QStringLiteral("display.magnifier");
const QString kMagPanelName = QStringLiteral("magnifierPanel");
const QString kMagStatusLabel = QStringLiteral("magStatusLabel");
const QString kMagToggleButton = QStringLiteral("magToggleButton");
const QString kMagFactorSpin = QStringLiteral("magFactorSpin");
const QString kMagSizeSpin = QStringLiteral("magSizeSpin");
const QString kMagShapeCombo = QStringLiteral("magShapeCombo");
const QString kMagHintLabel = QStringLiteral("magHintLabel");

/// 放大窗与光标之间的间隙（文档化的位置规则；自检独立写死，改了就有人要重新确认）
constexpr int kMagnifierGap = 24;

// ---------------------------------------------------------------------------
//  P2-08 亮度 / 色温 / 护眼（display.brightness）
// ---------------------------------------------------------------------------

const QString kBriId = QStringLiteral("display.brightness");
const QString kBriPanelName = QStringLiteral("brightnessPanel");
const QString kBriStatusLabel = QStringLiteral("briStatusLabel");
const QString kBriChannelLabel = QStringLiteral("briChannelLabel");
const QString kBriScheduleLabel = QStringLiteral("briScheduleLabel");
const QString kBriPreviewLabel = QStringLiteral("briColorPreview");
const QString kBriBrightnessSlider = QStringLiteral("briBrightnessSlider");
const QString kBriBrightnessSpin = QStringLiteral("briBrightnessSpin");
const QString kBriKelvinSlider = QStringLiteral("briKelvinSlider");
const QString kBriKelvinSpin = QStringLiteral("briKelvinSpin");
const QString kBriEyeCareCheck = QStringLiteral("briEyeCareCheck");
const QString kBriDayKelvinSpin = QStringLiteral("briDayKelvinSpin");
const QString kBriNightKelvinSpin = QStringLiteral("briNightKelvinSpin");
const QString kBriNightStartEdit = QStringLiteral("briNightStartEdit");
const QString kBriNightEndEdit = QStringLiteral("briNightEndEdit");

/// 自检预设的"启用时色温"（main.cpp 里写进配置）；刻意不是默认值 6500 ——
/// 这样"配置真的被读走"才是被断言出来的，而不是靠默认值巧合（踩坑 #26）
constexpr int kPresetKelvin = 5000;
/// 6500K 是插件文档化的"不调色"档
constexpr int kNeutralKelvin = 6500;

// ---------------------------------------------------------------------------
//  小工具
// ---------------------------------------------------------------------------

/// 静默等一会儿（照常转事件循环）：验证"某件事没发生"之前必须先给它机会发生
void settle(int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

QWidget *topLevelNamed(const QString &objectName)
{
    const QWidgetList widgets = QApplication::topLevelWidgets();
    for (QWidget *widget : widgets) {
        if (widget != nullptr && widget->objectName() == objectName) {
            return widget;
        }
    }
    return nullptr;
}

template <typename T>
T *childNamed(QWidget *root, const QString &objectName)
{
    return root != nullptr ? root->findChild<T *>(objectName) : nullptr;
}

/// 从**系统**读麦克风静音状态；读不到时 okOut 为 false（绝不当成"未静音"）
bool readMicMuted(const Win32::AudioEndpoint &mic, bool *okOut = nullptr)
{
    bool muted = false;
    QString error;
    const bool ok = mic.muteState(&muted, &error);
    if (okOut != nullptr) {
        *okOut = ok;
    }
    return ok && muted;
}

// ---------------------------------------------------------------------------
//  P2-09 用的工具
// ---------------------------------------------------------------------------

/// 放大镜宿主窗口（找不到返回 nullptr）
HWND findMagnifierHost()
{
    const QString className = WinEase::Win32::magnifierHostClassName();
    return ::FindWindowW(reinterpret_cast<const wchar_t *>(className.utf16()), nullptr);
}

/// WC_MAGNIFIER 子窗口（宿主里面那一个）
HWND magnifierChildOf(HWND host)
{
    return host != nullptr ? ::GetWindow(host, GW_CHILD) : nullptr;
}

/// 窗口矩形（物理像素，屏幕坐标）
QRect windowRectOf(HWND hwnd)
{
    RECT rect{};
    if (hwnd == nullptr || ::GetWindowRect(hwnd, &rect) == FALSE) {
        return QRect();
    }
    return QRect(static_cast<int>(rect.left), static_cast<int>(rect.top),
                 static_cast<int>(rect.right - rect.left), static_cast<int>(rect.bottom - rect.top));
}

/// 放大镜自己报的放大源矩形：`MagGetWindowSource` 是 API 的读回接口，
/// 比"插件说它放大了多少倍"硬得多（源矩形边长 == 窗口边长 / 倍率）。
QRect readMagnifierSource(HWND magnifier)
{
    RECT rect{};
    if (magnifier == nullptr || ::MagGetWindowSource(magnifier, &rect) == FALSE) {
        return QRect();
    }
    return QRect(static_cast<int>(rect.left), static_cast<int>(rect.top),
                 static_cast<int>(rect.right - rect.left), static_cast<int>(rect.bottom - rect.top));
}

/// 窗口区域类型：ERROR(0) = 没有区域（整块矩形），其余见 GetWindowRgn 文档
int windowRegionType(HWND hwnd)
{
    HRGN probe = ::CreateRectRgn(0, 0, 1, 1);
    const int type = ::GetWindowRgn(hwnd, probe);
    ::DeleteObject(probe);
    return type;
}

/// 进程累计 CPU 时间（内核 + 用户，毫秒）：0 表示读不到
qint64 processCpuTimeMs()
{
    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernel{};
    FILETIME user{};
    if (::GetProcessTimes(::GetCurrentProcess(), &creation, &exitTime, &kernel, &user) == FALSE) {
        return 0;
    }
    const auto toMs = [](const FILETIME &time) {
        ULARGE_INTEGER value{};
        value.LowPart = time.dwLowDateTime;
        value.HighPart = time.dwHighDateTime;
        return static_cast<qint64>(value.QuadPart / 10000ULL); // 100ns → ms
    };
    return toMs(kernel) + toMs(user);
}

/// 一次 CPU 成本测量（两个单位各有用途）
struct CpuCost {
    qint64 ms = -1;     ///< `GetProcessTimes` 的毫秒（分辨率约 15.6ms，只适合"占比上限"）
    qint64 cycles = -1; ///< `QueryProcessCycleTime` 的 CPU 周期（精确，用来比大小）
};

/// 进程累计 CPU 周期数。
/// 为什么还要它：`GetProcessTimes` 由时钟中断驱动（约 15.6ms 一跳），
/// "跟随 40 帧"这种量级的成本会被四舍五入成 0 —— 那样"谁更省 CPU"就没法比大小了。
qint64 processCpuCycles()
{
    ULONG64 cycles = 0;
    if (::QueryProcessCycleTime(::GetCurrentProcess(), &cycles) == FALSE) {
        return -1;
    }
    return static_cast<qint64>(cycles);
}

/// 让"跟随"这档测量**真的在跟随**：每帧把真实光标挪一点（半径 60px 的小圈），走完回原点。
///
/// 为什么必须单独测这一档：光标不动时我们**故意**跳过"把同样的源矩形再交给 API 一次"，
/// 那是省下来的活；拿"省下来的活"去和"每帧都必须重抓一次"的自抓屏比是不公平的对照。
/// 可比的是"双方都干满活"的这一档（踩坑 #52 的续集）。
void moveCursorWhileSettling(int frames, int frameMs, const QPoint &center)
{
    constexpr int kRadiusPx = 60;
    constexpr double kTwoPi = 6.28318530717958647692;
    for (int frame = 0; frame < frames; ++frame) {
        const double angle = kTwoPi * static_cast<double>(frame) / static_cast<double>(frames);
        const int x = center.x() + static_cast<int>(std::cos(angle) * kRadiusPx);
        const int y = center.y() + static_cast<int>(std::sin(angle) * kRadiusPx);
        ::SetCursorPos(x, y);
        settle(frameMs);
    }
    ::SetCursorPos(center.x(), center.y());
}

/// "单核 100%"的尺子：紧循环跑一小段，量出这段时间本进程烧掉多少 CPU 周期，换算成"周期/毫秒"。
///
/// 为什么需要它：想把"占用百分之几的单核"说成一个数，得先知道"一个核跑满是多少"。
/// `GetProcessTimes` 的毫秒撑不起这件事 —— 本机上**同样的测量**两次能差 10 倍
/// （16 ms vs 172 ms，而同时测到的 CPU 周期几乎一致），它的 15.6ms 时钟分辨率加上
/// 线程记账方式让它在"几十毫秒"这个量级上完全不可信。周期数才是稳的，
/// 所以用它当分子、"满负荷"当分母。
double fullCoreCyclesPerMs(int calibrationMs)
{
    const qint64 beforeCycles = processCpuCycles();
    QElapsedTimer timer;
    timer.start();
    volatile quint64 sink = 0;
    while (timer.elapsed() < calibrationMs) {
        for (int i = 0; i < 1000; ++i) {
            sink = sink + static_cast<quint64>(i) * 3ULL; // volatile：不许被优化掉
        }
    }
    const qint64 afterCycles = processCpuCycles();
    Q_UNUSED(sink)
    const qint64 elapsed = qMax<qint64>(1, timer.elapsed());
    return static_cast<double>(afterCycles - beforeCycles) / static_cast<double>(elapsed);
}

/// 参考实现用的"呈现窗口"类名（可见、置顶、不抢焦点；画完即毁）
const wchar_t kReferenceWindowClass[] = L"WinEaseSmokeSelfCapture";

/// 建一个**真的可见**的窗口给参考实现当画布。
///
/// 为什么必须可见：只往内存 DC 里画的"自抓屏"是**半套活** —— 真正的自抓屏放大镜
/// 还得把缩放结果送到屏幕上（一次 BitBlt + 合成）。少了这一步，参考实现就凭空
/// 少干一半活，拿它去比"我们的放大镜已经在屏上了"是不公平的对照（踩坑 #52 的续集）。
HWND createReferenceWindow(int width, int height)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kReferenceWindowClass;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    ::RegisterClassW(&wc); // 已注册时返回 0，无所谓

    HWND window = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                    kReferenceWindowClass, L"", WS_POPUP | WS_VISIBLE, 0, 0, width,
                                    height, nullptr, nullptr, wc.hInstance, nullptr);
    if (window != nullptr) {
        ::ShowWindow(window, SW_SHOWNOACTIVATE);
    }
    return window;
}

/// "自抓屏方案"的参考实现：抓屏 → 缩放 → **呈现到真实窗口**，同一个源区域、同一帧率。
///
/// 它存在的意义只有一个：让"这套方案到底省不省 CPU"这句话**当场被量出来**，
/// 而不是写在文档里让人信（量出来是"不省"也得认，见 ROADMAP 里 P2-09 的验收结论）。
/// 缩放开了 HALFTONE —— 自抓屏方案想做到和 API 一样的观感就得这么干（更贵的那条路）。
CpuCost measureSelfCaptureCpu(int frames, int frameMs, const QRect &source, const QSize &target)
{
    CpuCost cost;
    HDC screen = ::GetDC(nullptr);
    if (screen == nullptr) {
        return cost;
    }
    HWND window = createReferenceWindow(target.width(), target.height());
    if (window == nullptr) {
        ::ReleaseDC(nullptr, screen);
        return cost;
    }
    HDC sourceDc = ::CreateCompatibleDC(screen);
    HDC targetDc = ::CreateCompatibleDC(screen);
    HBITMAP sourceBitmap = ::CreateCompatibleBitmap(screen, source.width(), source.height());
    HBITMAP targetBitmap = ::CreateCompatibleBitmap(screen, target.width(), target.height());
    HGDIOBJ oldSource = ::SelectObject(sourceDc, sourceBitmap);
    HGDIOBJ oldTarget = ::SelectObject(targetDc, targetBitmap);
    ::SetStretchBltMode(targetDc, HALFTONE);
    ::SetBrushOrgEx(targetDc, 0, 0, nullptr);

    const qint64 beforeMs = processCpuTimeMs();
    const qint64 beforeCycles = processCpuCycles();
    for (int frame = 0; frame < frames; ++frame) {
        ::BitBlt(sourceDc, 0, 0, source.width(), source.height(), screen, source.x(), source.y(),
                 SRCCOPY);
        ::StretchBlt(targetDc, 0, 0, target.width(), target.height(), sourceDc, 0, 0,
                     source.width(), source.height(), SRCCOPY);
        // 呈现：真正的自抓屏放大镜必须把结果送到屏幕上（缺了它就不是同一个活）
        HDC windowDc = ::GetDC(window);
        if (windowDc != nullptr) {
            ::BitBlt(windowDc, 0, 0, target.width(), target.height(), targetDc, 0, 0, SRCCOPY);
            ::ReleaseDC(window, windowDc);
        }
        ::GdiFlush(); // GDI 会攒批，不 Flush 的话成本会被算到测量窗口之外
        QThread::msleep(static_cast<unsigned long>(frameMs));
    }
    cost.ms = processCpuTimeMs() - beforeMs;
    cost.cycles = processCpuCycles() - beforeCycles;

    ::SelectObject(sourceDc, oldSource);
    ::SelectObject(targetDc, oldTarget);
    ::DeleteObject(sourceBitmap);
    ::DeleteObject(targetBitmap);
    ::DeleteDC(sourceDc);
    ::DeleteDC(targetDc);
    ::DestroyWindow(window);
    ::ReleaseDC(nullptr, screen);
    return cost;
}

/// 用户原来的麦克风静音状态。
/// 这个功能直接改**系统状态**，用例结束必须还回去（用户可能正开着会）。
/// RAII 兜底 + 用完在收尾处显式还原一次并证实"系统接受了"。
class MicMuteGuard
{
public:
    MicMuteGuard()
    {
        QString error;
        m_endpoint = Win32::AudioEndpoint::defaultEndpoint(Win32::AudioDirection::Capture, &error);
        if (!m_endpoint.isValid()) {
            return;
        }
        bool muted = false;
        m_valid = m_endpoint.muteState(&muted, &error);
        m_muted = muted;
    }

    ~MicMuteGuard()
    {
        if (m_valid) {
            m_endpoint.setMuted(m_muted); // 尽力还原（失败也无法再做什么，只能记在日志里）
            QCoreApplication::processEvents();
        }
    }

    bool valid() const { return m_valid; }
    bool muted() const { return m_muted; }

private:
    Win32::AudioEndpoint m_endpoint;
    bool m_valid = false;
    bool m_muted = false;
};

// ===========================================================================
//  P2-10 麦克风一键静音（media.mic_mute）
//
//  外在状态 = **真实的麦克风静音状态**（IAudioEndpointVolume::GetMute 读回）
//             + 宿主托盘上的状态徽标（服务桩记录）。
//
//  这个功能最容易犯的错是"自己存一个 bool"：耳机上的麦键、Windows 声音设置、
//  会议软件都会改这个状态，存了就一定会分叉，而分叉的后果是"用户以为闭麦了、
//  其实开着"。所以本用例专门**绕过插件改系统状态**，看它跟不跟得上。
// ===========================================================================

void runMicMuteCase(Reporter &reporter,
                    WinEase::PluginManager &manager,
                    StubServices &services)
{
    WinEase::IFeaturePlugin *plugin = manager.plugin(kMicId);
    reporter.check(plugin != nullptr,
                   QStringLiteral("P2-10 插件已从插件目录加载（media.mic_mute）"));
    if (plugin == nullptr) {
        return;
    }

    // ---- 前置：本机有没有默认采集端点 ----
    QString endpointError;
    Win32::AudioEndpoint mic =
        Win32::AudioEndpoint::defaultEndpoint(Win32::AudioDirection::Capture, &endpointError);
    if (!mic.isValid()) {
        // 没有麦克风**不是失败**：要验的是"插件如实报告不可用"，而不是假装能用
        reporter.info(QStringLiteral("P2-10 前置不成立：本机没有默认麦克风（%1）"
                                     " —— 本用例改为验证「如实报告不可用」")
                          .arg(endpointError));
        reporter.check(!manager.setPluginEnabled(kMicId, true),
                       QStringLiteral("P2-10 没有麦克风时**启用失败**（不假装启用成功）"),
                       plugin->lastError());

        QWidget *settings = plugin->createSettingsWidget(nullptr);
        reporter.check(settings != nullptr,
                       QStringLiteral("P2-10 设置面板在「没有麦克风」时依然能打开"));
        if (settings != nullptr) {
            QLabel *stateLabel = childNamed<QLabel>(settings, kMicStateLabel);
            QPushButton *toggleButton = childNamed<QPushButton>(settings, kMicToggleButton);
            reporter.check(stateLabel != nullptr && toggleButton != nullptr,
                           QStringLiteral("P2-10 面板控件按对象名找到"));
            reporter.check(toggleButton != nullptr && !toggleButton->isEnabled(),
                           QStringLiteral("P2-10 没有麦克风时「静音」按钮被禁用"
                                          "（不给用户一个按了没反应的按钮）"));
            reporter.check(stateLabel != nullptr
                               && !stateLabel->text().contains(QStringLiteral("已静音")),
                           QStringLiteral("P2-10 面板如实说明「读不到麦克风」，不编造状态"),
                           stateLabel != nullptr ? stateLabel->text() : QString());
            delete settings;
        }
        return;
    }

    reporter.info(QStringLiteral("P2-10 麦克风设备：%1").arg(mic.deviceName()));

    MicMuteGuard guard;
    reporter.check(guard.valid(),
                   QStringLiteral("P2-10 前置：能读到麦克风当前的静音状态（否则整段无意义）"));
    if (!guard.valid()) {
        return;
    }

    // ---- 面板：控件、设备名、默认开关 ----
    QWidget *settings = plugin->createSettingsWidget(nullptr);
    reporter.check(settings != nullptr, QStringLiteral("P2-10 设置面板可创建"));
    if (settings == nullptr) {
        return;
    }

    QLabel *stateLabel = childNamed<QLabel>(settings, kMicStateLabel);
    QLabel *deviceLabel = childNamed<QLabel>(settings, kMicDeviceLabel);
    QPushButton *toggleButton = childNamed<QPushButton>(settings, kMicToggleButton);
    QCheckBox *notifyCheck = childNamed<QCheckBox>(settings, kMicNotifyCheck);
    QLabel *hintLabel = childNamed<QLabel>(settings, kMicHintLabel);
    reporter.check(stateLabel != nullptr && deviceLabel != nullptr && toggleButton != nullptr
                       && notifyCheck != nullptr && hintLabel != nullptr,
                   QStringLiteral("P2-10 面板控件按对象名全部找到"));
    if (stateLabel == nullptr || deviceLabel == nullptr || toggleButton == nullptr
        || notifyCheck == nullptr || hintLabel == nullptr) {
        delete settings;
        return;
    }

    reporter.check(deviceLabel->text().contains(mic.deviceName()),
                   QStringLiteral("P2-10 面板显示真实设备名（读的是默认采集端点）"),
                   deviceLabel->text());
    reporter.check(notifyCheck->isChecked(),
                   QStringLiteral("P2-10 「切换时弹气泡提示」默认开启（可关）"));
    reporter.check(hintLabel->text().contains(QStringLiteral("不会自动取消静音")),
                   QStringLiteral("P2-10 面板明说「停用不会自动取消静音」"
                                  "（隐私功能的关键承诺，不能只写在代码注释里）"));

    // ---- 启用：徽标**常驻**（不是按了快捷键才出现）----
    const int badgeCallsBefore = services.trayBadgeCalls();
    reporter.check(manager.setPluginEnabled(kMicId, true),
                   QStringLiteral("P2-10 插件启用成功"), plugin->lastError());

    bool ok = false;
    const bool mutedAfterEnable = readMicMuted(mic, &ok);
    reporter.check(ok, QStringLiteral("P2-10 前置：启用后仍能读到系统状态"));
    reporter.check(services.trayBadgeCalls() > badgeCallsBefore,
                   QStringLiteral("P2-10 一启用就挂上托盘徽标（常驻可见，"
                                  "而不是等用户按快捷键才出现）"));
    reporter.check(services.trayBadgeText(kMicId) == (mutedAfterEnable ? kBadgeMuted : kBadgeLive),
                   QStringLiteral("P2-10 徽标文字 == **系统真实状态**"),
                   QStringLiteral("系统：%1 / 徽标：%2")
                       .arg(mutedAfterEnable ? QStringLiteral("已静音") : QStringLiteral("未静音"),
                            services.trayBadgeText(kMicId).isEmpty()
                                ? QStringLiteral("(空)")
                                : services.trayBadgeText(kMicId)));
    reporter.check(stateLabel->text().contains(mutedAfterEnable ? QStringLiteral("已静音")
                                                                : QStringLiteral("可用")),
                   QStringLiteral("P2-10 面板状态文本 == 系统真实状态"), stateLabel->text());

    // ---- ① 快捷键：系统状态必须**真的**翻转 ----
    const bool expectedFirst = !mutedAfterEnable;
    reporter.check(dispatchAction(manager, kMicId, QStringLiteral("default")),
                   QStringLiteral("P2-10 经宿主分发快捷键（等价于用户按下 Ctrl+Alt+M）"));
    reporter.check(waitFor([&mic, expectedFirst] {
                       bool muted = false;
                       QString error;
                       return mic.muteState(&muted, &error) && muted == expectedFirst;
                   }),
                   QStringLiteral("P2-10 快捷键让**系统**麦克风静音状态真的翻转了"
                                  "（IAudioEndpointVolume 读回）"),
                   QStringLiteral("现在 %1").arg(readMicMuted(mic) ? QStringLiteral("已静音")
                                                                   : QStringLiteral("未静音")));
    reporter.check(services.trayBadgeText(kMicId)
                       == (expectedFirst ? kBadgeMuted : kBadgeLive),
                   QStringLiteral("P2-10 徽标跟着一起变"));
    reporter.check(stateLabel->text().contains(expectedFirst ? QStringLiteral("已静音")
                                                             : QStringLiteral("可用")),
                   QStringLiteral("P2-10 面板跟着一起变"), stateLabel->text());

    // ---- ② 外部改状态：插件必须跟上（这一条专抓"自己存 bool"）----
    const bool external = !expectedFirst;
    reporter.check(mic.setMuted(external),
                   QStringLiteral("P2-10 前置：绕过插件直接改系统状态（模拟耳机上的麦键）"));
    reporter.check(waitFor([&services, external] {
                       return services.trayBadgeText(kMicId)
                              == (external ? kBadgeMuted : kBadgeLive);
                   },
                   4500),
                   QStringLiteral("P2-10 **外部**改了静音状态 → 托盘徽标在轮询周期内跟上"
                                  "（不吃本地缓存）"),
                   QStringLiteral("徽标现在：%1")
                       .arg(services.trayBadgeText(kMicId).isEmpty()
                                ? QStringLiteral("(空)")
                                : services.trayBadgeText(kMicId)));
    reporter.check(stateLabel->text().contains(external ? QStringLiteral("已静音")
                                                        : QStringLiteral("可用")),
                   QStringLiteral("P2-10 外部改动后面板同步刷新（不是只有托盘对）"),
                   stateLabel->text());

    // 外部改完再按一次快捷键：取反的基准必须是**系统当前值**
    reporter.check(dispatchAction(manager, kMicId, QStringLiteral("default")),
                   QStringLiteral("P2-10 外部改动后再按一次快捷键"));
    reporter.check(waitFor([&mic, expectedFirst] {
                       bool muted = false;
                       QString error;
                       return mic.muteState(&muted, &error) && muted == expectedFirst;
                   }),
                   QStringLiteral("P2-10 取反的是**系统当前值**，不是插件记忆里的值"
                                  "（存 bool 的实现会在这里翻错）"));

    // ---- ③ 面板按钮：与快捷键同一条路 ----
    const bool beforeClick = readMicMuted(mic);
    toggleButton->click();
    reporter.check(waitFor([&mic, beforeClick] {
                       bool muted = false;
                       QString error;
                       return mic.muteState(&muted, &error) && muted != beforeClick;
                   }),
                   QStringLiteral("P2-10 点面板按钮同样切换**系统**状态"
                                  "（按钮与快捷键走同一条实现）"));
    reporter.check(stateLabel->text().contains(readMicMuted(mic) ? QStringLiteral("已静音")
                                                                 : QStringLiteral("可用")),
                   QStringLiteral("P2-10 按钮切换后面板文本同步"), stateLabel->text());

    // ---- ④ 关掉气泡提示：不再弹，且配置真的落盘 ----
    notifyCheck->setChecked(false);
    settle(150);
    reporter.check(!services.configValue(kMicId, QStringLiteral("notifyOnToggle"), true).toBool(),
                   QStringLiteral("P2-10 关掉气泡提示后**配置里**也是关（不只是界面上的勾）"));
    const int notificationsBefore = services.notificationCount();
    reporter.check(dispatchAction(manager, kMicId, QStringLiteral("default")),
                   QStringLiteral("P2-10 关掉气泡后再切换一次"));
    settle(300);
    reporter.check(services.notificationCount() == notificationsBefore,
                   QStringLiteral("P2-10 关掉气泡提示后切换不再弹气泡"),
                   QStringLiteral("气泡数 %1 → %2")
                       .arg(notificationsBefore)
                       .arg(services.notificationCount()));

    // ---- ⑤ 面板窗口：这个功能靠托盘 + 快捷键，插件不该自己另开窗 ----
    //  （同名顶层窗口只应该有我们手工创建的那一个）
    int panelWindows = 0;
    for (QWidget *widget : QApplication::topLevelWidgets()) {
        if (widget != nullptr && widget->objectName() == kMicPanelName) {
            ++panelWindows;
        }
    }
    reporter.check(panelWindows == 1,
                   QStringLiteral("P2-10 插件没有额外自己打开面板窗口（只有测试创建的那一个）"),
                   QStringLiteral("同名顶层窗口 %1 个").arg(panelWindows));

    delete settings; // 面板先消失，下面才能断言"停用后不留窗口"
    reporter.check(topLevelNamed(kMicPanelName) == nullptr,
                   QStringLiteral("P2-10 面板销毁后没有残留的同名顶层窗口"));

    // ---- ⑥ 停用：摘徽标 + **不动麦克风**（fail-closed）----
    bool beforeDisableOk = false;
    const bool beforeDisable = readMicMuted(mic, &beforeDisableOk);
    reporter.check(beforeDisableOk, QStringLiteral("P2-10 前置：停用前读到状态"));
    reporter.check(manager.setPluginEnabled(kMicId, false),
                   QStringLiteral("P2-10 插件停用成功"));
    reporter.check(services.trayBadgeText(kMicId).isEmpty(),
                   QStringLiteral("P2-10 停用时摘掉徽标（不留一个没人负责的假状态）"),
                   QStringLiteral("徽标：%1").arg(services.trayBadgeText(kMicId)));
    settle(200);
    bool afterDisableOk = false;
    const bool afterDisable = readMicMuted(mic, &afterDisableOk);
    reporter.check(afterDisableOk && afterDisable == beforeDisable,
                   QStringLiteral("P2-10 停用**不改变**麦克风静音状态"
                                  "（隐私状态 fail-closed：绝不自动开麦）"),
                   QStringLiteral("停用前 %1 → 停用后 %2")
                       .arg(beforeDisable ? QStringLiteral("已静音") : QStringLiteral("未静音"),
                            afterDisable ? QStringLiteral("已静音") : QStringLiteral("未静音")));

    // ---- 收尾：把用户原来的静音状态还回去，并证实系统真的接受了 ----
    reporter.check(mic.setMuted(guard.muted()),
                   QStringLiteral("P2-10 收尾：把麦克风静音状态还原为用例开始时读到的值"));
    reporter.check(waitFor([&mic, &guard] {
                       bool muted = false;
                       QString error;
                       return mic.muteState(&muted, &error) && muted == guard.muted();
                   }),
                   QStringLiteral("P2-10 收尾：还原已被系统接受"));
}

// ===========================================================================
//  P2-09 放大镜增强（display.magnifier）
//
//  外在状态 = **真实的原生放大镜窗口**：
//    * 窗口/子窗口存在性与类名（宿主类 + WC_MAGNIFIER）；
//    * 扩展样式（置顶 / 不进任务栏 / 不抢焦点）；
//    * `MagGetWindowSource` 读回的放大源矩形（倍率的硬证据）；
//    * `GetWindowRgn` 读回的窗口区域（圆角/圆形遮罩真的生效）；
//    * 跟随鼠标后的窗口位置（贴边要翻转到屏幕内）；
//    * 跟随 40 帧的 CPU 时间，与"自抓屏参考实现"当场对照。
//
//  用例会把真实光标挪来挪去（"跟随鼠标"必须真的移动鼠标），结束还原。
// ===========================================================================

void runMagnifierCase(Reporter &reporter, WinEase::PluginManager &manager, StubServices &services)
{
    WinEase::IFeaturePlugin *plugin = manager.plugin(kMagId);
    reporter.check(plugin != nullptr,
                   QStringLiteral("P2-09 插件已从插件目录加载（display.magnifier）"));
    if (plugin == nullptr) {
        return;
    }

    const QPoint savedCursor = Win32::cursorPosition();

    QWidget *settings = plugin->createSettingsWidget(nullptr);
    reporter.check(settings != nullptr, QStringLiteral("P2-09 设置面板可创建"));
    if (settings == nullptr) {
        return;
    }
    QLabel *statusLabel = childNamed<QLabel>(settings, kMagStatusLabel);
    QPushButton *toggleButton = childNamed<QPushButton>(settings, kMagToggleButton);
    QDoubleSpinBox *factorSpin = childNamed<QDoubleSpinBox>(settings, kMagFactorSpin);
    QSpinBox *sizeSpin = childNamed<QSpinBox>(settings, kMagSizeSpin);
    QComboBox *shapeCombo = childNamed<QComboBox>(settings, kMagShapeCombo);
    QLabel *hintLabel = childNamed<QLabel>(settings, kMagHintLabel);
    reporter.check(statusLabel != nullptr && toggleButton != nullptr && factorSpin != nullptr
                       && sizeSpin != nullptr && shapeCombo != nullptr && hintLabel != nullptr,
                   QStringLiteral("P2-09 面板控件按对象名全部找到"));
    if (statusLabel == nullptr || toggleButton == nullptr || factorSpin == nullptr
        || sizeSpin == nullptr || shapeCombo == nullptr || hintLabel == nullptr) {
        delete settings;
        return;
    }

    const auto cleanup = [&] {
        manager.setPluginEnabled(kMagId, false);
        delete settings;
        if (!savedCursor.isNull()) {
            ::SetCursorPos(savedCursor.x(), savedCursor.y());
        }
    };

    // ---- ① 启用只是"待命"：不自动弹窗 ----
    reporter.check(manager.setPluginEnabled(kMagId, true),
                   QStringLiteral("P2-09 插件启用成功"), plugin->lastError());
    settle(250);
    reporter.check(findMagnifierHost() == nullptr,
                   QStringLiteral("P2-09 启用功能**不**自动弹放大窗"
                                  "（启用=待命，显示由快捷键/面板按钮触发）"));

    // ---- ② 面板按钮开始放大：出现的必须是**系统的**放大镜窗口 ----
    toggleButton->click();
    settle(300);
    reporter.info(QStringLiteral("P2-09 点「开始放大」后：按钮可用=%1，面板状态=%2，插件最后错误=%3")
                      .arg(toggleButton->isEnabled() ? QStringLiteral("是") : QStringLiteral("否"),
                           statusLabel->text(),
                           plugin->lastError().isEmpty() ? QStringLiteral("(无)")
                                                         : plugin->lastError()));
    reporter.check(waitFor([] { return findMagnifierHost() != nullptr; }, 2500),
                   QStringLiteral("P2-09 点「开始放大」→ 出现原生放大镜窗口"));
    HWND host = findMagnifierHost();
    if (host == nullptr) {
        reporter.check(false, QStringLiteral("P2-09 前置：放大镜窗口未创建，后续断言无法进行"));
        cleanup();
        return;
    }
    HWND magnifier = magnifierChildOf(host);
    reporter.check(magnifier != nullptr
                       && Win32::windowClassName(magnifier) == QLatin1String("Magnifier"),
                   QStringLiteral("P2-09 放大画面是系统的 WC_MAGNIFIER 子窗口"
                                  "（不是自己抓屏画出来的窗口）"),
                   QStringLiteral("子窗口类名：%1").arg(Win32::windowClassName(magnifier)));
    reporter.check(hasExStyle(host, WS_EX_TOPMOST) && hasExStyle(host, WS_EX_TOOLWINDOW)
                       && hasExStyle(host, WS_EX_NOACTIVATE),
                   QStringLiteral("P2-09 放大窗置顶、不进任务栏、不抢焦点"));
    reporter.check(::GetForegroundWindow() != host,
                   QStringLiteral("P2-09 放大窗没有抢走前台焦点"));
    reporter.check(statusLabel->text().contains(QStringLiteral("跟随鼠标中")),
                   QStringLiteral("P2-09 面板状态显示「跟随鼠标中」"), statusLabel->text());

    // ---- ③ 前置：光标放到屏幕中间，让放大源区完整 ----
    const Win32::MonitorInfo monitor = Win32::monitorForPoint(savedCursor);
    reporter.check(monitor.valid, QStringLiteral("P2-09 前置：能取到光标所在显示器的可用区"));
    if (!monitor.valid) {
        cleanup();
        return;
    }
    const QRect work = monitor.workArea;
    const int sizePx = sizeSpin->value();
    reporter.check(work.width() >= sizePx + 2 * kMagnifierGap
                       && work.height() >= sizePx + 2 * kMagnifierGap,
                   QStringLiteral("P2-09 前置：显示器可用区容得下放大窗 + 间隙"
                                  "（否则「不盖住光标」验不了）"),
                   QStringLiteral("可用区 %1，窗口 %2 px").arg(rectText(work)).arg(sizePx));

    reporter.check(moveCursorTo(work.center()),
                   QStringLiteral("P2-09 前置：光标已移到屏幕中间"));
    settle(250);

    // ---- ④ 倍率真的生效：源矩形 = 窗口 / 倍率（API 自己报的）----
    const double factor = factorSpin->value();
    const QRect source = readMagnifierSource(magnifier);
    const int expectedSource = qRound(static_cast<double>(sizePx) / factor);
    reporter.check(!source.isEmpty() && qAbs(source.width() - expectedSource) <= 1
                       && qAbs(source.height() - expectedSource) <= 1,
                   QStringLiteral("P2-09 放大源矩形边长 == 窗口边长 / 倍率"
                                  "（MagGetWindowSource 读回，倍率真的生效）"),
                   QStringLiteral("窗口 %1 px、倍率 ×%2 → 源区 %3×%4（期望 %5）")
                       .arg(sizePx)
                       .arg(factor)
                       .arg(source.width())
                       .arg(source.height())
                       .arg(expectedSource));
    const QPoint cursorAtCenter = Win32::cursorPosition();
    reporter.check(qAbs(source.center().x() - cursorAtCenter.x()) <= 2
                       && qAbs(source.center().y() - cursorAtCenter.y()) <= 2,
                   QStringLiteral("P2-09 放大源区中心就是光标位置（放大的是光标底下那块）"),
                   QStringLiteral("源区 %1 / 光标 %2,%3")
                       .arg(rectText(source))
                       .arg(cursorAtCenter.x())
                       .arg(cursorAtCenter.y()));

    // ---- ⑤ 跟随：移动光标 → 窗口跟着走，且不盖住光标 ----
    const QRect centerRect = windowRectOf(host);
    const QPoint targetA(work.left() + work.width() / 4, work.top() + work.height() / 4);
    reporter.check(moveCursorTo(targetA), QStringLiteral("P2-09 前置：光标移到可用区左上 1/4 处"));
    reporter.check(waitFor([&host, targetA] {
                       const QRect rect = windowRectOf(host);
                       return !rect.isEmpty() && qAbs(rect.left() - (targetA.x() + kMagnifierGap)) <= 2
                              && qAbs(rect.top() - (targetA.y() + kMagnifierGap)) <= 2;
                   },
                   1500),
                   QStringLiteral("P2-09 光标一动，放大窗就跟着走（默认停在右下侧 %1 px 处）")
                       .arg(kMagnifierGap),
                   QStringLiteral("窗口 %1 / 期望左上 %2,%3")
                       .arg(rectText(windowRectOf(host)))
                       .arg(targetA.x() + kMagnifierGap)
                       .arg(targetA.y() + kMagnifierGap));
    reporter.check(windowRectOf(host) != centerRect,
                   QStringLiteral("P2-09 跟随是真的移动了窗口（不是原地不动）"));
    {
        const QRect current = windowRectOf(host);
        const QPoint cursorNow = Win32::cursorPosition();
        reporter.check(work.contains(current) && !current.contains(cursorNow),
                       QStringLiteral("P2-09 放大窗整块留在可用区内、且**不盖住光标**"),
                       QStringLiteral("窗口 %1 / 光标 %2,%3 / 可用区 %4")
                           .arg(rectText(current))
                           .arg(cursorNow.x())
                           .arg(cursorNow.y())
                           .arg(rectText(work)));
    }

    // ---- ⑥ 靠边翻转 ----
    const QPoint corner(work.right() - 4, work.bottom() - 4);
    reporter.check(moveCursorTo(corner),
                   QStringLiteral("P2-09 前置：光标移到可用区右下角"));
    settle(300);
    {
        const QRect cornerRect = windowRectOf(host);
        const QPoint cornerCursor = Win32::cursorPosition();
        reporter.check(work.contains(cornerRect),
                       QStringLiteral("P2-09 光标贴右下角 → 放大窗自动翻转/夹取，仍整块在可用区内"),
                       QStringLiteral("窗口 %1 / 可用区 %2").arg(rectText(cornerRect), rectText(work)));
        reporter.check(cornerRect.right() < cornerCursor.x()
                           && cornerRect.bottom() < cornerCursor.y(),
                       QStringLiteral("P2-09 贴角时放大窗翻到了光标的左上侧（不盖住要看的那块）"),
                       QStringLiteral("窗口 %1 / 光标 %2,%3")
                           .arg(rectText(cornerRect))
                           .arg(cornerCursor.x())
                           .arg(cornerCursor.y()));
    }

    // ---- ⑦ 面板改倍率 → API 的源矩形立刻按新倍率变化 ----
    reporter.check(moveCursorTo(work.center()), QStringLiteral("P2-09 光标回到屏幕中间"));
    settle(250);
    factorSpin->setValue(4.0);
    settle(300);
    {
        const QRect sourceAt4 = readMagnifierSource(magnifier);
        const int expectedAt4 = qRound(static_cast<double>(sizePx) / 4.0);
        reporter.check(qAbs(sourceAt4.width() - expectedAt4) <= 1,
                       QStringLiteral("P2-09 面板把倍率改成 ×4 → 源矩形边长立刻变成 窗口/4（API 读回）"),
                       QStringLiteral("源区 %1×%2（期望边长 %3）")
                           .arg(sourceAt4.width())
                           .arg(sourceAt4.height())
                           .arg(expectedAt4));
    }
    reporter.check(qFuzzyCompare(services.configValue(kMagId, QStringLiteral("factor"), 0.0).toDouble(),
                                 4.0),
                   QStringLiteral("P2-09 倍率改动落进了配置（下次启动还记得）"));

    // ---- ⑧ 遮罩形状：三种都要真的生效（看窗口区域）----
    const int circleIndex = shapeCombo->findData(QStringLiteral("circle"));
    reporter.check(circleIndex >= 0, QStringLiteral("P2-09 面板有「圆形」遮罩选项"));
    shapeCombo->setCurrentIndex(circleIndex);
    settle(300);
    {
        const int half = sizePx / 2;
        HRGN region = ::CreateRectRgn(0, 0, 1, 1);
        const int type = ::GetWindowRgn(host, region);
        reporter.check(type != ERROR,
                       QStringLiteral("P2-09 切「圆形」→ 宿主窗口有了窗口区域（遮罩生效）"));
        reporter.check(::PtInRegion(region, half, half) == TRUE
                           && ::PtInRegion(region, 2, 2) == FALSE
                           && ::PtInRegion(region, sizePx - 2, 2) == FALSE,
                       QStringLiteral("P2-09 「圆形」遮罩：中心在区域内、四角在区域外"
                                      "（真是圆形，不是矩形）"),
                       QStringLiteral("区域类型 %1").arg(type));
        ::DeleteObject(region);
    }

    const int rectIndex = shapeCombo->findData(QStringLiteral("rect"));
    reporter.check(rectIndex >= 0, QStringLiteral("P2-09 面板有「矩形」遮罩选项"));
    shapeCombo->setCurrentIndex(rectIndex);
    settle(300);
    reporter.check(windowRegionType(host) == ERROR,
                   QStringLiteral("P2-09 切「矩形」→ 窗口区域被去掉（整块矩形）"));

    const int roundedIndex = shapeCombo->findData(QStringLiteral("rounded"));
    reporter.check(roundedIndex >= 0, QStringLiteral("P2-09 面板有「圆角」遮罩选项"));
    shapeCombo->setCurrentIndex(roundedIndex);
    settle(300);
    {
        HRGN region = ::CreateRectRgn(0, 0, 1, 1);
        const int type = ::GetWindowRgn(host, region);
        reporter.check(type != ERROR && ::PtInRegion(region, sizePx / 2, 2) == TRUE
                           && ::PtInRegion(region, 2, 2) == FALSE,
                       QStringLiteral("P2-09 「圆角」遮罩：角被切掉、上边中点在区域内"));
        ::DeleteObject(region);
    }

    // ---- ⑨ 快捷键开关（与面板按钮同一个动作）----
    reporter.check(dispatchAction(manager, kMagId, QStringLiteral("default")),
                   QStringLiteral("P2-09 经宿主分发快捷键（Ctrl+Alt+Z）"));
    reporter.check(waitFor([] { return findMagnifierHost() == nullptr; }, 2000),
                   QStringLiteral("P2-09 快捷键关掉放大镜 → 原生窗口被**销毁**（不是藏起来）"));
    reporter.check(dispatchAction(manager, kMagId, QStringLiteral("default"))
                       && waitFor([] { return findMagnifierHost() != nullptr; }, 2500),
                   QStringLiteral("P2-09 再按一次快捷键 → 放大镜重新出现（开关是同一个动作）"));
    host = findMagnifierHost();
    magnifier = magnifierChildOf(host);
    reporter.check(magnifier != nullptr,
                   QStringLiteral("P2-09 重建后的窗口依然是 WC_MAGNIFIER 子窗口"));

    // ---- ⑩ CPU：与"自抓屏方案"当场对照 ----
    reporter.check(moveCursorTo(work.center()),
                   QStringLiteral("P2-09 前置：光标回到屏幕中间（两侧测同一个源区域）"));
    settle(250);
    const QRect cpuSource = readMagnifierSource(magnifier);
    if (cpuSource.isEmpty()) {
        reporter.check(false, QStringLiteral("P2-09 CPU 对照：读不到放大源矩形，无法对照"));
    } else {
        constexpr int kFrames = 40;
        constexpr int kFrameMs = 30;
        const int durationMs = kFrames * kFrameMs;

        // ① 空转基线：把放大镜关掉，同样时长里光是事件循环要花多少 CPU（先扣掉它）
        dispatchAction(manager, kMagId, QStringLiteral("default"));
        reporter.check(waitFor([] { return findMagnifierHost() == nullptr; }, 2000),
                       QStringLiteral("P2-09 CPU 对照：先关掉放大镜测空转基线"));
        const qint64 idleMsBefore = processCpuTimeMs();
        const qint64 idleCyclesBefore = processCpuCycles();
        settle(durationMs);
        const qint64 idleMs = processCpuTimeMs() - idleMsBefore;
        const qint64 idleCycles = processCpuCycles() - idleCyclesBefore;

        // ② 打开放大镜，光标**不动**：源区与窗口位置都没变
        dispatchAction(manager, kMagId, QStringLiteral("default"));
        reporter.check(waitFor([] { return findMagnifierHost() != nullptr; }, 2500),
                       QStringLiteral("P2-09 CPU 对照：重新打开放大镜"));
        const qint64 stillMsBefore = processCpuTimeMs();
        const qint64 stillCyclesBefore = processCpuCycles();
        settle(durationMs);
        const qint64 stillMs = processCpuTimeMs() - stillMsBefore;
        const qint64 stillCycles = processCpuCycles() - stillCyclesBefore;

        // ③ 真实跟随：光标**每帧都在动**（双方都干满活的那一档，才是可比的对照）
        const qint64 followMsBefore = processCpuTimeMs();
        const qint64 followCyclesBefore = processCpuCycles();
        moveCursorWhileSettling(kFrames, kFrameMs, work.center());
        const qint64 followMs = processCpuTimeMs() - followMsBefore;
        const qint64 followCycles = processCpuCycles() - followCyclesBefore;

        // ④ 自抓屏参考实现：同一个源区域、同一帧率，抓屏 → 缩放 → **呈现到真实窗口**
        const CpuCost reference =
            measureSelfCaptureCpu(kFrames, kFrameMs, cpuSource, QSize(sizePx, sizePx));
        const qint64 stillNetCycles = qMax<qint64>(0, stillCycles - idleCycles);
        const qint64 followNetCycles = qMax<qint64>(0, followCycles - idleCycles);
        const qint64 followNetMs = qMax<qint64>(0, followMs - idleMs);

        reporter.info(QStringLiteral("P2-09 CPU 对照（%1 帧 / %2 ms）：空转基线 %3 周期；"
                                     "放大镜静止 %4 ms / %5 周期；放大镜跟随移动 %6 ms / %7 周期；"
                                     "自抓屏参考实现（含呈现）%8 ms / %9 周期")
                          .arg(kFrames)
                          .arg(durationMs)
                          .arg(idleCycles)
                          .arg(stillMs)
                          .arg(stillCycles)
                          .arg(followMs)
                          .arg(followCycles)
                          .arg(reference.ms)
                          .arg(reference.cycles));
        reporter.info(QStringLiteral("P2-09 扣掉空转基线后：静止 %1 周期、跟随 %2 周期（%3 ms）"
                                     "；自抓屏参考 %4 周期（跟随/自抓屏 = %5%）")
                          .arg(stillNetCycles)
                          .arg(followNetCycles)
                          .arg(followNetMs)
                          .arg(reference.cycles)
                          .arg(reference.cycles > 0
                                   ? QString::number(100.0 * static_cast<double>(followNetCycles)
                                                         / static_cast<double>(reference.cycles),
                                                     'f', 1)
                                   : QStringLiteral("?")));
        reporter.check(reference.cycles > 0 && stillNetCycles >= 0 && followNetCycles >= 0,
                       QStringLiteral("P2-09 CPU 对照：两侧都测到了 CPU 周期数"));
        // 这一条断言的是**我们自己的优化**：源区与窗口位置都没变时，不重复把同样的
        // 源矩形交给 API（每 30ms 无条件重设一次是纯浪费）。它同时也是"静止时几乎不做事"的凭证。
        reporter.check(stillNetCycles < followNetCycles,
                       QStringLiteral("P2-09 光标不动时的 CPU 成本**明显低于**跟随移动时"
                                      "（源区/位置没变就不重复设源区，不做无意义的重设）"),
                       QStringLiteral("静止 %1 周期 / 跟随 %2 周期")
                           .arg(stillNetCycles)
                           .arg(followNetCycles));
        // 这一条**不**宣称"比自抓屏更省"：实测静止档与参考实现同量级（略高），
        // 跟随档更高（多出来的是移窗 + 改源区 + 移动真实光标）。断言的只是"同量级"，
        // 并把真实结论写进 INFO —— 原验收标准"CPU 明显低于自抓屏"**不成立**，如实记在 ROADMAP。
        reporter.check(stillNetCycles < reference.cycles * 2,
                       QStringLiteral("P2-09 静止档（只保持画面新鲜）的 CPU 成本与自抓屏参考实现"
                                      "**同量级**（不宣称更低：实测略高，见下方 INFO）"),
                       QStringLiteral("静止 %1 周期 / 自抓屏 %2 周期")
                           .arg(stillNetCycles)
                           .arg(reference.cycles));
        reporter.info(QStringLiteral("P2-09 CPU 结论：「把放大交给系统所以 CPU 更低」**不成立**"
                                     " —— 取像与缩放仍在放大镜控件里跑（本进程），绝对成本与自抓屏同量级；"
                                     "API 的真实收益是**无撕裂、无抓屏停顿、系统级正确**"
                                     "（硬件叠加/UWP/多 DPI/多显示器），以及「没变化就不重设源区」这一档省法"));
        // ⑤ 把周期数翻译成"占单核百分之几"：分母用"单核跑满"的实测尺子
        const double coreCyclesPerMs = fullCoreCyclesPerMs(300);
        const double coreCyclesInWindow = coreCyclesPerMs * static_cast<double>(durationMs);
        const double stillPercent =
            coreCyclesInWindow > 0.0 ? 100.0 * stillNetCycles / coreCyclesInWindow : -1.0;
        const double followPercent =
            coreCyclesInWindow > 0.0 ? 100.0 * followNetCycles / coreCyclesInWindow : -1.0;
        reporter.info(QStringLiteral("P2-09 折算成单核占用：静止 %1%、跟随 %2%"
                                     "（分母 = 实测「单核跑满」%3 周期/ms；"
                                     "跟随档还包含每帧移动真实光标本身触发的全局鼠标钩子回调）")
                          .arg(stillPercent, 0, 'f', 1)
                          .arg(followPercent, 0, 'f', 1)
                          .arg(coreCyclesPerMs, 0, 'f', 0));
        // 阈值按实测留 ~4 倍余量（实测静止 1.0% / 跟随 3.6%）：真要劣化 4 倍就该红
        reporter.check(stillPercent >= 0.0 && stillPercent < 5.0,
                       QStringLiteral("P2-09 静止时（光标不动）CPU 占用 < 单核 5%"),
                       QStringLiteral("%1%").arg(stillPercent, 0, 'f', 1));
        reporter.check(followPercent >= 0.0 && followPercent < 15.0,
                       QStringLiteral("P2-09 跟随放大时 CPU 占用 < 单核 15%"
                                      "（分母是实测的「单核跑满」，不用 15.6ms 分辨率的毫秒）"),
                       QStringLiteral("%1%").arg(followPercent, 0, 'f', 1));
    }

    // ---- ⑪ 停用：窗口必须被销毁 ----
    reporter.check(manager.setPluginEnabled(kMagId, false),
                   QStringLiteral("P2-09 插件停用成功"));
    reporter.check(findMagnifierHost() == nullptr,
                   QStringLiteral("P2-09 停用 → 放大镜窗口被销毁（「可完整还原」落在停用上）"));

    delete settings;
    if (!savedCursor.isNull()) {
        ::SetCursorPos(savedCursor.x(), savedCursor.y());
        reporter.info(QStringLiteral("P2-09 收尾：真实光标已还原到 %1,%2")
                          .arg(savedCursor.x())
                          .arg(savedCursor.y()));
    }
}

// ===========================================================================
//  P2-08 亮度 / 色温 / 护眼（display.brightness）
//
//  这一项改的是**用户眼前的屏幕**，所以外在状态必须直接问系统，而不是看插件报了什么：
//    * 色温 → `GetDeviceGammaRamp` **逐元素**读回真实的 gamma ramp，
//             与"原始 ramp × 色温增益"逐个元素比对（±1，取整损耗）；
//    * 亮度 → 问 WMI / DDC/CI 要**当前亮度真值**（不是我们刚写进去的参数）；
//    * 停用 → 再读一次 gamma，断言它与**启用前**逐元素相同（可完整还原的硬证据）。
//
//  关于"层层叠加"这个最容易犯的错：每次调节都必须基于**启用时那一份原始 ramp**
//  重算，而不是在上一次结果上再乘一次。证据就是"4000K 的结果 == 原始 ramp × 4000K 增益"——
//  若实现是叠出来的，这里会读到 5000K × 4000K 的双重衰减，蓝通道会明显偏低。
//
//  护眼模式的时段判断**不靠等时间**：把"夜间时段"设成覆盖/不覆盖当前时刻，
//  改完立刻生效 —— 这正是用户改设置时的真实路径（也是唯一能在自检里稳定复现的路径）。
//
//  ⚠ 本用例会**真的改变屏幕亮度与色温**（这就是产品行为本身，躲不开）：
//    gamma 由插件在停用时自行还原，亮度由用例在收尾时写回用户原值并读回确认。
// ===========================================================================

/// 读回的 ramp 是否逐元素等于「基准 ramp × 该色温的通道增益」
bool rampMatchesGains(const Win32::GammaSnapshot &actual,
                      const Win32::GammaSnapshot &baseline,
                      int kelvin,
                      int tolerance,
                      QString *detailOut)
{
    if (!actual.valid || !baseline.valid || actual.ramp.size() != 768 || baseline.ramp.size() != 768) {
        if (detailOut != nullptr) {
            *detailOut = QStringLiteral("ramp 无效或长度不是 768");
        }
        return false;
    }

    double gains[3] = { 1.0, 1.0, 1.0 };
    Win32::colorTemperatureGains(kelvin, &gains[0], &gains[1], &gains[2]);

    int worst = 0;
    int worstIndex = -1;
    for (int index = 0; index < 768; ++index) {
        const double expected = std::min(65535.0,
                                         static_cast<double>(baseline.ramp.at(index))
                                             * gains[index / 256]);
        const int diff = std::abs(static_cast<int>(std::lround(expected))
                                  - static_cast<int>(actual.ramp.at(index)));
        if (diff > worst) {
            worst = diff;
            worstIndex = index;
        }
    }

    if (detailOut != nullptr) {
        *detailOut = QStringLiteral("最大偏差 %1（通道 %2 第 %3 级；该通道增益 %4）")
                         .arg(worst)
                         .arg(worstIndex / 256)
                         .arg(worstIndex % 256)
                         .arg(gains[worstIndex / 256], 0, 'f', 3);
    }
    return worst <= tolerance;
}

/// ramp 逐元素比较，返回首个不同的位置（相同返回 -1）
int firstRampDifference(const QList<quint16> &left, const QList<quint16> &right)
{
    if (left.size() != right.size()) {
        return 0;
    }
    for (int index = 0; index < left.size(); ++index) {
        if (left.at(index) != right.at(index)) {
            return index;
        }
    }
    return -1;
}

void runBrightnessCase(Reporter &reporter, WinEase::PluginManager &manager, StubServices &services)
{
    Q_UNUSED(services)

    WinEase::IFeaturePlugin *plugin = manager.plugin(kBriId);
    reporter.check(plugin != nullptr,
                   QStringLiteral("P2-08 插件已从插件目录加载（display.brightness）"));
    if (plugin == nullptr) {
        return;
    }

    // ---- 前置 A：真实的 gamma ramp（色温那半段的唯一真相来源）----
    const Win32::GammaSnapshot baseline = Win32::captureGamma(0);
    reporter.check(baseline.valid,
                   QStringLiteral("P2-08 前置：能从系统读回真实的 gamma ramp"
                                  "（否则色温那半段无意义）"),
                   baseline.error);

    // ---- 前置 B：亮度通路（逐级回退的结果）----
    const Win32::BrightnessChannel channel = Win32::resolveBrightnessChannel(0);
    reporter.info(QStringLiteral("P2-08 亮度通路：后端=%1 · 目标=%2 · 详情=%3")
                      .arg(Win32::brightnessBackendText(channel.backend),
                           channel.target,
                           channel.detail));
    if (!channel.ddcError.isEmpty()) {
        reporter.info(QStringLiteral("P2-08 DDC/CI 侧的原因：%1").arg(channel.ddcError));
    }
    if (!channel.wmiError.isEmpty()) {
        reporter.info(QStringLiteral("P2-08 WMI 侧的原因：%1").arg(channel.wmiError));
    }

    if (channel.backend == Win32::BrightnessBackend::WmiInternal) {
        reporter.check(!channel.ddcError.isEmpty(),
                       QStringLiteral("P2-08 逐级回退：DDC/CI 不通 → 回退到 WMI 内置屏，"
                                      "且**留下**了失败原因（用户排障要看这个）"),
                       channel.ddcError);
    }
    if (!channel.supported) {
        reporter.info(QStringLiteral("P2-08 前置不成立：本机没有可用的亮度通路"
                                     "（虚拟机显卡 / 远程桌面 / 纯外接屏机型都属正常）"
                                     " —— 亮度那半段改为断言「面板如实禁用」，色温照常验证"));
    }

    QWidget *settings = plugin->createSettingsWidget(nullptr);
    reporter.check(settings != nullptr, QStringLiteral("P2-08 设置面板可创建"));
    if (settings == nullptr) {
        return;
    }

    auto *statusLabel = childNamed<QLabel>(settings, kBriStatusLabel);
    auto *channelLabel = childNamed<QLabel>(settings, kBriChannelLabel);
    auto *scheduleLabel = childNamed<QLabel>(settings, kBriScheduleLabel);
    auto *previewLabel = childNamed<QLabel>(settings, kBriPreviewLabel);
    auto *brightnessSlider = childNamed<QSlider>(settings, kBriBrightnessSlider);
    auto *brightnessSpin = childNamed<QSpinBox>(settings, kBriBrightnessSpin);
    auto *kelvinSlider = childNamed<QSlider>(settings, kBriKelvinSlider);
    auto *kelvinSpin = childNamed<QSpinBox>(settings, kBriKelvinSpin);
    auto *eyeCareCheck = childNamed<QCheckBox>(settings, kBriEyeCareCheck);
    auto *dayKelvinSpin = childNamed<QSpinBox>(settings, kBriDayKelvinSpin);
    auto *nightKelvinSpin = childNamed<QSpinBox>(settings, kBriNightKelvinSpin);
    auto *nightStartEdit = childNamed<QTimeEdit>(settings, kBriNightStartEdit);
    auto *nightEndEdit = childNamed<QTimeEdit>(settings, kBriNightEndEdit);

    reporter.check(statusLabel != nullptr && channelLabel != nullptr && scheduleLabel != nullptr
                       && previewLabel != nullptr && brightnessSlider != nullptr
                       && brightnessSpin != nullptr && kelvinSlider != nullptr
                       && kelvinSpin != nullptr && eyeCareCheck != nullptr
                       && dayKelvinSpin != nullptr && nightKelvinSpin != nullptr
                       && nightStartEdit != nullptr && nightEndEdit != nullptr,
                   QStringLiteral("P2-08 面板控件按对象名全部找到"));
    if (statusLabel == nullptr || channelLabel == nullptr || scheduleLabel == nullptr
        || previewLabel == nullptr || brightnessSlider == nullptr || brightnessSpin == nullptr
        || kelvinSlider == nullptr || kelvinSpin == nullptr || eyeCareCheck == nullptr
        || dayKelvinSpin == nullptr || nightKelvinSpin == nullptr || nightStartEdit == nullptr
        || nightEndEdit == nullptr) {
        delete settings;
        return;
    }

    // 用户的屏幕亮度是他自己调出来的，用例结束必须写回原值
    const bool hasBrightness = channel.supported;
    const double originalBrightness = hasBrightness ? channel.percent : -1.0;
    const auto restoreBrightness = [&]() {
        if (!hasBrightness) {
            return;
        }
        QString error;
        WinEase::Win32::setBrightnessPercentOn(channel, originalBrightness, &error);
    };
    const auto cleanup = [&]() {
        manager.setPluginEnabled(kBriId, false);
        restoreBrightness();
        delete settings;
    };

    // ---- ① 启用：配置里的色温立即生效（配置真的被读走）----
    reporter.check(manager.setPluginEnabled(kBriId, true),
                   QStringLiteral("P2-08 插件启用成功"), plugin->lastError());
    settle(300);

    reporter.check(std::abs(kelvinSpin->value() - kPresetKelvin) <= 1,
                   QStringLiteral("P2-08 面板上的色温是配置里的 %1K（不是代码里的默认值）")
                       .arg(kPresetKelvin),
                   QStringLiteral("面板显示 %1K").arg(kelvinSpin->value()));

    QString detail;
    const Win32::GammaSnapshot warm = Win32::captureGamma(0);
    reporter.check(rampMatchesGains(warm, baseline, kPresetKelvin, 1, &detail),
                   QStringLiteral("P2-08 启用即按 %1K 生效：读回的 gamma 逐元素等于"
                                  "「原始 ramp × %1K 增益」")
                       .arg(kPresetKelvin),
                   detail);

    // ---- ② 面板改色温：必须基于**同一份原始基准**重算 ----
    kelvinSpin->setValue(4000);
    settle(250);
    const Win32::GammaSnapshot colder = Win32::captureGamma(0);
    reporter.check(rampMatchesGains(colder, baseline, 4000, 1, &detail),
                   QStringLiteral("P2-08 面板改 4000K：结果基于同一份原始基准重算"
                                  "（不是在上一次结果上再乘一次）"),
                   detail);

    // ---- ③ 6500K = 不调色：逐元素还原 ----
    kelvinSpin->setValue(kNeutralKelvin);
    settle(250);
    const Win32::GammaSnapshot neutral = Win32::captureGamma(0);
    reporter.check(neutral.valid && firstRampDifference(neutral.ramp, baseline.ramp) < 0,
                   QStringLiteral("P2-08 拖回 6500K「不调色」→ ramp 逐元素等于原始值"
                                  "（不是套一组近似 1.0 的增益）"),
                   QStringLiteral("首个不同项：%1（-1 表示完全相同）")
                       .arg(firstRampDifference(neutral.ramp, baseline.ramp)));

    // ---- ③.5 能力探测：本机的 gamma 到底能调多暖 ----
    //  ⚠ Windows 会**静默拒绝**幅度过大的 ramp（`SetDeviceGammaRamp` 返回 FALSE，
    //    但 `GetLastError()` 是 0）：实测本机 4000K 写得进去、3200K 被拒。
    //    所以"夜间色温"不能随便写死一个理想值 —— 先探测出**本机真实的可写范围**，
    //    再拿实测出来的档位做断言（否则在任何一台限制更严的机器上都会假失败）。
    int warmestWritable = kNeutralKelvin;
    int warmestRejected = -1;
    QString rejectReason;
    for (int kelvin = kNeutralKelvin; kelvin >= 2500; kelvin -= 250) {
        QString error;
        const bool ok = (kelvin == kNeutralKelvin)
                            ? Win32::restoreGamma(baseline, &error)
                            : Win32::applyColorTemperature(0, kelvin, baseline, &error);
        if (!ok) {
            warmestRejected = kelvin; // 幅度越大越容易被拒 → 第一个失败就是下限
            rejectReason = error;
            break;
        }
        warmestWritable = kelvin;
    }
    WinEase::Win32::restoreGamma(baseline);
    settle(150);
    reporter.info(QStringLiteral("P2-08 能力探测：本机 gamma 能写到 %1K；%2")
                      .arg(warmestWritable)
                      .arg(warmestRejected >= 0
                               ? QStringLiteral("再暖的 %1K 被系统拒绝（%2）")
                                     .arg(warmestRejected)
                                     .arg(rejectReason)
                               : QStringLiteral("一路试到 2500K 都被接受")));
    reporter.check(warmestWritable < kNeutralKelvin,
                   QStringLiteral("P2-08 前置：本机能写进至少一档暖色温"
                                  "（否则护眼模式那半段无意义）"),
                   rejectReason);

    // ---- ④ 护眼模式：夜间时段覆盖当前时刻 → 立刻切到夜间色温 ----
    eyeCareCheck->setChecked(true);
    settle(150);
    reporter.check(!kelvinSpin->isEnabled(),
                   QStringLiteral("P2-08 护眼开启后手动色温控件让位（色温由时段接管，"
                                  "避免「拖了没反应」）"));

    dayKelvinSpin->setValue(kNeutralKelvin); // 日间 = 不调色，日间那一条断言才是干净的
    if (warmestWritable < kNeutralKelvin) {
        nightKelvinSpin->setValue(warmestWritable);
    }
    const QTime now = QTime::currentTime();
    // [now-1h, now+1h) 一定包含当前时刻（跨午夜时插件按"跨夜区间"处理）
    nightStartEdit->setTime(now.addSecs(-3600));
    nightEndEdit->setTime(now.addSecs(3600));
    settle(300);

    if (warmestWritable < kNeutralKelvin) {
        const Win32::GammaSnapshot night = Win32::captureGamma(0);
        reporter.check(rampMatchesGains(night, baseline, warmestWritable, 1, &detail),
                       QStringLiteral("P2-08 夜间时段覆盖当前时刻 → 自动切到夜间色温 %1K"
                                      "（本机实测可写的最暖档）")
                           .arg(warmestWritable),
                       detail);
    }
    reporter.check(scheduleLabel->text().contains(QStringLiteral("夜间")),
                   QStringLiteral("P2-08 状态栏如实说明当前处于夜间时段"),
                   scheduleLabel->text());

    // ---- ④.5 系统拒绝的档位：必须如实报错，且不能留下半套滤镜 ----
    if (warmestRejected >= 0 && warmestWritable < kNeutralKelvin) {
        const Win32::GammaSnapshot beforeReject = Win32::captureGamma(0);
        nightKelvinSpin->setValue(warmestRejected);
        settle(300);

        reporter.check(!plugin->lastError().isEmpty(),
                       QStringLiteral("P2-08 系统拒绝 %1K 时插件**如实报错**"
                                      "（不谎报成功、也不偷偷换成别的色温）")
                           .arg(warmestRejected),
                       plugin->lastError());

        const Win32::GammaSnapshot afterReject = Win32::captureGamma(0);
        reporter.check(afterReject.valid
                           && firstRampDifference(afterReject.ramp, beforeReject.ramp) < 0,
                       QStringLiteral("P2-08 被拒绝的色温没有留下半套滤镜"
                                      "（屏幕保持在上一份状态）"),
                       QStringLiteral("首个不同项：%1")
                           .arg(firstRampDifference(afterReject.ramp, beforeReject.ramp)));

        nightKelvinSpin->setValue(warmestWritable); // 回到可写档，后面的断言才有干净前置
        settle(250);
    } else if (warmestRejected < 0) {
        reporter.info(QStringLiteral("P2-08 本机接受了所有试过的色温，"
                                     "「系统拒绝时如实报错」这条无从触发（如实跳过）"));
    }

    // ---- ⑤ 时段不含当前时刻 → 回到日间色温 ----
    // [now+1h, now+2h) 一定**不**包含当前时刻（无论是否跨午夜）
    nightStartEdit->setTime(now.addSecs(3600));
    nightEndEdit->setTime(now.addSecs(7200));
    settle(300);

    const Win32::GammaSnapshot daytime = Win32::captureGamma(0);
    reporter.check(daytime.valid && firstRampDifference(daytime.ramp, baseline.ramp) < 0,
                   QStringLiteral("P2-08 时段不含当前时刻 → 回到日间色温 6500K（ramp 逐元素还原）"),
                   QStringLiteral("首个不同项：%1")
                       .arg(firstRampDifference(daytime.ramp, baseline.ramp)));
    reporter.check(scheduleLabel->text().contains(QStringLiteral("日间")),
                   QStringLiteral("P2-08 状态栏切到「日间时段」"), scheduleLabel->text());

    // ---- ⑥ 亮度：面板上的数字是**真值**，写进去要能在系统里读回来 ----
    double externalBrightness = -1.0;
    if (hasBrightness) {
        const double target = originalBrightness <= 50.0 ? std::min(90.0, originalBrightness + 25.0)
                                                         : std::max(10.0, originalBrightness - 25.0);
        reporter.check(std::abs(target - originalBrightness) >= 20.0,
                       QStringLiteral("P2-08 前置：亮度目标与原值差距 ≥20%"
                                      "（否则「真的变了」这条断言没有意义）"),
                       QStringLiteral("原值 %1% → 目标 %2%")
                           .arg(originalBrightness, 0, 'f', 0)
                           .arg(target, 0, 'f', 0));

        brightnessSpin->setValue(qRound(target));
        settle(400); // 面板有 150ms 防抖：拖滑块不该产生几十次硬件写入

        const bool reached = waitFor(
            [&channel, target] {
                double actual = 0.0;
                return Win32::readBrightnessPercent(channel, &actual)
                       && std::abs(actual - target) <= 2.0;
            },
            6000);

        double readback = -1.0;
        WinEase::Win32::readBrightnessPercent(channel, &readback);
        reporter.check(reached,
                       QStringLiteral("P2-08 面板设亮度 → 系统亮度真的变了（读回确认真值）"),
                       QStringLiteral("目标 %1% / 系统读回 %2%")
                           .arg(target, 0, 'f', 0)
                           .arg(readback, 0, 'f', 0));

        const int shown = firstNumberAfter(statusLabel->text(), QStringLiteral("亮度 "));
        reporter.check(std::abs(shown - qRound(readback)) <= 1,
                       QStringLiteral("P2-08 面板显示的是刚读回来的真值（不是我们记的值）"),
                       statusLabel->text());

        // ---- ⑦ 绕开插件直接改亮度（等价于用户按 Fn 键 / 拖系统托盘滑块）----
        externalBrightness = target <= 50.0 ? std::min(90.0, target + 20.0)
                                            : std::max(10.0, target - 20.0);
        QString externalError;
        const bool externalWritten =
            WinEase::Win32::setBrightnessPercentOn(channel, externalBrightness, &externalError);
        reporter.check(externalWritten,
                       QStringLiteral("P2-08 前置：绕开插件直接改系统亮度（模拟 Fn 键 / 系统托盘）"),
                       externalError);

        const bool followed = waitFor(
            [statusLabel, externalBrightness] {
                const int value = firstNumberAfter(statusLabel->text(), QStringLiteral("亮度 "));
                return std::abs(value - qRound(externalBrightness)) <= 1;
            },
            9000);
        reporter.check(followed,
                       QStringLiteral("P2-08 外部改了亮度 → 面板 5 秒内跟上"
                                      "（插件不自己记一份亮度状态）"),
                       QStringLiteral("外部改为 %1% 后面板显示：%2")
                           .arg(externalBrightness, 0, 'f', 0)
                           .arg(statusLabel->text()));
    } else {
        reporter.check(!brightnessSpin->isEnabled() && !brightnessSlider->isEnabled(),
                       QStringLiteral("P2-08 没有可用亮度通路时，面板的亮度控件被禁用"
                                      "（不给用户一个按了没反应的控件）"));
        reporter.check(channelLabel->text().contains(QStringLiteral("不可用")),
                       QStringLiteral("P2-08 面板如实说明亮度通路不可用"),
                       channelLabel->text());
    }

    // ---- ⑧ 停用：色温逐元素还原，亮度**有意不还原** ----
    reporter.check(manager.setPluginEnabled(kBriId, false),
                   QStringLiteral("P2-08 插件停用成功"));
    settle(300);

    const Win32::GammaSnapshot afterDisable = Win32::captureGamma(0);
    reporter.check(afterDisable.valid && firstRampDifference(afterDisable.ramp, baseline.ramp) < 0,
                   QStringLiteral("P2-08 停用 → gamma 逐元素回到启用前"
                                  "（「可完整还原」落在停用上）"),
                   QStringLiteral("首个不同项：%1")
                       .arg(firstRampDifference(afterDisable.ramp, baseline.ramp)));

    if (hasBrightness) {
        double still = -1.0;
        WinEase::Win32::readBrightnessPercent(channel, &still);
        reporter.check(std::abs(still - externalBrightness) <= 2.0,
                       QStringLiteral("P2-08 停用**不**还原亮度（有意例外：屏幕现在的亮度就是"
                                      "用户眼前的样子，改回去才是新的副作用）"),
                       QStringLiteral("停用时 %1% / 现在 %2%")
                           .arg(externalBrightness, 0, 'f', 0)
                           .arg(still, 0, 'f', 0));
        reporter.check(!brightnessSpin->isEnabled(),
                       QStringLiteral("P2-08 停用后面板亮度控件被禁用"));
    }

    cleanup();

    // ---- ⑨ 收尾：把亮度写回用户原值并确认 ----
    if (hasBrightness) {
        settle(200);
        double finalValue = -1.0;
        const bool readable = WinEase::Win32::readBrightnessPercent(channel, &finalValue);
        reporter.check(readable && std::abs(finalValue - originalBrightness) <= 2.0,
                       QStringLiteral("P2-08 收尾：屏幕亮度已还原为用户原值"),
                       QStringLiteral("原值 %1% / 现在 %2%")
                           .arg(originalBrightness, 0, 'f', 0)
                           .arg(finalValue, 0, 'f', 0));
        reporter.info(QStringLiteral("P2-08 收尾：亮度已还原为 %1%").arg(finalValue, 0, 'f', 0));
    }
}

} // namespace

int runDisplayGroupTests(Reporter &reporter,
                         WinEase::PluginManager &manager,
                         StubServices &services)
{
    const int failuresAtStart = reporter.failures();

    reporter.info(QStringLiteral("说明：本组会**短暂改动真实系统**——"
                                 "P2-10 改麦克风静音状态、P2-08 改屏幕亮度与 gamma，"
                                 "两项均在用例结束时还原"));

    runMicMuteCase(reporter, manager, services);
    runMagnifierCase(reporter, manager, services);
    runBrightnessCase(reporter, manager, services);

    reporter.info(QStringLiteral("显示与媒体组失败项：%1").arg(reporter.failures() - failuresAtStart));
    return reporter.failures() - failuresAtStart;
}

} // namespace FeatureSmoke
