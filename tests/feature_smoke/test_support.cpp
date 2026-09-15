// winsock2.h 必须先于 windows.h（test_support.h → WindowUtils.h → windows.h），
// 否则先拉进旧版 winsock.h，两套定义会冲突
#include <winsock2.h>
#include <ws2tcpip.h>

#include "test_support.h"

#include <QBuffer>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QPalette>
#include <QScreen>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

// P3-09 的音频会话探针要建一份**真的**音频流（见 test_support.h 的说明）。
// 顺序不能动：上面已经把 winsock2.h 排在 windows.h 前面，这里追加 WASAPI 头即可。
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

// P3-11 的媒体会话探针要扮演 SMTC **发布方**（桌面程序走互操作接口，见 test_support.h）
#include "win32/MediaSessions.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.Streams.h>
// SetCurrentProcessExplicitAppUserModelID（给探针一个能认出来的 AUMID）
#include <shobjidl_core.h>

#include <chrono>

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
//  Reporter
// ---------------------------------------------------------------------------

FeatureSmoke::Reporter::Reporter(const QString &suiteName)
    : m_suite(suiteName)
{
}

namespace {

/// 设了 WINEASE_SMOKE_ASCII=1 就用纯 ASCII 标记输出。
/// 为什么需要：调试时经常要在 PowerShell 里把失败行 grep 出来，
/// 而中文在"命令参数 → 管道 → 文件"的往返里会被编码来回糟蹋，
/// 导致"明明有失败却 grep 不到"。CI 上也一样。
bool asciiOutputMode()
{
    static const bool enabled = !qEnvironmentVariableIsEmpty("WINEASE_SMOKE_ASCII");
    return enabled;
}

} // namespace

void FeatureSmoke::Reporter::check(bool ok, const QString &what, const QString &detail)
{
    ++m_total;
    const char *passTag = asciiOutputMode() ? "[PASS]" : "[通过]";
    const char *failTag = asciiOutputMode() ? "[FAIL]" : "[失败]";
    if (ok) {
        std::printf("%s %s\n", passTag, what.toUtf8().constData());
        return;
    }
    ++m_failures;
    std::printf("%s %s", failTag, what.toUtf8().constData());
    if (!detail.isEmpty()) {
        std::printf("  —— %s", detail.toUtf8().constData());
    }
    std::printf("\n");
    std::fflush(stdout);
}

void FeatureSmoke::Reporter::info(const QString &text)
{
    if (asciiOutputMode()) {
        std::printf("[INFO] %s\n", text.toUtf8().constData());
        std::fflush(stdout);
        return;
    }
    std::printf("[信息] %s\n", text.toUtf8().constData());
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
//  StubServices
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
//  RecordingElevationService —— 记录型提权桩
// ---------------------------------------------------------------------------

WinEase::ElevationResult FeatureSmoke::RecordingElevationService::execute(const QString &operation,
                                                                          const QVariantMap &arguments)
{
    m_calls.append(Call{ operation, arguments });

    WinEase::ElevationResult result;
    if (!m_available) {
        result.ok = false;
        result.error = QStringLiteral("提权助手不可用（自检桩）");
        return result;
    }

    // "成功"但**不执行任何真实操作**：自检断言的是链路与参数，
    // 不是"系统真的关机了"（那件事永远不该由自检来做）
    result.ok = true;
    result.data.insert(QStringLiteral("simulated"), true);
    if (operation == QStringLiteral("powerAction")) {
        result.data.insert(QStringLiteral("action"),
                           arguments.value(QStringLiteral("action")).toString());
    }
    if (m_resultData.contains(operation)) {
        // 自检预设的返回数据（例如"历史备份列表"这种只有真助手才看得见的东西）
        const QVariantMap preset = m_resultData.value(operation);
        for (auto it = preset.constBegin(); it != preset.constEnd(); ++it) {
            result.data.insert(it.key(), it.value());
        }
    }
    return result;
}

QString FeatureSmoke::RecordingElevationService::statusText()
{
    return m_available ? QStringLiteral("已就绪（自检桩，不会真的执行）")
                       : QStringLiteral("不可用（自检桩）");
}

int FeatureSmoke::RecordingElevationService::callCount(const QString &operation) const
{
    if (operation.isEmpty()) {
        return m_calls.size();
    }
    int count = 0;
    for (const Call &call : m_calls) {
        if (call.operation == operation) {
            ++count;
        }
    }
    return count;
}

QStringList FeatureSmoke::RecordingElevationService::operations() const
{
    QStringList list;
    for (const Call &call : m_calls) {
        list << call.operation;
    }
    return list;
}

QVariantMap FeatureSmoke::RecordingElevationService::lastArguments() const
{
    return m_calls.isEmpty() ? QVariantMap() : m_calls.last().arguments;
}

QVariantMap FeatureSmoke::RecordingElevationService::lastArgumentsOf(const QString &operation) const
{
    for (int index = m_calls.size() - 1; index >= 0; --index) {
        if (m_calls.at(index).operation == operation) {
            return m_calls.at(index).arguments;
        }
    }
    return QVariantMap();
}

QVariant FeatureSmoke::StubServices::configValue(const QString &pluginId,
                                                 const QString &key,
                                                 const QVariant &defaultValue) const
{
    return m_config.value(pluginId + QStringLiteral("::") + key, defaultValue);
}

void FeatureSmoke::StubServices::setConfigValue(const QString &pluginId,
                                                const QString &key,
                                                const QVariant &value)
{
    m_config.insert(pluginId + QStringLiteral("::") + key, value);
}

void FeatureSmoke::StubServices::log(const QString &pluginId,
                                     WinEase::PluginLogLevel level,
                                     const QString &message)
{
    ++m_logCount;
    if (level != WinEase::PluginLogLevel::Info) {
        m_warnings.append(QStringLiteral("[%1] %2").arg(pluginId, message));
    }
}

void FeatureSmoke::StubServices::notify(const QString &title, const QString &body)
{
    ++m_notifications;
    m_warnings.append(QStringLiteral("通知：%1 —— %2").arg(title, body));
}

void FeatureSmoke::StubServices::setTrayBadge(const QString &pluginId,
                                              const QString &text,
                                              const QString &colorHex,
                                              const QString &tooltip)
{
    ++m_badgeCalls;
    m_lastBadgeColor = colorHex;
    m_lastBadgeTooltip = tooltip;
    // 空文字 = 清除（约定见 PluginServices::setTrayBadge）。
    // 这里**不做**任何自作主张的清理：桩只记录"插件请求了什么"，
    // 宿主的兜底回收（停用/崩溃时按 pluginId 摘掉）在 SystemTrayManager 那一侧。
    if (text.isEmpty()) {
        m_badges.remove(pluginId);
    } else {
        m_badges.insert(pluginId, text);
    }
}

bool FeatureSmoke::StubServices::registerHotkey(const QString &pluginId,
                                                const QString &action,
                                                const QKeySequence &sequence,
                                                const QString &description)
{
    Q_UNUSED(description);
    m_hotkeys.insert(pluginId + QStringLiteral("::") + action,
                     sequence.toString(QKeySequence::PortableText));
    return true;
}

void FeatureSmoke::StubServices::unregisterHotkey(const QString &pluginId, const QString &action)
{
    m_hotkeys.remove(pluginId + QStringLiteral("::") + action);
}

void FeatureSmoke::StubServices::preset(const QString &pluginId,
                                        const QString &key,
                                        const QVariant &value)
{
    setConfigValue(pluginId, key, value);
}

// ---------------------------------------------------------------------------
//  ProbeWindow —— 独立进程里的目标窗口
// ---------------------------------------------------------------------------

QString FeatureSmoke::ProbeWindow::titleText()
{
    return QStringLiteral("WinEase 自检目标窗口");
}

QString FeatureSmoke::ProbeWindow::title() const
{
    return WinEase::Win32::windowTitle(m_handle);
}

QRect FeatureSmoke::ProbeWindow::rect() const
{
    return WinEase::Win32::windowRect(m_handle);
}

QRect FeatureSmoke::ProbeWindow::visualRect() const
{
    return WinEase::Win32::visualWindowRect(m_handle);
}

QPoint FeatureSmoke::ProbeWindow::center() const
{
    const QRect r = rect();
    return r.isValid() ? r.center() : QPoint();
}

FeatureSmoke::ProbeWindow::~ProbeWindow()
{
    stop();
}

bool FeatureSmoke::ProbeWindow::start(QString *errorOut)
{
    m_readyFile = QDir::tempPath()
                  + QStringLiteral("/winease_feature_smoke_probe_%1.txt")
                        .arg(QCoreApplication::applicationPid());
    m_raiseFile = m_readyFile + QStringLiteral(".raise");
    QFile::remove(m_readyFile);
    QFile::remove(m_raiseFile);

    m_process = new QProcess();
    m_process->setProgram(QCoreApplication::applicationFilePath());
    m_process->setArguments({ QStringLiteral("--probe-window"), m_readyFile });
    m_process->start();
    if (!m_process->waitForStarted(5000)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("无法启动探针子进程：%1").arg(m_process->errorString());
        }
        return false;
    }

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 15000) {
        if (QFile::exists(m_readyFile)) {
            QFile file(m_readyFile);
            if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                const QString text = QString::fromUtf8(file.readAll()).trimmed();
                const QStringList parts = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
                if (parts.size() >= 3) {
                    const HWND handle = reinterpret_cast<HWND>(parts.at(0).toULongLong());
                    const HWND helper = reinterpret_cast<HWND>(parts.at(1).toULongLong());
                    if (WinEase::Win32::isValidWindow(handle)) {
                        m_handle = handle;
                        m_helper = WinEase::Win32::isValidWindow(helper) ? helper : nullptr;
                        return true;
                    }
                }
            }
        }
        QThread::msleep(100);
    }

    if (errorOut != nullptr) {
        *errorOut = QStringLiteral("目标窗口探针 15 秒内未就绪");
    }
    return false;
}

