#include "device_group.h"

#include "DeviceUsageMonitor.h"

#include "win32/RegistryUtils.h"

#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QElapsedTimer>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QThread>
#include <QWidget>

namespace FeatureSmoke {

namespace {

using WinEase::Common::DeviceKind;
using WinEase::Common::DeviceUsageEntry;
using WinEase::Common::RawUsage;
using WinEase::Win32::RegistryKey;
using WinEase::Win32::RegistryRoot;
using WinEase::Win32::RegistryValueType;
using WinEase::Win32::RegistryView;

const QString kAlertId = QStringLiteral("security.device_alert");

// 面板控件对象名（跨 DLL 边界只能靠运行期元对象找，见踩坑 #18）
const QString kAlertPanelName = QStringLiteral("deviceAlertPanel");
const QString kAlertStatusLabel = QStringLiteral("deviceAlertStatusLabel");
const QString kAlertDetailLabel = QStringLiteral("deviceAlertDetailLabel");
const QString kAlertApprovedList = QStringLiteral("deviceAlertApprovedList");
const QString kAlertApproveButton = QStringLiteral("deviceAlertApproveButton");
const QString kAlertForgetButton = QStringLiteral("deviceAlertForgetButton");
const QString kAlertWebcamCheck = QStringLiteral("deviceAlertWebcamCheck");
const QString kAlertMicCheck = QStringLiteral("deviceAlertMicrophoneCheck");
const QString kAlertNotifyCheck = QStringLiteral("deviceAlertNotifyCheck");
const QString kAlertPollSpin = QStringLiteral("deviceAlertPollSpin");

/// 自检专用夹具的 exe 路径（编码成键名后写进 ConsentStore）
const QString kFixtureAppPath = QStringLiteral("C:\\WinEaseSmoke\\camera_smoke.exe");
/// FILETIME 与 Unix 纪元之间的 100ns 差
constexpr qulonglong kFileTimeUnixEpochDiff = 116444736000000000ULL;

qulonglong unixMsToFileTime(qint64 unixMs)
{
    return static_cast<qulonglong>(unixMs) * 10000ULL + kFileTimeUnixEpochDiff;
}

/// 静默等一会儿（照常转事件循环）
void settle(int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

template <typename T>
T *childNamed(QWidget *root, const QString &objectName)
{
    return root != nullptr ? root->findChild<T *>(objectName) : nullptr;
}

QString shorten(const QString &text, int limit = 140)
{
    return text.size() <= limit ? text : text.left(limit) + QStringLiteral("…");
}

// ===========================================================================
//  注册表夹具：在系统隐私页面那份数据里造一条"正在使用"的记录
//
//  ⚠ 这是**写用户真实注册表**（HKCU）。系统「隐私和安全性」页面在这期间会短暂看到
//     这个假条目 —— 所以：① 只写两个时间戳、**不写 Value**（不碰权限语义）；
//     ② 用例结束立刻删除并**断言不留残留**；③ 析构里也删（中途失败也不留垃圾）。
//
//  ⚠ 开始时间刻意写在**未来 1 分钟**：这样"正在使用的条目按开始时间倒序"时它排最前，
//     "批准当前应用"按钮一定作用在它身上 —— 断言不会因为本机恰好有别的应用在用摄像头
//     而变得不确定（那样就不是"前置不成立"，而是测试自己不可靠了）。
// ===========================================================================

class ConsentFixture
{
public:
    ConsentFixture(DeviceKind kind, const QString &appPath)
        : m_kind(kind)
    {
        m_keyName = WinEase::Common::keyNameFromAppPath(appPath);
        m_path = WinEase::Common::consentStorePath(kind, true)
                 + QLatin1Char('\\') + m_keyName;

        QString error;
        RegistryKey key = RegistryKey::create(RegistryRoot::CurrentUser, m_path,
                                              RegistryView::Default, &error);
        if (!key.isValid()) {
            m_error = error;
            return;
        }

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const qulonglong startTime = unixMsToFileTime(nowMs + 60 * 1000);
        if (!key.setValue(QStringLiteral("LastUsedTimeStart"), startTime,
                          RegistryValueType::QWord, &error)
            || !key.setValue(QStringLiteral("LastUsedTimeStop"), static_cast<qulonglong>(0),
                             RegistryValueType::QWord, &error)) {
            m_error = error;
            return;
        }
        m_created = true;
    }

    ~ConsentFixture() { remove(); }

    bool created() const { return m_created; }
    QString keyName() const { return m_keyName; }
    QString path() const { return m_path; }
    QString error() const { return m_error; }

    /// 读回"停止时间"，确认夹具真的写进去了
    qulonglong readStopTime() const
    {
        RegistryKey key = RegistryKey::open(RegistryRoot::CurrentUser, m_path);
        if (!key.isValid()) {
            return ~0ULL;
        }
        return key.value(QStringLiteral("LastUsedTimeStop")).toULongLong();
    }

    /// 把这条记录改成"已经停止使用"（= 应用关掉了摄像头）
    bool markStopped(QString *errorOut = nullptr)
    {
        RegistryKey key = RegistryKey::open(RegistryRoot::CurrentUser, m_path, false);
        if (!key.isValid()) {
            if (errorOut != nullptr) {
                *errorOut = key.errorMessage();
            }
            return false;
        }
        return key.setValue(QStringLiteral("LastUsedTimeStop"),
                            unixMsToFileTime(QDateTime::currentMSecsSinceEpoch()),
                            RegistryValueType::QWord,
                            errorOut);
    }

    bool exists() const
    {
        return WinEase::Win32::keyExists(RegistryRoot::CurrentUser, m_path);
    }

    bool remove()
    {
        if (!m_created) {
            return true;
        }
        m_created = false;
        return WinEase::Win32::deleteKeyRecursively(RegistryRoot::CurrentUser, m_path, RegistryView::Default);
    }

private:
    DeviceKind m_kind = DeviceKind::Webcam;
    QString m_keyName;
    QString m_path;
    QString m_error;
    bool m_created = false;
};

// ===========================================================================
//  用例
// ===========================================================================

void runDeviceAlertCase(Reporter &reporter, WinEase::PluginManager &manager, StubServices &services)
{
    // ---- ① 纯函数：键名编解码（不还原成路径，界面上就是一串天书）----
    reporter.check(WinEase::Common::keyNameFromAppPath(kFixtureAppPath)
                       == QStringLiteral("C:#WinEaseSmoke#camera_smoke.exe"),
                   QStringLiteral("P2-11 exe 路径 → 注册表键名（`\\` 换成 `#`）"),
                   WinEase::Common::keyNameFromAppPath(kFixtureAppPath));
    reporter.check(WinEase::Common::appPathFromKeyName(QStringLiteral("C:#WinEaseSmoke#camera_smoke.exe"))
                       == kFixtureAppPath,
                   QStringLiteral("P2-11 键名 → exe 路径往返一致"));
    reporter.check(WinEase::Common::displayNameForAppKey(QStringLiteral("C:#a#b#cam.exe"))
                       == QStringLiteral("cam.exe"),
                   QStringLiteral("P2-11 展示名是可执行文件名（不是整条路径）"));
    reporter.check(WinEase::Common::displayNameForAppKey(
                       QStringLiteral("Microsoft.WindowsCamera_8wekyb3d8bbwe"))
                       == QStringLiteral("Microsoft.WindowsCamera_8wekyb3d8bbwe"),
                   QStringLiteral("P2-11 包标识型条目（没有路径分隔符）原样显示"));

    // ---- ② 纯函数："正在使用"的判据 + FILETIME 换算 ----
    reporter.check(!WinEase::Common::usageInProgress(0, 0),
                   QStringLiteral("P2-11 ★「从没用过」的空键**不算**正在使用"
                                  "（算错了用户一启用就被假警报淹没）"));
    reporter.check(WinEase::Common::usageInProgress(12345, 0),
                   QStringLiteral("P2-11 「开始非 0、停止为 0」= 正在使用"));
    reporter.check(!WinEase::Common::usageInProgress(12345, 67890),
                   QStringLiteral("P2-11 「已经停止」不算正在使用"));

    const qint64 sampleMs = QDateTime::currentMSecsSinceEpoch();
    reporter.check(WinEase::Common::fileTimeToUnixMs(unixMsToFileTime(sampleMs)) == sampleMs,
                   QStringLiteral("P2-11 FILETIME ⇄ Unix 毫秒往返一致"),
                   QStringLiteral("%1 ms").arg(sampleMs));
    reporter.check(WinEase::Common::fileTimeToUnixMs(0) == -1,
                   QStringLiteral("P2-11 FILETIME 为 0（从没用过）→ -1，不是 1970 年"));

    // ---- ③ 纯函数：条目构建、排序与警报筛选 ----
    const QDateTime now = QDateTime::currentDateTime();
    RawUsage neverUsed;
    neverUsed.keyName = QStringLiteral("C:#ghost.exe");
    RawUsage oldApp;
    oldApp.keyName = QStringLiteral("C:#old.exe");
    oldApp.lastUsedTimeStart = unixMsToFileTime(now.addSecs(-3600).toMSecsSinceEpoch());
    oldApp.lastUsedTimeStop = unixMsToFileTime(now.addSecs(-1800).toMSecsSinceEpoch());
    RawUsage liveApp;
    liveApp.keyName = QStringLiteral("C:#live.exe");
    liveApp.lastUsedTimeStart = unixMsToFileTime(now.toMSecsSinceEpoch());
    liveApp.lastUsedTimeStop = 0;

    const QList<DeviceUsageEntry> entries = WinEase::Common::buildUsageEntries(
        { neverUsed, oldApp, liveApp }, { QStringLiteral("C:#old.exe") }, now);
    reporter.check(entries.size() == 2,
                   QStringLiteral("P2-11 从没用过的空键不进列表（只留真有记录的）"),
                   QStringLiteral("剩 %1 条").arg(entries.size()));
    reporter.check(!entries.isEmpty() && entries.first().keyName == QStringLiteral("C:#live.exe")
                       && entries.first().inUse,
                   QStringLiteral("P2-11 正在使用的排在最前"),
                   entries.isEmpty() ? QString() : entries.first().keyName);
    reporter.check(entries.size() > 1 && entries.at(1).approved
                       && entries.at(1).keyName == QStringLiteral("C:#old.exe"),
                   QStringLiteral("P2-11 「已批准」标记来自名单（键名匹配）"));

    const QList<DeviceUsageEntry> alerts = WinEase::Common::alertEntries(entries);
    reporter.check(alerts.size() == 1 && alerts.first().keyName == QStringLiteral("C:#live.exe"),
                   QStringLiteral("P2-11 警报 = 正在使用 **且** 未批准（已批准的不再打扰）"),
                   QStringLiteral("警报 %1 条").arg(alerts.size()));
    reporter.check(WinEase::Common::alertSummary(DeviceKind::Webcam, alerts)
                       .contains(QStringLiteral("live.exe")),
                   QStringLiteral("P2-11 警报文案里带应用名（「哪个应用」才是用户要的信息）"),
                   WinEase::Common::alertSummary(DeviceKind::Webcam, alerts));
    reporter.check(WinEase::Common::deviceBadgeText({ DeviceKind::Webcam }) == QStringLiteral("摄")
                       && WinEase::Common::deviceBadgeText({ DeviceKind::Webcam,
                                                             DeviceKind::Microphone })
                              == QStringLiteral("摄麦")
                       && WinEase::Common::deviceBadgeText({}).isEmpty(),
                   QStringLiteral("P2-11 托盘徽标文字按设备组合（摄/麦/摄麦/空）"));

    // ---- ④ 夹具：造一条"摄像头正在被 camera_smoke.exe 使用"的记录 ----
    ConsentFixture fixture(DeviceKind::Webcam, kFixtureAppPath);
    reporter.check(fixture.created(),
                   QStringLiteral("P2-11 前置：已在 ConsentStore 里造好自检专用条目"),
                   fixture.error());
    if (!fixture.created()) {
        return;
    }
    reporter.check(fixture.readStopTime() == 0,
                   QStringLiteral("P2-11 前置：夹具的 LastUsedTimeStop 读回为 0（= 正在使用）"),
                   QStringLiteral("读回 %1").arg(fixture.readStopTime()));

    // ---- ⑤ 插件：启用后 5 秒内就应当提醒 ----
    WinEase::IFeaturePlugin *plugin = manager.plugin(kAlertId);
    reporter.check(plugin != nullptr,
                   QStringLiteral("P2-11 插件已从插件目录加载（security.device_alert）"));
    if (plugin == nullptr) {
        fixture.remove();
        return;
    }

    QWidget *settings = plugin->createSettingsWidget(nullptr);
    reporter.check(settings != nullptr && settings->objectName() == kAlertPanelName,
                   QStringLiteral("P2-11 设置面板可创建且对象名正确"));
    if (settings == nullptr) {
        fixture.remove();
        return;
    }

    auto *statusLabel = childNamed<QLabel>(settings, kAlertStatusLabel);
    auto *detailLabel = childNamed<QLabel>(settings, kAlertDetailLabel);
    auto *approvedList = childNamed<QListWidget>(settings, kAlertApprovedList);
    auto *approveButton = childNamed<QPushButton>(settings, kAlertApproveButton);
    auto *forgetButton = childNamed<QPushButton>(settings, kAlertForgetButton);
    auto *webcamCheck = childNamed<QCheckBox>(settings, kAlertWebcamCheck);
    auto *micCheck = childNamed<QCheckBox>(settings, kAlertMicCheck);
    auto *notifyCheck = childNamed<QCheckBox>(settings, kAlertNotifyCheck);
    auto *pollSpin = childNamed<QSpinBox>(settings, kAlertPollSpin);

    reporter.check(statusLabel != nullptr && detailLabel != nullptr && approvedList != nullptr
                       && approveButton != nullptr && forgetButton != nullptr
                       && webcamCheck != nullptr && micCheck != nullptr && notifyCheck != nullptr
                       && pollSpin != nullptr,
                   QStringLiteral("P2-11 面板控件按对象名全部找到"));
    if (statusLabel == nullptr || approveButton == nullptr || approvedList == nullptr
        || forgetButton == nullptr) {
        delete settings;
        fixture.remove();
        return;
    }

    const auto cleanup = [&]() {
        manager.setPluginEnabled(kAlertId, false);
        delete settings;
        fixture.remove();
    };

    reporter.check(pollSpin->value() == 1,
                   QStringLiteral("P2-11 轮询间隔来自配置（预设 1 秒）"),
                   QStringLiteral("面板显示 %1 秒").arg(pollSpin->value()));
    reporter.check(webcamCheck->isChecked(),
                   QStringLiteral("P2-11 监视摄像头默认开着（这是本功能的本体）"));
    reporter.check(!micCheck->isChecked(),
                   QStringLiteral("P2-11 本组预设只开摄像头（关掉麦克风监视，"
                                  "避免本机真实麦克风使用干扰断言）"),
                   QStringLiteral("面板显示 = %1")
                       .arg(micCheck->isChecked() ? QStringLiteral("开") : QStringLiteral("关")));

    const int notificationsBefore = services.notificationCount();
    reporter.check(manager.setPluginEnabled(kAlertId, true),
                   QStringLiteral("P2-11 插件启用成功"), plugin->lastError());

    // 启用即立刻检查一次（用户往往是"发现摄像头亮了才想起来装个提醒"）
    const bool alerted = waitFor(
        [&services] { return !services.trayBadgeText(kAlertId).isEmpty(); },
        5000);
    reporter.check(alerted,
                   QStringLiteral("P2-11 ★ 检测到「正在使用」→ 5 秒内亮起托盘徽标"),
                   QStringLiteral("徽标 = 「%1」").arg(services.trayBadgeText(kAlertId)));
    reporter.check(services.trayBadgeText(kAlertId) == QStringLiteral("摄"),
                   QStringLiteral("P2-11 只监视摄像头 → 徽标只写「摄」"),
                   services.trayBadgeText(kAlertId));
    // ⚠ 给足机会再断言（"某件事发生了"和"它已经轮到"是两件事）：
    //    启用瞬间的那次检查排在事件循环下一轮，通知也因此晚一拍
    const bool notified = waitFor(
        [&services, notificationsBefore] {
            return services.notificationCount() > notificationsBefore;
        },
        5000);
    reporter.check(notified,
                   QStringLiteral("P2-11 弹了气泡提示"),
                   QStringLiteral("通知数 %1 → %2")
                       .arg(notificationsBefore)
                       .arg(services.notificationCount()));
    reporter.check(statusLabel->text().contains(QStringLiteral("camera_smoke.exe")),
                   QStringLiteral("P2-11 状态里点出**是哪个应用**在用摄像头"),
                   shorten(statusLabel->text()));
    reporter.check(detailLabel->text().contains(QStringLiteral("camera_smoke.exe")),
                   QStringLiteral("P2-11 明细里列出这条记录"),
                   shorten(detailLabel->text()));
    reporter.check(approveButton->isEnabled()
                       && approveButton->text().contains(QStringLiteral("camera_smoke.exe")),
                   QStringLiteral("P2-11 「批准」按钮直接写出它会批准谁（不用用户猜）"),
                   approveButton->text());

    // ---- ⑥ 同一个应用只提醒一次（3 秒一轮的轮询不能变成骚扰）----
    const int notificationsAfterFirstAlert = services.notificationCount();
    settle(2500); // 至少又轮询了两轮
    reporter.check(services.notificationCount() == notificationsAfterFirstAlert,
                   QStringLiteral("P2-11 同一个应用**只弹一次**（每轮都弹的话用户第一件事"
                                  "就是把这个功能关掉）"),
                   QStringLiteral("通知数 %1 → %2")
                       .arg(notificationsAfterFirstAlert)
                       .arg(services.notificationCount()));
    reporter.check(!services.trayBadgeText(kAlertId).isEmpty(),
                   QStringLiteral("P2-11 但仍持续使用中（徽标保持亮着）"));

    // ---- ⑦ 记住已批准：提示解除，但明细里仍看得见 ----
    approveButton->click();
    settle(400);
    const QStringList approved = services
                                     .configValue(kAlertId, QStringLiteral("approved"),
                                                  QStringList())
                                     .toStringList();
    reporter.check(approved.contains(fixture.keyName()),
                   QStringLiteral("P2-11 点「批准」→ 名单落到配置里（存的是注册表键名）"),
                   approved.join(QStringLiteral(",")));
    reporter.check(services.trayBadgeText(kAlertId).isEmpty(),
                   QStringLiteral("P2-11 批准之后徽标熄灭（已批准的应用不再打扰）"),
                   services.trayBadgeText(kAlertId));
    reporter.check(statusLabel->text().contains(QStringLiteral("没有应用在使用")),
                   QStringLiteral("P2-11 状态如实变成「没有应用在使用」"),
                   shorten(statusLabel->text()));
    reporter.check(detailLabel->text().contains(QStringLiteral("已批准")),
                   QStringLiteral("P2-11 已批准的应用仍在明细里看得见（看得到但不打扰）"),
                   shorten(detailLabel->text()));
    reporter.check(approvedList->count() == 1,
                   QStringLiteral("P2-11 名单列表里出现这一条"),
                   QStringLiteral("列表 %1 项").arg(approvedList->count()));

    // ---- ⑧ 移出名单 = 用户反悔了，提醒要回来 ----
    const int notificationsBeforeForget = services.notificationCount();
    approvedList->setCurrentRow(0);
    forgetButton->click();
    settle(600);
    reporter.check(services
                       .configValue(kAlertId, QStringLiteral("approved"), QStringList())
                       .toStringList()
                       .isEmpty(),
                   QStringLiteral("P2-11 点「移出名单」→ 配置里的名单被清空"));
    reporter.check(!services.trayBadgeText(kAlertId).isEmpty(),
                   QStringLiteral("P2-11 移出名单后提醒重新亮起"));
    reporter.check(services.notificationCount() > notificationsBeforeForget,
                   QStringLiteral("P2-11 移出名单同时也清掉了「已提醒过」标记"
                                  "（否则会亮徽标却再也不弹通知，用户以为移出没用）"),
                   QStringLiteral("通知数 %1 → %2")
                       .arg(notificationsBeforeForget)
                       .arg(services.notificationCount()));

    // ---- ⑨ 验收原文：关掉之后提示要**自动解除** ----
    QString stopError;
    reporter.check(fixture.markStopped(&stopError),
                   QStringLiteral("P2-11 前置：把夹具记录改成「已经停止使用」"), stopError);
    const bool cleared = waitFor(
        [&services] { return services.trayBadgeText(kAlertId).isEmpty(); },
        5000);
    reporter.check(cleared,
                   QStringLiteral("P2-11 ★ 应用停用摄像头后提示自动解除（徽标熄灭）"),
                   QStringLiteral("徽标 = 「%1」").arg(services.trayBadgeText(kAlertId)));
    reporter.check(statusLabel->text().contains(QStringLiteral("没有应用在使用")),
                   QStringLiteral("P2-11 状态文本同步回到「没有应用在使用」"),
                   shorten(statusLabel->text()));

    // ---- ⑩ 停用：不再轮询、徽标摘掉 ----
    reporter.check(manager.setPluginEnabled(kAlertId, false),
                   QStringLiteral("P2-11 插件停用成功"));
    settle(300);
    reporter.check(services.trayBadgeText(kAlertId).isEmpty(),
                   QStringLiteral("P2-11 停用后徽标被清掉"));
    const int notificationsAfterDisable = services.notificationCount();
    settle(2500);
    reporter.check(services.notificationCount() == notificationsAfterDisable,
                   QStringLiteral("P2-11 停用后连轮询都停了（不再有任何新提示）"));

    cleanup();

    // ---- ⑪ 收尾：注册表里不留任何残留 ----
    reporter.check(!fixture.exists(),
                   QStringLiteral("P2-11 收尾：自检专用条目已从注册表删除（不留残留）"));
    const WinEase::Common::ConsentStoreSnapshot snapshot =
        WinEase::Common::readConsentStore(DeviceKind::Webcam);
    bool fixtureGone = true;
    for (const RawUsage &usage : snapshot.entries) {
        if (usage.keyName == fixture.keyName()) {
            fixtureGone = false;
        }
    }
    reporter.check(snapshot.ok && fixtureGone,
                   QStringLiteral("P2-11 收尾：再读一遍 ConsentStore，确认列表里没有那条假记录"),
                   snapshot.error);
}

} // namespace

int runDeviceGroupTests(Reporter &reporter,
                        WinEase::PluginManager &manager,
                        StubServices &services)
{
    const int failuresAtStart = reporter.failures();

    reporter.info(QStringLiteral("说明：本组会在注册表里**临时**造一条自检专用的"
                                 "「摄像头正在被使用」记录（ConsentStore/NonPackaged），"
                                 "用例结束立刻删除并断言不留残留；本插件对注册表只读"));

    runDeviceAlertCase(reporter, manager, services);

    reporter.info(QStringLiteral("安全与隐私组失败项：%1").arg(reporter.failures() - failuresAtStart));
    return reporter.failures() - failuresAtStart;
}

} // namespace FeatureSmoke