void FeatureSmoke::ProbeWindow::requestRaise()
{
    if (m_raiseFile.isEmpty()) {
        return;
    }
    QFile file(m_raiseFile);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream(&file) << "raise\n";
    }
}

void FeatureSmoke::ProbeWindow::stop()
{
    // 删除哨兵文件 → 子进程自己退出（比强杀干净：Qt 能正常析构窗口）
    if (!m_readyFile.isEmpty()) {
        QFile::remove(m_readyFile);
    }
    if (!m_raiseFile.isEmpty()) {
        QFile::remove(m_raiseFile);
    }
    if (m_process != nullptr) {
        if (!m_process->waitForFinished(3000)) {
            m_process->terminate();
            if (!m_process->waitForFinished(2000)) {
                m_process->kill();
                m_process->waitForFinished(1000);
            }
        }
        m_process->deleteLater();
        m_process = nullptr;
    }
    m_handle = nullptr;
    m_helper = nullptr;
}

namespace {

/// 轻量"挤到最前"：只抬 z 序 + 试一次前台，**不做任何等待**（定时器里用它，
/// 别把子进程的事件循环堵住）。
bool raiseLightweight(HWND hwnd)
{
    if (hwnd == nullptr || ::IsWindow(hwnd) == FALSE) {
        return false;
    }
    ::SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    ::SetForegroundWindow(hwnd);
    return ::GetForegroundWindow() == hwnd;
}

// 子进程内：把 hwnd 切到前台。**分阶段尝试 + 连续稳定确认**，不是"调一次就返回结果"：
//   ① `SetWindowPos(TOP)` + `SetForegroundWindow`，**立即检查一次**；
//   ② 失败 → 等一会儿事件循环（前台切换可能是排队投递的，也顺便等对方线程松手）；
//   ③ 再试"`AttachThreadInput` + `SetForegroundWindow`"：前台切换受**前台锁定**限制 ——
//      调用进程既不是前台进程、也没收到最后一次输入事件时，单纯 SetForegroundWindow
//      只会闪一下任务栏按钮然后失败；标准绕法是临时把自己 AttachThreadInput 到
//      当前前台线程上，借它的输入队列取得前台设置权；
//   ④ 最后交给 `waitForForeground()` 做**连续稳定**确认（连续 3 次采样都保持目标）。
//
// 为什么必须有 ④：`SetForegroundWindow` 的返回值不等于"前台已经是它"，
// 而"**某一次**等于"也不等于"稳住了" —— 中间还有别的进程在抢。
// 判"连续稳定"才是真的成了（P1-04 的前台切换前置就是被这一条反复打脸的）。
bool forceForeground(HWND hwnd)
{
    if (hwnd == nullptr || ::IsWindow(hwnd) == FALSE) {
        return false;
    }

    // ① 直接试一次，立即检查
    if (raiseLightweight(hwnd)) {
        return true;
    }

    // ② 等事件循环（也让前台线程松手）
    if (FeatureSmoke::waitForForeground(hwnd, 300)) {
        return true;
    }

    // ③ 借前台线程的输入队列抢一次设置权
    const HWND foreground = ::GetForegroundWindow();
    const DWORD foregroundThread =
        (foreground != nullptr) ? ::GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD ownThread = ::GetCurrentThreadId();
    const bool attached = (foregroundThread != 0 && foregroundThread != ownThread)
                          && ::AttachThreadInput(ownThread, foregroundThread, TRUE) != FALSE;

    ::SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    ::SetForegroundWindow(hwnd);

    if (attached) {
        ::AttachThreadInput(ownThread, foregroundThread, FALSE);
    }

    // ④ 交给"连续稳定"确认
    return FeatureSmoke::waitForForeground(hwnd, 1200);
}

} // namespace

int FeatureSmoke::ProbeWindow::runChild(const QString &readyFile)
{
    QWidget window;
    window.setWindowTitle(titleText());
    window.resize(900, 600);
    window.move(120, 120);
    window.show();

    // 辅助窗口：只用来"制造一次来自别的进程的前台切换"（见 requestRaise 的说明）。
    // 一开始就显示（位置固定、不遮挡目标窗口），这样整个用例期间**可见顶层窗口集合是稳定的**，
    // 父进程比较 z 序下标才有意义。测试结束时随子进程一起消失。
    QWidget helper;
    helper.setWindowTitle(titleText() + QStringLiteral("（辅助）"));
    helper.resize(260, 140);
    {
        const QScreen *screen = QGuiApplication::primaryScreen();
        const QRect area = (screen != nullptr) ? screen->availableGeometry() : QRect(0, 0, 800, 600);
        helper.move(area.right() - 300, area.bottom() - 200);
    }
    helper.show();

    const HWND hwnd = reinterpret_cast<HWND>(window.winId());
    const HWND helperHwnd = reinterpret_cast<HWND>(helper.winId());
    ::SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    forceForeground(hwnd);

    // 前 3 秒反复往最前挤：父进程靠"光标下的窗口"锁定目标，
    // 被别的窗口压住的话锁定会指向别人（父进程也会做前置断言，这里只是提高成功率）。
    // ⚠ 定时器里只能用**不等待**的轻量版：`forceForeground()` 现在会做分阶段等待
    //    （最长 ~1.5 秒），放进定时器会把子进程的事件循环堵住，
    //    连"抬升命令 / 退出哨兵"都处理不了。
    auto ticks = std::make_shared<int>(0);
    auto *raiseTimer = new QTimer(&window);
    QObject::connect(raiseTimer, &QTimer::timeout, &window, [hwnd, raiseTimer, ticks] {
        if (*ticks >= 15) {
            raiseTimer->stop();
            return;
        }
        ++(*ticks);
        raiseLightweight(hwnd);
    });
    raiseTimer->start(200);

    // 就绪文件：目标窗口句柄 + 辅助窗口句柄 + 目标窗口矩形（父进程用它定位光标）
    {
        RECT rect{ 0, 0, 0, 0 };
        ::GetWindowRect(hwnd, &rect);
        QFile file(readyFile);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            return 3;
        }
        QTextStream out(&file);
        out << reinterpret_cast<quintptr>(hwnd) << ' ' << reinterpret_cast<quintptr>(helperHwnd) << ' '
            << rect.left << ' ' << rect.top << ' ' << (rect.right - rect.left) << ' '
            << (rect.bottom - rect.top) << '\n';
    }

    // "抬升"命令（父进程写文件触发）：先模拟别的程序把目标窗口抬到最顶，
    // 再把辅助窗口激活到前台 —— 后者会产生 EVENT_SYSTEM_FOREGROUND，
    // 而事件来自**本子进程**（不是装钩子的父进程），因此不会被 WINEVENT_SKIPOWNPROCESS 过滤。
    const QString raiseFile = readyFile + QStringLiteral(".raise");
    QTimer raiseWatch;
    QObject::connect(&raiseWatch, &QTimer::timeout, &window, [hwnd, helperHwnd, raiseFile] {
        if (!QFile::exists(raiseFile)) {
            return;
        }
        QFile::remove(raiseFile);

        // 先把它抬到最顶，再激活辅助窗口（模拟"用户切到别的窗口"）。
        //
        // ⚠ 前台切换受"前台锁定"限制，用户正在操作别的窗口时会被系统**拒绝** ——
        //   那种情况下我们要的是**如实失败**（父进程据此报"前置不成立"），
        //   而不是在这里无限重试把测试"搓"成通过。
        //   所以只试 2 轮：`forceForeground()` 内部已经把"立即检查 → 等事件循环 →
        //   AttachThreadInput → 连续稳定确认"这一整套做完了（约 1.5 秒/轮），
        //   外层再套 2 轮就够覆盖"对方刚松手"这种时序，再多就是浪费与被测无关的时间。
        for (int attempt = 0; attempt < 2; ++attempt) {
            ::SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            if (forceForeground(helperHwnd)) {
                return;
            }
        }
    });
    raiseWatch.start(100);

    // 退出哨兵：父进程删掉文件我们就收工；另加 2 分钟硬超时兜底，
    // 避免父进程异常退出时留下一个看不见的窗口进程
    QTimer watch;
    QObject::connect(&watch, &QTimer::timeout, &window, [readyFile] {
        if (!QFile::exists(readyFile)) {
            QCoreApplication::quit();
        }
    });
    watch.start(200);
    QTimer::singleShot(120000, &window, [] { QCoreApplication::quit(); });

    return QCoreApplication::exec();
}

// ---------------------------------------------------------------------------
//  FileLockProbe（P3-02 的真实占用源）
// ---------------------------------------------------------------------------

namespace {

/// 以"独占"方式打开（`dwShareMode = 0`）——这之后**任何**别的进程（包括自己再开一次）
/// 都拿不到这个文件，删除/改名也会失败。这就是"文件被占用删不掉"的最小复现。
HANDLE openExclusive(const QString &filePath)
{
    const std::wstring native = QDir::toNativeSeparators(filePath).toStdWString();
    return ::CreateFileW(native.c_str(),
                         GENERIC_READ | GENERIC_WRITE,
                         0,
                         nullptr,
                         OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL,
                         nullptr);
}

} // namespace

int FeatureSmoke::FileLockProbe::runChild(const QString &filePath, const QString &readyFile)
{
    const HANDLE handle = openExclusive(filePath);
    if (handle == INVALID_HANDLE_VALUE) {
        std::printf("[失败] 锁定探针：无法独占打开 %s（错误码 %lu）\n",
                    filePath.toUtf8().constData(),
                    static_cast<unsigned long>(::GetLastError()));
        return 3;
    }

    // 就绪文件里写下自己的 PID：父进程据此断言"Restart Manager 报出的占用者就是它"
    {
        QFile file(readyFile);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            ::CloseHandle(handle);
            return 4;
        }
        QTextStream stream(&file);
        stream << static_cast<unsigned long>(::GetCurrentProcessId()) << "\n";
    }

    // 持有句柄直到父进程删掉就绪文件（比"被强杀"干净：句柄能正常关闭）
    while (QFile::exists(readyFile)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(20);
    }

    ::CloseHandle(handle);
    return 0;
}

FeatureSmoke::FileLockProbe::~FileLockProbe()
{
    stop();
}

bool FeatureSmoke::FileLockProbe::start(const QString &filePath, QString *errorOut)
{
    m_readyFile = QDir::tempPath()
                  + QStringLiteral("/winease_lock_probe_%1.txt")
                        .arg(QCoreApplication::applicationPid());
    QFile::remove(m_readyFile);

    m_process = new QProcess();
    m_process->setProgram(QCoreApplication::applicationFilePath());
    m_process->setArguments({ QStringLiteral("--lock-file"), filePath, m_readyFile });
    m_process->start();
    if (!m_process->waitForStarted(5000)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("无法启动锁定探针子进程：%1").arg(m_process->errorString());
        }
        return false;
    }

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 10000) {
        if (QFile::exists(m_readyFile)) {
            // 就绪文件出现 = 句柄已经拿到。再确认一次"确实锁住了"：
            // 父进程自己也打不开它，才是"占用"的硬证据
            const HANDLE probe = openExclusive(filePath);
            if (probe == INVALID_HANDLE_VALUE) {
                return true;
            }
            ::CloseHandle(probe);
        }
        QThread::msleep(50);
    }

    if (errorOut != nullptr) {
        *errorOut = QStringLiteral("锁定探针 10 秒内没有锁住文件");
    }
    stop();
    return false;
}

void FeatureSmoke::FileLockProbe::stop()
{
    if (!m_readyFile.isEmpty()) {
        QFile::remove(m_readyFile);
    }
    if (m_process != nullptr) {
        if (!m_process->waitForFinished(3000)) {
            m_process->terminate();
            if (!m_process->waitForFinished(2000)) {
                m_process->kill();
                m_process->waitForFinished(1000);
            }
        }
        m_process->deleteLater();
        m_process = nullptr;
    }
}

bool FeatureSmoke::FileLockProbe::isRunning() const
{
    return m_process != nullptr && m_process->state() == QProcess::Running;
}

bool FeatureSmoke::FileLockProbe::waitFinished(int timeoutMs)
{
    if (m_process == nullptr) {
        return true;
    }
    return m_process->state() != QProcess::Running || m_process->waitForFinished(timeoutMs);
}

quint32 FeatureSmoke::FileLockProbe::pid() const
{
    return (m_process != nullptr) ? static_cast<quint32>(m_process->processId()) : 0;
}

// ---------------------------------------------------------------------------
//  AudioSessionProbe（P3-09 的真实音频会话源）
// ---------------------------------------------------------------------------

int FeatureSmoke::AudioSessionProbe::runChild(const QString &readyFile, float volume)
{
    const auto failWith = [](const char *stage, long result) {
        std::printf("[失败] 音频会话探针：%s 失败（HRESULT 0x%08lX）\n",
                    stage,
                    static_cast<unsigned long>(result));
        return 3;
    };

    const HRESULT comResult = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        return failWith("CoInitializeEx", comResult);
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                    nullptr,
                                    CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void **>(enumerator.GetAddressOf()));
    if (FAILED(hr)) {
        return failWith("创建设备枚举器", hr);
    }

    // 用 eConsole：这才是"正在播放的应用"所属的会话域（eMultimedia 是另一个域）
    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.GetAddressOf());
    if (FAILED(hr)) {
        return failWith("取默认渲染端点", hr);
    }

    ComPtr<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient),
                          CLSCTX_ALL,
                          nullptr,
                          reinterpret_cast<void **>(client.GetAddressOf()));
    if (FAILED(hr)) {
        return failWith("激活 IAudioClient", hr);
    }

    WAVEFORMATEX *mixFormat = nullptr;
    hr = client->GetMixFormat(&mixFormat);
    if (FAILED(hr) || mixFormat == nullptr) {
        return failWith("取混音格式", hr);
    }

    // 共享模式下的缓冲时长必须与设备周期对齐，否则 Initialize 会回
    // AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED；拿不到周期就交 0（= 用引擎默认值）
    REFERENCE_TIME defaultPeriod = 0;
    if (FAILED(client->GetDevicePeriod(&defaultPeriod, nullptr))) {
        defaultPeriod = 0;
    }

    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, defaultPeriod, 0, mixFormat, nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, mixFormat, nullptr);
    }
    const UINT32 blockAlign = mixFormat->nBlockAlign;
    ::CoTaskMemFree(mixFormat);
    if (FAILED(hr)) {
        return failWith("Initialize（共享模式）", hr);
    }

    // 会话级别的音量控制：**这一步本身就是"这是真会话"的证明**
    ComPtr<ISimpleAudioVolume> sessionVolume;
    hr = client->GetService(__uuidof(ISimpleAudioVolume),
                            reinterpret_cast<void **>(sessionVolume.GetAddressOf()));
    if (FAILED(hr)) {
        return failWith("GetService(ISimpleAudioVolume)", hr);
    }
    const HRESULT volumeResult = sessionVolume->SetMasterVolume(volume, nullptr);

    ComPtr<IAudioRenderClient> render;
    hr = client->GetService(__uuidof(IAudioRenderClient),
                            reinterpret_cast<void **>(render.GetAddressOf()));
    if (FAILED(hr)) {
        return failWith("GetService(IAudioRenderClient)", hr);
    }

    UINT32 bufferFrames = 0;
    hr = client->GetBufferSize(&bufferFrames);
    if (FAILED(hr)) {
        return failWith("GetBufferSize", hr);
    }

    // Start 之前必须先把缓冲填满，否则会听到一声"咔"
    BYTE *initial = nullptr;
    if (SUCCEEDED(render->GetBuffer(bufferFrames, &initial))) {
        ::memset(initial, 0, static_cast<size_t>(bufferFrames) * blockAlign);
        render->ReleaseBuffer(bufferFrames, 0);
    }
    const HRESULT startResult = client->Start();

    // 就绪文件里写下自己的 PID 与真实达到的状态：
    // 父进程据此断言"这份会话确实存在"，而不是"子进程说自己成功了"
    {
        QFile file(readyFile);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            return 4;
        }
        QTextStream stream(&file);
        stream << static_cast<unsigned long>(::GetCurrentProcessId()) << "\n";
        stream << ((SUCCEEDED(startResult)) ? QStringLiteral("active")
                                            : QStringLiteral("initialized-only"))
               << "\n";
        stream << QStringLiteral("volumeRequest=0x%1\n")
                      .arg(static_cast<unsigned long>(volumeResult), 8, 16, QLatin1Char('0'));
    }

    // 持续灌静音数据，直到父进程删掉就绪文件（会话保持"正在播放"）。
    // 另加 2 分钟硬超时兜底：父进程若被强杀，"删就绪文件"这一步就没人做了，
    // 而这个子进程会一直贴在默认输出设备上占一份会话 —— 与 WindowProbe 同一套
    // 兜底思路（自检绝不把自己的残留留在用户的系统里）。
    QElapsedTimer lifetime;
    lifetime.start();
    while (QFile::exists(readyFile) && lifetime.elapsed() < 120000) {
        if (SUCCEEDED(startResult)) {
            UINT32 padding = 0;
            if (SUCCEEDED(client->GetCurrentPadding(&padding)) && bufferFrames > padding) {
                const UINT32 available = bufferFrames - padding;
                BYTE *data = nullptr;
                if (SUCCEEDED(render->GetBuffer(available, &data))) {
                    ::memset(data, 0, static_cast<size_t>(available) * blockAlign);
                    render->ReleaseBuffer(available, 0);
                }
            }
        }
        QThread::msleep(20);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }

    if (SUCCEEDED(startResult)) {
        client->Stop();
    }
    return 0;
}

FeatureSmoke::AudioSessionProbe::~AudioSessionProbe()
{
    stop();
}

bool FeatureSmoke::AudioSessionProbe::start(float volume, QString *errorOut)
{
    static int counter = 0;
    ++counter;
    m_readyFile = QDir::tempPath()
                  + QStringLiteral("/winease_audio_probe_%1_%2.txt")
                        .arg(QCoreApplication::applicationPid())
                        .arg(counter);
    QFile::remove(m_readyFile);

    m_process = new QProcess();
    // 子进程的报错直接打到父进程的控制台上：探针失败时最需要的就是那句 HRESULT
    m_process->setProcessChannelMode(QProcess::ForwardedChannels);
    m_process->setProgram(QCoreApplication::applicationFilePath());
    m_process->setArguments({ QStringLiteral("--audio-session"),
                              m_readyFile,
                              QString::number(static_cast<double>(volume), 'f', 4) });
    m_process->start();
    if (!m_process->waitForStarted(5000)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("无法启动音频会话探针子进程：%1").arg(m_process->errorString());
        }
        stop();
        return false;
    }

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 10000) {
        if (QFile::exists(m_readyFile)) {
            return true;
        }
        if (m_process->state() != QProcess::Running) {
            break; // 子进程自己挂了（它会把自己的失败原因打到控制台）
        }
        QThread::msleep(40);
    }

    if (errorOut != nullptr) {
        *errorOut = QStringLiteral("音频会话探针 10 秒内没有建出音频会话");
    }
    stop();
    return false;
}

void FeatureSmoke::AudioSessionProbe::stop()
{
    if (!m_readyFile.isEmpty()) {
        QFile::remove(m_readyFile);
    }
    if (m_process != nullptr) {
        if (!m_process->waitForFinished(3000)) {
            m_process->terminate();
            if (!m_process->waitForFinished(2000)) {
                m_process->kill();
                m_process->waitForFinished(1000);
            }
        }
        m_process->deleteLater();
        m_process = nullptr;
    }
}

bool FeatureSmoke::AudioSessionProbe::isRunning() const
{
    return m_process != nullptr && m_process->state() == QProcess::Running;
}

bool FeatureSmoke::AudioSessionProbe::waitFinished(int timeoutMs)
{
    if (m_process == nullptr) {
        return true;
    }
    return m_process->state() != QProcess::Running || m_process->waitForFinished(timeoutMs);
}

quint32 FeatureSmoke::AudioSessionProbe::pid() const
{
    return (m_process != nullptr) ? static_cast<quint32>(m_process->processId()) : 0;
}

// ---------------------------------------------------------------------------
//  MediaSessionProbe（P3-11 的真实媒体会话源）
// ---------------------------------------------------------------------------

namespace {

const wchar_t *kMediaTitle = L"WinEase 自检曲目";
const wchar_t *kMediaArtist = L"WinEase 自检艺术家";
const wchar_t *kMediaAlbum = L"WinEase 自检专辑";

const char *kMediaAumid = "WinEase.FeatureSmoke.MediaProbe";

/// 探针对外声明的曲目时长（3:00）——
/// 平台层把 WinRT 的 100ns tick 换算成毫秒这件事，就靠这个数验（踩坑 #75）
constexpr qint64 kMediaDurationMs = 180000;
/// 探针封面的纯色（回读后按像素比对，证明封面数据真的过来了）
const QColor kMediaArtworkColor(0xC8, 0x1E, 0x28);

constexpr qint64 kTicksPerMs = 10000;

/// 桌面互操作接口（`SystemMediaTransportControlsInterop.h` 里那个，IID 固定）。
/// 与 `VirtualDesktop.cpp` 里本地声明 `IVirtualDesktopManagerLocal` 同一套路：
/// 手写声明比拉进 um 头更稳，也不给插件侧增加依赖。
///
/// ⚠ 基类必须是 `IInspectable`：这个接口是 WinRT 接口（不是纯 COM），
/// 少写这一层会让 vtable 整体前移 3 个槽位 —— `GetForWindow` 会调到 `GetIids` 上，
/// 表现成调用当场 0xC0000005（踩坑 #77）。
struct __declspec(uuid("ddb0472d-c911-4a1f-86d9-dc3d71a95f5a")) ISystemMediaTransportControlsInterop
    : ::IInspectable
{
    virtual HRESULT __stdcall GetForWindow(HWND appWindow,
                                          REFIID riid,
                                          void **mediaTransportControl) = 0;
};

QString mediaButtonName(int button)
{
    switch (button) {
    case 0:
        return QStringLiteral("Play");
    case 1:
        return QStringLiteral("Pause");
    case 2:
        return QStringLiteral("Stop");
    case 3:
        return QStringLiteral("Record");
    case 4:
        return QStringLiteral("FastForward");
    case 5:
        return QStringLiteral("Rewind");
    case 6:
        return QStringLiteral("Next");
    case 7:
        return QStringLiteral("Previous");
    case 8:
        return QStringLiteral("ChannelUp");
    case 9:
        return QStringLiteral("ChannelDown");
    default:
        return QStringLiteral("未知(%1)").arg(button);
    }
}

QString mediaEventFile(const QString &readyFile)
{
    return readyFile + QStringLiteral(".events");
}

/// 事件记录：每行一条 `button=<编号>` 或 `position=<毫秒>`。
/// 这是"系统的请求确实送到了播放器"唯一的硬证据，所以由子进程逐条落盘。
void appendMediaEvent(const QString &readyFile, const QString &line)
{
    QFile file(mediaEventFile(readyFile));
    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream stream(&file);
        stream << line << "\n";
        file.flush();
    }
}

/// 造一张纯色小图当封面，**落成磁盘上的文件**再发布出去。
///
/// 为什么不是内存流（`CreateFromStream`）：封面是"另一个进程的对象"，
/// 父进程读它要跨进程编排调用；文件形态的引用（`CreateFromFile`）就是真播放器
/// 给封面的常见形态，读起来就是各读各的本地文件（读封面的线程要求见踩坑 #78）。
///
/// ⚠ 路径必须转成**反斜杠**：`StorageFile::GetFileFromPathAsync` 不认正斜杠，
/// Qt 的 `QDir::tempPath()` 恰好给的是正斜杠 —— 直接传进去会回 `0x800700A1`
/// （ERROR_BAD_PATHNAME，踩坑 #79）。
winrt::Windows::Storage::Streams::RandomAccessStreamReference makeArtworkReference(
    const QString &path)
{
    QImage image(32, 32, QImage::Format_RGB32);
    image.fill(kMediaArtworkColor);
    image.save(path, "PNG");

    const QString nativePath = QDir::toNativeSeparators(path);
    const auto file =
        winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(nativePath.toStdWString()).get();
    return winrt::Windows::Storage::Streams::RandomAccessStreamReference::CreateFromFile(file);
}

/// 把进度广播出去（SMTC2 的 `UpdateTimelineProperties`）。
/// 不广播的话系统对这份会话的"时长/位置"一无所知 —— 面板就该如实写"没有提供进度"。
void publishTimeline(const winrt::Windows::Media::SystemMediaTransportControls &controls,
                     qint64 positionMs)
{
    winrt::Windows::Media::SystemMediaTransportControlsTimelineProperties timeline;
    timeline.StartTime(winrt::Windows::Foundation::TimeSpan{ std::chrono::milliseconds(0) });
    timeline.MinSeekTime(winrt::Windows::Foundation::TimeSpan{ std::chrono::milliseconds(0) });
    timeline.MaxSeekTime(winrt::Windows::Foundation::TimeSpan{ std::chrono::milliseconds(
        kMediaDurationMs) });
    timeline.EndTime(winrt::Windows::Foundation::TimeSpan{ std::chrono::milliseconds(
        kMediaDurationMs) });
    timeline.Position(winrt::Windows::Foundation::TimeSpan{ std::chrono::milliseconds(positionMs) });
    controls.UpdateTimelineProperties(timeline);
}

/// 读"key=value"格式的状态文件（文件不存在时返回空 map）
QHash<QString, QString> readKeyValueFile(const QString &path)
{
    QHash<QString, QString> values;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return values;
    }
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine();
        const int separator = line.indexOf(QLatin1Char('='));
        if (separator <= 0) {
            continue;
        }
        values.insert(line.left(separator).trimmed(), line.mid(separator + 1).trimmed());
    }
    return values;
}

/// 从枚举结果里认出"自检自己造的那一份会话"。
/// 判据按可靠性排序：来源串（显式 AUMID）→ 来源串里带探针名 → 曲目文案。
bool matchesProbeSession(const WinEase::Win32::MediaSessionInfo &info, const QString &aumid)
{
    if (!aumid.isEmpty() && info.sourceAppId.compare(aumid, Qt::CaseInsensitive) == 0) {
        return true;
    }
    if (info.sourceAppId.contains(QStringLiteral("MediaProbe"), Qt::CaseInsensitive)) {
        return true;
    }
    return info.title == QString::fromWCharArray(kMediaTitle);
}

} // namespace

QString FeatureSmoke::MediaSessionProbe::titleText()
{
    return QString::fromWCharArray(kMediaTitle);
}

QString FeatureSmoke::MediaSessionProbe::artistText()
{
    return QString::fromWCharArray(kMediaArtist);
}

QString FeatureSmoke::MediaSessionProbe::albumText()
{
    return QString::fromWCharArray(kMediaAlbum);
}

QString FeatureSmoke::MediaSessionProbe::appUserModelId()
{
    return QString::fromUtf8(kMediaAumid);
}

int FeatureSmoke::MediaSessionProbe::runChild(const QString &readyFile)
{
    // AUMID 必须在**建窗口之前**设：Windows 只认进程第一次报出的那个。
    // 有了它，父进程就能把"自检造的那一份会话"从真实会话里精确认出来
    const std::wstring aumid = QString::fromUtf8(kMediaAumid).toStdWString();
    ::SetCurrentProcessExplicitAppUserModelID(aumid.c_str());

    // 互操作接口要"一个窗口"才能给出 SMTC 控制器，所以子进程必须建真窗口。
    // 不给焦点（自检期间用户可能正在别的窗口里工作）、放到屏幕右下角。
    QWidget window;
    window.setWindowTitle(titleText());
    window.resize(360, 200);
    window.setAttribute(Qt::WA_ShowWithoutActivating, true);
    if (const QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect area = screen->availableGeometry();
        window.move(area.right() - 420, area.bottom() - 280);
    }
    window.show();

    const HWND hwnd = reinterpret_cast<HWND>(window.winId());

    QString publishError;
    QString artworkNote;
    bool published = false;
    winrt::Windows::Media::SystemMediaTransportControls controls{ nullptr };

    // 收到的"播放/暂停/停止"请求要真的落到**播放状态**上 —— 真实播放器就是这么做的。
    // 不这么做的话，父进程回读到的永远是发布时那句"正在播放"（踩坑 #80：`TryPauseAsync`
    // 只是把请求交给播放器，**状态是播放器自己改的**）。
    // 同时把收到的每一个键记进文件：这是"系统的请求确实送到了播放器"唯一的硬证据。
    const auto handleButton = [&controls, &readyFile](int button) {
        appendMediaEvent(readyFile, QStringLiteral("button=%1").arg(button));
        if (!controls) {
            return;
        }
        switch (button) {
        case 0: // Play
            controls.PlaybackStatus(winrt::Windows::Media::MediaPlaybackStatus::Playing);
            break;
        case 1: // Pause
            controls.PlaybackStatus(winrt::Windows::Media::MediaPlaybackStatus::Paused);
            break;
        case 2: // Stop
            controls.PlaybackStatus(winrt::Windows::Media::MediaPlaybackStatus::Stopped);
            break;
        default:
            break;
        }
    };

    // "跳到某个位置"的请求：真播放器会把自己的位置改成它并重新广播进度。
    // 这里照做，父进程才能回读到"跳转真的生效了"（而不是只看到"请求被接受"）。
    const auto handlePosition = [&controls, &readyFile](qint64 positionMs) {
        appendMediaEvent(readyFile, QStringLiteral("position=%1").arg(positionMs));
        if (!controls) {
            return;
        }
        publishTimeline(controls, positionMs);
    };

    try {
        const auto interop =
            winrt::get_activation_factory<winrt::Windows::Media::SystemMediaTransportControls,
                                          ISystemMediaTransportControlsInterop>();
        // ⚠ 这里**不能**用 `winrt::put_abi(controls)`：投影 runtime class 对象不是
        //    "一个接口指针"，把接口指针写进它自己的地址上是内存踩踏（实测直接 0xC0000005）。
        //    正确姿势：先拿裸指针，再用 `take_ownership_from_abi` 交给投影对象接管。
        void *rawControls = nullptr;
        const HRESULT hr = interop->GetForWindow(
            hwnd,
            winrt::guid_of<winrt::Windows::Media::ISystemMediaTransportControls>(),
            &rawControls);
        if (FAILED(hr) || rawControls == nullptr) {
            publishError = QStringLiteral("GetForWindow 失败（HRESULT 0x%1）")
                               .arg(static_cast<unsigned long>(hr), 8, 16, QLatin1Char('0'));
        } else {
            controls = winrt::Windows::Media::SystemMediaTransportControls{
                rawControls, winrt::take_ownership_from_abi };

            controls.IsEnabled(true);
            controls.IsPlayEnabled(true);
            controls.IsPauseEnabled(true);
            controls.IsNextEnabled(true);
            controls.IsPreviousEnabled(true);
            controls.PlaybackStatus(winrt::Windows::Media::MediaPlaybackStatus::Playing);

            const auto updater = controls.DisplayUpdater();
            updater.Type(winrt::Windows::Media::MediaPlaybackType::Music);
            const auto music = updater.MusicProperties();
            music.Title(kMediaTitle);
            music.Artist(kMediaArtist);
            music.AlbumTitle(kMediaAlbum);
            // 封面单独兜一层：封面失败不该把"整个会话注册"一起拖下水
            // （真播放器也是这个道理：没有封面照样能播）
            try {
                updater.Thumbnail(
                    makeArtworkReference(readyFile + QStringLiteral(".artwork.png")));
            } catch (const winrt::hresult_error &error) {
                artworkNote = QStringLiteral("封面没能发布（HRESULT 0x%1）")
                                  .arg(static_cast<unsigned long>(error.code().value),
                                       8,
                                       16,
                                       QLatin1Char('0'));
            }
            updater.Update();

            // 进度（时长 3:00、位置 0）—— 不广播的话系统对这份会话的时长一无所知
            publishTimeline(controls, 0);

            // 媒体键被系统路由到"当前会话"时，也会以 ButtonPressed 送到这里
            controls.ButtonPressed([handleButton](const auto &, const auto &args) {
                handleButton(static_cast<int>(args.Button()));
            });
            controls.PlaybackPositionChangeRequested(
                [handlePosition](const auto &, const auto &args) {
                    handlePosition(args.RequestedPlaybackPosition().count() / kTicksPerMs);
                });
            published = true;
        }
    } catch (const winrt::hresult_error &error) {
        publishError = QStringLiteral("注册媒体会话抛出异常（HRESULT 0x%1）")
                           .arg(static_cast<unsigned long>(error.code().value),
                                8,
                                16,
                                QLatin1Char('0'));
    } catch (...) {
        publishError = QStringLiteral("注册媒体会话时发生未知异常");
    }

    // ---- 自己先认一遍：这份会话到底有没有进到系统的会话列表里 ----
    QString selfSessionId = QStringLiteral("-");
    int sessionCount = 0;
    bool selfCurrent = false;
    QString selfTitle;
    QString selfArtist;
    QString selfArtworkNote = QStringLiteral("-");
    QString enumError;

    if (published) {
        QElapsedTimer waitTimer;
        waitTimer.start();
        while (waitTimer.elapsed() < 5000) {
            QString error;
            const QList<WinEase::Win32::MediaSessionInfo> sessions =
                WinEase::Win32::mediaSessions(&error);
            enumError = error;
            sessionCount = sessions.size();
            for (const WinEase::Win32::MediaSessionInfo &info : sessions) {
                if (matchesProbeSession(info, QString::fromUtf8(kMediaAumid))) {
                    selfSessionId = info.sessionId;
                    selfCurrent = info.current;
                    selfTitle = info.title;
                    selfArtist = info.artist;
                    break;
                }
            }
            if (selfSessionId != QStringLiteral("-")) {
                // 诊断：自己读一次自己的封面（同一进程，但走的是同一条系统链路）
                QString artworkError;
                const QImage selfArtwork =
                    WinEase::Win32::mediaSessionArtwork(selfSessionId, &artworkError);
                selfArtworkNote = selfArtwork.isNull()
                                      ? QStringLiteral("空图：%1").arg(artworkError)
                                      : QStringLiteral("%1x%2 中心像素 %3")
                                            .arg(selfArtwork.width())
                                            .arg(selfArtwork.height())
                                            .arg(selfArtwork.pixelColor(selfArtwork.width() / 2,
                                                                        selfArtwork.height() / 2)
                                                     .name());
                break;
            }
            QThread::msleep(100);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
    }

    {
        QFile file(readyFile);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            return 4;
        }
        QTextStream stream(&file);
        stream << "pid=" << static_cast<unsigned long>(::GetCurrentProcessId()) << "\n";
        stream << "state=" << (published ? QStringLiteral("ready") : QStringLiteral("failed")) << "\n";
        stream << "error=" << publishError << "\n";
        stream << "artworkNote=" << artworkNote << "\n";
        stream << "sessionId=" << selfSessionId << "\n";
        stream << "sessions=" << sessionCount << "\n";
        stream << "current=" << (selfCurrent ? QStringLiteral("yes") : QStringLiteral("no")) << "\n";
        stream << "selfTitle=" << selfTitle << "\n";
        stream << "selfArtist=" << selfArtist << "\n";
        stream << "selfArtwork=" << selfArtworkNote << "\n";
        stream << "enumError=" << enumError << "\n";
        file.flush();
    }

    // 人工跑这一档时（`feature_smoke.exe --media-session <文件>`）把结论直接打到控制台，
    // 免得还要去解析状态文件
    std::printf("[媒体会话探针] 注册=%s 会话数=%d 自己=%s 当前会话=%s 读回的曲目=%s / %s 自读封面=%s\n",
                published ? "成功" : "失败",
                sessionCount,
                selfSessionId.toUtf8().constData(),
                selfCurrent ? "是" : "否",
                selfTitle.toUtf8().constData(),
                selfArtist.toUtf8().constData(),
                selfArtworkNote.toUtf8().constData());
    if (!publishError.isEmpty()) {
        std::printf("[媒体会话探针] 失败原因：%s\n", publishError.toUtf8().constData());
    }
    if (!artworkNote.isEmpty()) {
        std::printf("[媒体会话探针] %s\n", artworkNote.toUtf8().constData());
    }

    // 持有会话直到父进程删掉就绪文件；另加 2 分钟硬超时兜底（与其它探针同一套思路：
    // 父进程被强杀时，自检绝不把自己的残留留在用户的系统媒体列表里）
    QTimer watch;
    QObject::connect(&watch, &QTimer::timeout, &window, [readyFile] {
        if (!QFile::exists(readyFile)) {
            QCoreApplication::quit();
        }
    });
    watch.start(200);
    QTimer::singleShot(120000, &window, [] { QCoreApplication::quit(); });

    return QCoreApplication::exec();
}

FeatureSmoke::MediaSessionProbe::~MediaSessionProbe()
{
    stop();
}

bool FeatureSmoke::MediaSessionProbe::start(QString *errorOut)
{
    static int counter = 0;
    ++counter;
    m_readyFile = QDir::tempPath()
                  + QStringLiteral("/winease_media_probe_%1_%2.txt")
                        .arg(QCoreApplication::applicationPid())
                        .arg(counter);
    QFile::remove(m_readyFile);
    QFile::remove(mediaEventFile(m_readyFile));

    m_process = new QProcess();
    m_process->setProcessChannelMode(QProcess::ForwardedChannels);
    m_process->setProgram(QCoreApplication::applicationFilePath());
    m_process->setArguments({ QStringLiteral("--media-session"), m_readyFile });
    m_process->start();
    if (!m_process->waitForStarted(5000)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("无法启动媒体会话探针子进程：%1").arg(m_process->errorString());
        }
        stop();
        return false;
    }

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 15000) {
        if (QFile::exists(m_readyFile)) {
            const QHash<QString, QString> state = readKeyValueFile(m_readyFile);
            if (state.value(QStringLiteral("state")) == QStringLiteral("ready")) {
                return true;
            }
            // 子进程把失败原因写进状态文件了 —— 如实带出去，不要只说"超时"
            if (errorOut != nullptr) {
                *errorOut = QStringLiteral("媒体会话探针没能注册出会话：%1")
                                .arg(state.value(QStringLiteral("error"),
                                                 QStringLiteral("（没有写原因）")));
            }
            stop();
            return false;
        }
        if (m_process->state() != QProcess::Running) {
            break; // 子进程自己挂了（它会把失败原因打到控制台）
        }
        QThread::msleep(50);
    }

    if (errorOut != nullptr) {
        *errorOut = QStringLiteral("媒体会话探针 15 秒内没有注册出会话");
    }
    stop();
    return false;
}

void FeatureSmoke::MediaSessionProbe::stop()
{
    if (!m_readyFile.isEmpty()) {
        QFile::remove(m_readyFile);
    }
    if (m_process != nullptr) {
        if (!m_process->waitForFinished(3000)) {
            m_process->terminate();
            if (!m_process->waitForFinished(2000)) {
                m_process->kill();
                m_process->waitForFinished(1000);
            }
        }
        m_process->deleteLater();
        m_process = nullptr;
    }
}

bool FeatureSmoke::MediaSessionProbe::isRunning() const
{
    return m_process != nullptr && m_process->state() == QProcess::Running;
}

bool FeatureSmoke::MediaSessionProbe::waitFinished(int timeoutMs)
{
    if (m_process == nullptr) {
        return true;
    }
    return m_process->state() != QProcess::Running || m_process->waitForFinished(timeoutMs);
}

quint32 FeatureSmoke::MediaSessionProbe::pid() const
{
    return (m_process != nullptr) ? static_cast<quint32>(m_process->processId()) : 0;
}

QString FeatureSmoke::MediaSessionProbe::state() const
{
    return readKeyValueFile(m_readyFile).value(QStringLiteral("state"));
}

QString FeatureSmoke::MediaSessionProbe::error() const
{
    return readKeyValueFile(m_readyFile).value(QStringLiteral("error"));
}

QString FeatureSmoke::MediaSessionProbe::sessionId() const
{
    return readKeyValueFile(m_readyFile).value(QStringLiteral("sessionId"));
}

int FeatureSmoke::MediaSessionProbe::sessionCount() const
{
    return readKeyValueFile(m_readyFile).value(QStringLiteral("sessions")).toInt();
}

bool FeatureSmoke::MediaSessionProbe::selfIsCurrent() const
{
    return readKeyValueFile(m_readyFile).value(QStringLiteral("current"))
           == QStringLiteral("yes");
}

QStringList FeatureSmoke::MediaSessionProbe::receivedButtons() const
{
    QStringList buttons;
    QFile file(mediaEventFile(m_readyFile));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return buttons;
    }
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (!line.startsWith(QStringLiteral("button="))) {
            continue;
        }
        buttons << mediaButtonName(line.mid(7).toInt());
    }
    return buttons;
}

QList<qint64> FeatureSmoke::MediaSessionProbe::requestedPositions() const
{
    QList<qint64> positions;
    QFile file(mediaEventFile(m_readyFile));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return positions;
    }
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (!line.startsWith(QStringLiteral("position="))) {
            continue;
        }
        positions << line.mid(9).toLongLong();
    }
    return positions;
}

qint64 FeatureSmoke::MediaSessionProbe::durationMs()
{
    return kMediaDurationMs;
}

QColor FeatureSmoke::MediaSessionProbe::artworkColor()
{
    return kMediaArtworkColor;
}

// ---------------------------------------------------------------------------
//  通用辅助
// ---------------------------------------------------------------------------

bool FeatureSmoke::waitForStable(const std::function<bool()> &sample,
                                 int timeoutMs,
                                 int stableSamples,
                                 int sampleIntervalMs)
{
    if (!sample) {
        return false;
    }
    if (stableSamples < 1) {
        stableSamples = 1;
    }
    if (sampleIntervalMs < 1) {
        sampleIntervalMs = 1;
    }

    QElapsedTimer timer;
    timer.start();
    int stable = 0;
    while (timer.elapsed() < timeoutMs) {
        // ★ 每次采样之间都把 Qt 事件处理一遍：这些状态大多是**别的进程**改的，
        //   而我们这边靠排队投递的钩子回调感知它们 —— 不转事件就等于闭着眼采样
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (sample()) {
            if (++stable >= stableSamples) {
                return true;
            }
        } else {
            stable = 0; // 被打断就得重新数（这正是"连续"的含义）
        }
        QThread::msleep(static_cast<unsigned long>(sampleIntervalMs));
    }
    return false;
}

bool FeatureSmoke::waitForForeground(WinEase::Win32::WindowHandle expected,
                                     int timeoutMs,
                                     int stableSamples,
                                     int sampleIntervalMs)
{
    if (expected == nullptr) {
        return false;
    }
    return waitForStable([expected] { return WinEase::Win32::foregroundWindow() == expected; },
                         timeoutMs,
                         stableSamples,
                         sampleIntervalMs);
}

bool FeatureSmoke::waitForResolvedTarget(WinEase::Win32::WindowTargetMode mode,
                                         WinEase::Win32::WindowHandle expected,
                                         int timeoutMs,
                                         int stableSamples,
                                         int sampleIntervalMs)
{
    if (expected == nullptr) {
        return false;
    }
    return waitForStable(
        [mode, expected] {
            return WinEase::Win32::resolveWindowTarget(mode, nullptr) == expected;
        },
        timeoutMs,
        stableSamples,
        sampleIntervalMs);
}

void FeatureSmoke::settleEvents(int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

bool FeatureSmoke::waitFor(const std::function<bool()> &predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        // 转一次事件循环：WinEvent 钩子的回调是通过线程消息投递的，
        // 不转事件循环的话"置底维持"这类逻辑根本没机会执行
        QCoreApplication::processEvents();
        if (predicate()) {
            return true;
        }
        QThread::msleep(40);
    }
    QCoreApplication::processEvents();
    return predicate();
}

bool FeatureSmoke::moveCursorTo(const QPoint &physicalPoint)
{
    // 真实桌面里鼠标可能正被外部移动（人/触摸板）→ 重试几次；
    // 一直失败就把"光标没到位"如实报成前置条件不成立，绝不静默继续
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (::SetCursorPos(physicalPoint.x(), physicalPoint.y()) != FALSE) {
            QCoreApplication::processEvents();
            QThread::msleep(40);
            QCoreApplication::processEvents();
            if (WinEase::Win32::cursorPosition() == physicalPoint) {
                return true;
            }
        }
    }
    return false;
}

QString FeatureSmoke::createWallpaperFixture(const QString &parentDir, QString *errorOut)
{
    const QString folder = QDir(parentDir).filePath(QStringLiteral("wallpapers"));
    if (!QDir().mkpath(folder)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("无法创建目录：%1").arg(folder);
        }
        return QString();
    }

    // 三张一眼能区分的纯色图；文件名按序排列，便于断言"按序轮换"
    const QColor colors[3] = { QColor(0x11, 0x22, 0x33),
                               QColor(0x44, 0x55, 0x66),
                               QColor(0x77, 0x88, 0x99) };
    for (int index = 0; index < 3; ++index) {
        QImage image(64, 64, QImage::Format_RGB32);
        image.fill(colors[index]);
        const QString path = QDir(folder).filePath(QStringLiteral("%1_winease.bmp").arg(index + 1));
        if (!image.save(path, "BMP")) {
            if (errorOut != nullptr) {
                *errorOut = QStringLiteral("写图片失败：%1").arg(path);
            }
            return QString();
        }
    }
    if (errorOut != nullptr) {
        errorOut->clear();
    }
    return folder;
}

bool FeatureSmoke::hasExStyle(WinEase::Win32::WindowHandle hwnd, LONG_PTR style)
{
    if (!WinEase::Win32::isValidWindow(hwnd)) {
        return false;
    }
    return (::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & style) != 0;
}

int FeatureSmoke::firstNumberAfter(const QString &text, const QString &marker)
{
    const int markerIndex = text.indexOf(marker);
    if (markerIndex < 0) {
        return INT_MIN;
    }
    int index = markerIndex + marker.size();
    while (index < text.size() && !text.at(index).isDigit()) {
        ++index;
    }
    int value = 0;
    bool any = false;
    while (index < text.size() && text.at(index).isDigit()) {
        value = value * 10 + text.at(index).digitValue();
        any = true;
        ++index;
    }
    return any ? value : INT_MIN;
}

bool FeatureSmoke::rectsClose(const QRect &actual, const QRect &expected, int tolerance)
{
    return std::abs(actual.left() - expected.left()) <= tolerance
           && std::abs(actual.top() - expected.top()) <= tolerance
           && std::abs(actual.width() - expected.width()) <= tolerance
           && std::abs(actual.height() - expected.height()) <= tolerance;
}

QString FeatureSmoke::rectText(const QRect &rect)
{
    return QStringLiteral("(%1,%2 %3x%4)")
        .arg(rect.left())
        .arg(rect.top())
        .arg(rect.width())
        .arg(rect.height());
}

int FeatureSmoke::zOrderIndexOf(WinEase::Win32::WindowHandle hwnd)
{
    // GetTopWindow(NULL) 拿到 z 序最上层的顶层窗口，GW_HWNDNEXT 依次向下，
    // 顺序是文档保证的（EnumWindows 的枚举顺序反而没有官方保证）
    int index = 0;
    for (HWND current = ::GetTopWindow(nullptr); current != nullptr;
         current = ::GetWindow(current, GW_HWNDNEXT)) {
        if (::IsWindowVisible(current) == FALSE) {
            continue;
        }
        if (current == hwnd) {
            return index;
        }
        ++index;
    }
    return -1;
}

bool FeatureSmoke::isNoActivate(WinEase::Win32::WindowHandle hwnd)
{
    if (!WinEase::Win32::isValidWindow(hwnd)) {
        return false;
    }
    return (::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_NOACTIVATE) != 0;
}

QString FeatureSmoke::handleText(WinEase::Win32::WindowHandle hwnd)
{
    return QStringLiteral("0x%1").arg(reinterpret_cast<quintptr>(hwnd), 0, 16);
}

// ---------------------------------------------------------------------------
//  StatusLog / dispatchAction（窗口组与工具组共用）
// ---------------------------------------------------------------------------

void FeatureSmoke::StatusLog::attach(WinEase::IFeaturePlugin *plugin)
{
    // 上下文对象用 plugin 本身：插件被卸载时连接随之消失，不会留下悬空捕获
    QObject::connect(plugin, &WinEase::IFeaturePlugin::statusMessage, plugin,
                     [this](const QString &message) { messages.append(message); });
}

QString FeatureSmoke::StatusLog::last() const
{
    return messages.isEmpty() ? QString() : messages.last();
}

bool FeatureSmoke::StatusLog::contains(const QString &needle) const
{
    for (const QString &message : messages) {
        if (message.contains(needle)) {
            return true;
        }
    }
    return false;
}

bool FeatureSmoke::dispatchAction(WinEase::PluginManager &manager,
                                  const QString &pluginId,
                                  const QString &action)
{
    return manager.dispatchHotkey(pluginId + QStringLiteral("::") + action);
}

// ---------------------------------------------------------------------------
//  ClipboardGuard（工具组用例都要写剪贴板 → 必须能还原）
// ---------------------------------------------------------------------------

FeatureSmoke::ClipboardGuard::ClipboardGuard()
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        return;
    }
    const QMimeData *mime = clipboard->mimeData();
    m_hadText = (mime != nullptr) && mime->hasText();
    m_original = clipboard->text();
}

FeatureSmoke::ClipboardGuard::~ClipboardGuard()
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        return;
    }
    if (m_hadText) {
        clipboard->setText(m_original);
    } else {
        // 原来就没有文本 → 清掉，别给用户留一段自检产物
        clipboard->clear();
    }
    QCoreApplication::processEvents();
}

void FeatureSmoke::ClipboardGuard::set(const QString &text) const
{
    if (QClipboard *clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(text);
        QCoreApplication::processEvents();
    }
}

QString FeatureSmoke::clipboardText()
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    return (clipboard != nullptr) ? clipboard->text() : QString();
}

// ---------------------------------------------------------------------------
//  AudioVolumeGuard（输入组 P2-05 用例要真的改系统音量）
// ---------------------------------------------------------------------------

FeatureSmoke::AudioVolumeGuard::AudioVolumeGuard()
{
    WinEase::Win32::AudioEndpoint endpoint =
        WinEase::Win32::AudioEndpoint::defaultEndpoint(WinEase::Win32::AudioDirection::Render,
                                                       &m_error);
    if (!endpoint.isValid()) {
        return;
    }
    m_volume = endpoint.volume(&m_error);
    if (m_volume < 0.0f) {
        m_volume = -1.0f;
        return;
    }
    m_muted = endpoint.isMuted();
    m_valid = true;
}

FeatureSmoke::AudioVolumeGuard::~AudioVolumeGuard()
{
    if (!m_valid) {
        return;
    }
    WinEase::Win32::AudioEndpoint endpoint =
        WinEase::Win32::AudioEndpoint::defaultEndpoint(WinEase::Win32::AudioDirection::Render);
    if (!endpoint.isValid()) {
        return; // 设备没了（用户拔了耳机）→ 无处可还，如实留给调用方在日志里看到
    }
    endpoint.setVolume(m_volume);
    endpoint.setMuted(m_muted);
    QCoreApplication::processEvents();
}

// ---------------------------------------------------------------------------
//  SolidColorWindow（取色用例的"标准色卡"）
// ---------------------------------------------------------------------------

FeatureSmoke::SolidColorWindow::SolidColorWindow(const QColor &color,
                                                 const QPoint &logicalPosition,
                                                 bool topMost)
    : m_color(color)
{
    auto *widget = new QWidget();
    m_widget = widget;
    // Tool：不进任务栏；Frameless：没有标题栏和边框，整个矩形就是一个颜色；
    // StaysOnTop：保证屏幕上那个点**真的**是这个颜色（否则取到的是遮挡物的颜色）。
    // 焦点高亮用例需要"被悬浮层压住"的背景板 → 传 topMost=false
    Qt::WindowFlags flags = Qt::Tool | Qt::FramelessWindowHint;
    if (topMost) {
        flags |= Qt::WindowStaysOnTopHint;
    }
    widget->setWindowFlags(flags);
    widget->setAttribute(Qt::WA_ShowWithoutActivating, true);
    widget->setAutoFillBackground(true);
    QPalette palette = widget->palette();
    palette.setColor(QPalette::Window, color);
    widget->setPalette(palette);
    widget->resize(160, 160);
    widget->move(logicalPosition);
    widget->show();

    // show() 是异步的：等窗口真的上屏再让用例去取色
    for (int attempt = 0; attempt < 30 && !WinEase::Win32::isValidWindow(handle()); ++attempt) {
        QCoreApplication::processEvents();
        QThread::msleep(20);
    }
    QCoreApplication::processEvents();
    QThread::msleep(120); // 等 DWM 合成一帧
    QCoreApplication::processEvents();
}

FeatureSmoke::SolidColorWindow::~SolidColorWindow()
{
    if (m_widget != nullptr) {
        m_widget->hide();
        delete m_widget;
        m_widget = nullptr;
        QCoreApplication::processEvents();
    }
}

WinEase::Win32::WindowHandle FeatureSmoke::SolidColorWindow::handle() const
{
    if (m_widget == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<HWND>(m_widget->winId());
}

QRect FeatureSmoke::SolidColorWindow::physicalRect() const
{
    // 无边框窗口没有不可见投影边框（没有 WS_THICKFRAME），
    // 因此窗口矩形就是屏幕上看到的那个矩形 —— 这里用它是精确的
    return WinEase::Win32::windowRect(handle());
}

QPoint FeatureSmoke::SolidColorWindow::physicalCenter() const
{
    return physicalRect().center();
}

// ---------------------------------------------------------------------------
//  LoopbackListener（端口占用用例的真实占用源）
// ---------------------------------------------------------------------------

namespace {

/// WSAStartup 全局一次即可；进程退出由系统回收，不调用 WSACleanup
bool ensureWinsock()
{
    static const bool ready = [] {
        WSADATA data{};
        return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ready;
}

} // namespace

FeatureSmoke::LoopbackListener::LoopbackListener() = default;

FeatureSmoke::LoopbackListener::~LoopbackListener()
{
    if (m_socket != kInvalidSocket) {
        ::closesocket(static_cast<SOCKET>(m_socket));
    }
}

bool FeatureSmoke::LoopbackListener::listenTcp()
{
    if (!ensureWinsock()) {
        return false;
    }
    const SOCKET handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == INVALID_SOCKET) {
        return false;
    }
    m_socket = static_cast<quintptr>(handle);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0; // 端口交给系统分配：不用猜"哪个端口空着"
    if (::bind(handle, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        return false;
    }
    if (::listen(handle, 4) != 0) {
        return false;
    }

    int length = static_cast<int>(sizeof(address));
    if (::getsockname(handle, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
        return false;
    }
    m_port = ntohs(address.sin_port);
    return m_port != 0;
}

bool FeatureSmoke::LoopbackListener::bindUdp()
{
    if (!ensureWinsock()) {
        return false;
    }
    const SOCKET handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == INVALID_SOCKET) {
        return false;
    }
    m_socket = static_cast<quintptr>(handle);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(handle, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        return false;
    }

    int length = static_cast<int>(sizeof(address));
    if (::getsockname(handle, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
        return false;
    }
    m_port = ntohs(address.sin_port);
    return m_port != 0;
}
