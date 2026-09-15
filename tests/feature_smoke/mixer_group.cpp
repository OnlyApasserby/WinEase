// ============================================================================
//  mixer_group.cpp —— P3-09 音量混合器 的端到端自检
//
//  自检纪律在本组的具体落法：
//   1) **断言真实外部状态，不看返回值**。插件说"调好了"不算数，一律回
//      Win32::audioSessions() 逐份读真实音量/静音，外加一条分界线：
//      **调会话音量绝不能动系统主音量**（会话级与端点级的分水岭）。
//   2) **"被测对象"必须是真的**。自检自己起子进程当播放器（--audio-session），
//      两个不同进程、同一个进程名 → 两份真实会话，专门覆盖"一个应用多份会话"
//      这条最容易写错的路径（按应用调音量必须覆盖该进程**全部**会话）。
//   3) **前置条件断言出来**，不成立就如实失败并返回，绝不静默跳过。
//   4) **不留痕**：探针会话由自检自己建、自己收；动过的音量在收尾处逐份复原。
// ============================================================================

#include "mixer_group.h"

#include "MixerModel.h"

#include "app/core/PluginManager.h"
#include "sdk/IFeaturePlugin.h"
#include "win32/AudioSessions.h"
#include "win32/CoreAudio.h"

#include <QCheckBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSet>
#include <QSlider>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace FeatureSmoke {
namespace {

using WinEase::FeaturePlugins::Mixer::MixerOptions;
using WinEase::FeaturePlugins::Mixer::MixerRow;
using WinEase::FeaturePlugins::Mixer::MixerRowKind;
using WinEase::FeaturePlugins::Mixer::MixerSessionInput;
using WinEase::FeaturePlugins::Mixer::MixerSnapshot;
using WinEase::Win32::AudioSessionInfo;

const QString kPluginId = QStringLiteral("media.mixer");

/// 子进程探针的进程名 = 本 exe 的名字。两个探针会被聚合进**同一行**，
/// 这正是本组最想验的场景（一个应用多份会话）。
const QString kProbeRowKey = QStringLiteral("p:feature_smoke.exe");
const QString kProbeMemoryKey = QStringLiteral("feature_smoke.exe");

// 列表项上的私有角色（与 mixer_plugin.h 的约定一致；取不到就是面板没按约定做）
constexpr int kRowKeyRole = Qt::UserRole;
constexpr int kPidRole = Qt::UserRole + 1;
constexpr int kInstanceIdsRole = Qt::UserRole + 2;

/// 会话音量往返容差：SetMasterVolume → GetMasterVolume 之间有量化
constexpr float kVolumeTolerance = 0.02F;

/// ⚠ 不能叫 near：windef.h 里有历史宏 near/far
bool nearlyEqual(float left, float right)
{
    return std::abs(left - right) <= kVolumeTolerance;
}

// ---------------------------------------------------------------------------
//  真实状态读取（一律回 Win32 侧）
// ---------------------------------------------------------------------------

/// 该进程当前**全部**会话的音量是否都等于 expected
bool allVolumesAre(quint32 pid, float expected, int *countOut, QString *actualOut)
{
    int count = 0;
    bool allOk = true;
    QStringList actual;
    for (const AudioSessionInfo &session : WinEase::Win32::audioSessions()) {
        if (session.pid != pid) {
            continue;
        }
        ++count;
        actual << (session.volume >= 0.0F ? QString::number(session.volume, 'f', 3)
                                          : QStringLiteral("读不到"));
        if (session.volume < 0.0F || !nearlyEqual(session.volume, expected)) {
            allOk = false;
        }
    }
    if (countOut != nullptr) {
        *countOut = count;
    }
    if (actualOut != nullptr) {
        *actualOut = actual.join(QStringLiteral(" / "));
    }
    return count > 0 && allOk;
}

/// 该进程当前**全部**会话的静音状态是否都等于 expected（且都读得到）
bool allMutedAre(quint32 pid, bool expected, int *countOut, QString *actualOut)
{
    int count = 0;
    bool allOk = true;
    QStringList actual;
    for (const AudioSessionInfo &session : WinEase::Win32::audioSessions()) {
        if (session.pid != pid) {
            continue;
        }
        ++count;
        actual << (session.muteValid
                       ? (session.muted ? QStringLiteral("静音") : QStringLiteral("未静音"))
                       : QStringLiteral("读不到"));
        if (!session.muteValid || session.muted != expected) {
            allOk = false;
        }
    }
    if (countOut != nullptr) {
        *countOut = count;
    }
    if (actualOut != nullptr) {
        *actualOut = actual.join(QStringLiteral(" / "));
    }
    return count > 0 && allOk;
}

QStringList instanceIdsOfPid(quint32 pid)
{
    QStringList ids;
    for (const AudioSessionInfo &session : WinEase::Win32::audioSessions()) {
        if (session.pid == pid) {
            ids << session.instanceId;
        }
    }
    return ids;
}

float masterVolume()
{
    const WinEase::Win32::AudioEndpoint endpoint =
        WinEase::Win32::AudioEndpoint::defaultEndpoint(WinEase::Win32::AudioDirection::Render);
    return endpoint.volume();
}

QString describeSessions()
{
    QStringList parts;
    for (const AudioSessionInfo &session : WinEase::Win32::audioSessions()) {
        parts << QStringLiteral("%1(pid=%2,vol=%3)")
                     .arg(session.systemSounds ? QStringLiteral("系统声音")
                                               : session.processName,
                          QString::number(session.pid),
                          session.volume >= 0.0F ? QString::number(session.volume, 'f', 2)
                                                 : QStringLiteral("?"));
    }
    return parts.join(QStringLiteral("；"));
}

// ---- 配置里的"已记住的音量"（用于验证记忆的落盘/清除） ----
double rememberedValueOf(StubServices &services, const QString &key)
{
    const QStringList entries =
        services.configValue(kPluginId, QStringLiteral("rememberedVolumes"), QStringList())
            .toStringList();
    for (const QString &entry : entries) {
        const int separator = entry.lastIndexOf(QLatin1Char('='));
        if (separator > 0 && entry.left(separator) == key) {
            bool ok = false;
            const double value = entry.mid(separator + 1).toDouble(&ok);
            return ok ? value : -1.0;
        }
    }
    return -1.0;
}

QString rememberedText(StubServices &services)
{
    return services
        .configValue(kPluginId, QStringLiteral("rememberedVolumes"), QStringList())
        .toStringList()
        .join(QStringLiteral("、"));
}

// ---------------------------------------------------------------------------
//  面板（按 objectName 定位；缺一个就如实报出来）
// ---------------------------------------------------------------------------

struct MixerPanel
{
    QWidget *widget = nullptr;
    QLabel *statusLabel = nullptr;
    QListWidget *list = nullptr;
    QSlider *slider = nullptr;
    QLabel *valueLabel = nullptr;
    QCheckBox *muteCheck = nullptr;
    QLabel *detailLabel = nullptr;
    QPushButton *refreshButton = nullptr;
    QPushButton *forgetButton = nullptr;
    QCheckBox *showExpiredCheck = nullptr;
    QCheckBox *groupCheck = nullptr;
    QCheckBox *rememberCheck = nullptr;
    QStringList missing;
};

template <typename T>
T *childOf(QWidget *root, const char *name, QStringList *missing)
{
    auto *found = (root != nullptr) ? root->findChild<T *>(QString::fromLatin1(name)) : nullptr;
    if (found == nullptr) {
        missing->append(QString::fromLatin1(name));
    }
    return found;
}

MixerPanel grabPanel(QWidget *widget)
{
    MixerPanel panel;
    panel.widget = widget;
    if (widget == nullptr) {
        panel.missing << QStringLiteral("（根本没能创建出设置面板）");
        return panel;
    }
    panel.statusLabel = childOf<QLabel>(widget, "mixerStatusLabel", &panel.missing);
    panel.list = childOf<QListWidget>(widget, "mixerSessionList", &panel.missing);
    panel.slider = childOf<QSlider>(widget, "mixerVolumeSlider", &panel.missing);
    panel.valueLabel = childOf<QLabel>(widget, "mixerVolumeValueLabel", &panel.missing);
    panel.muteCheck = childOf<QCheckBox>(widget, "mixerMuteCheck", &panel.missing);
    panel.detailLabel = childOf<QLabel>(widget, "mixerDetailLabel", &panel.missing);
    panel.refreshButton = childOf<QPushButton>(widget, "mixerRefreshButton", &panel.missing);
    panel.forgetButton = childOf<QPushButton>(widget, "mixerForgetButton", &panel.missing);
    panel.showExpiredCheck = childOf<QCheckBox>(widget, "mixerShowExpiredCheck", &panel.missing);
    panel.groupCheck = childOf<QCheckBox>(widget, "mixerGroupCheck", &panel.missing);
    panel.rememberCheck = childOf<QCheckBox>(widget, "mixerRememberCheck", &panel.missing);
    return panel;
}

int rowIndexByKey(const QListWidget *list, const QString &key)
{
    if (list == nullptr) {
        return -1;
    }
    for (int index = 0; index < list->count(); ++index) {
        if (list->item(index)->data(kRowKeyRole).toString() == key) {
            return index;
        }
    }
    return -1;
}

QStringList rowKeys(const QListWidget *list)
{
    QStringList keys;
    if (list == nullptr) {
        return keys;
    }
    for (int index = 0; index < list->count(); ++index) {
        keys << list->item(index)->data(kRowKeyRole).toString();
    }
    return keys;
}

QStringList instanceIdsOfRow(const QListWidget *list, int row)
{
    if (list == nullptr || row < 0 || row >= list->count()) {
        return {};
    }
    return list->item(row)->data(kInstanceIdsRole).toStringList();
}

// ---------------------------------------------------------------------------
//  第一段：聚合模型（与插件**同一份源码**）
// ---------------------------------------------------------------------------

MixerSessionInput makeInput(const QString &instanceId,
                            quint32 pid,
                            const QString &processName,
                            float volume)
{
    MixerSessionInput input;
    input.instanceId = instanceId;
    input.pid = pid;
    input.processName = processName;
    input.volume = volume;
    input.muteValid = true;
    input.active = true;
    input.stateText = QStringLiteral("活动");
    return input;
}

void runModelCases(Reporter &reporter)
{
    using WinEase::FeaturePlugins::Mixer::buildSnapshot;
    using WinEase::FeaturePlugins::Mixer::unavailableVolumeText;

    // ---- ① 同一进程两份会话：合一行、带全 id、代表音量取最响的 ----
    //      "一个应用多份会话"是真实机器上最难碰到的组合（要开两个播放器），
    //      纯数据先钉死判断，真实会话侧再验一次"两份都真的被改"。
    {
        const MixerSnapshot snapshot = buildSnapshot(
            { makeInput(QStringLiteral("id-a"), 4242, QStringLiteral("chrome.exe"), 0.20F),
              makeInput(QStringLiteral("id-b"), 4242, QStringLiteral("chrome.exe"), 0.85F) },
            MixerOptions());

        reporter.check(snapshot.rows.size() == 1,
                       QStringLiteral("模型：同一进程的两份会话合并成一行"),
                       QStringLiteral("实际 %1 行").arg(snapshot.rows.size()));
        if (snapshot.rows.size() == 1) {
            const MixerRow &row = snapshot.rows.first();
            reporter.check(row.key == QStringLiteral("p:chrome.exe"),
                           QStringLiteral("模型：行 key 按进程分（p:<进程名>）"),
                           row.key);
            reporter.check(row.instanceIds.size() == 2
                               && row.instanceIds.contains(QStringLiteral("id-a"))
                               && row.instanceIds.contains(QStringLiteral("id-b")),
                           QStringLiteral(
                               "★ 模型：这一行带着**全部**会话实例 id（按应用调音量一个都不能漏）"),
                           row.instanceIds.join(QStringLiteral("、")));
            reporter.check(row.sessionCount == 2,
                           QStringLiteral("模型：会话数如实是 2"),
                           QString::number(row.sessionCount));
            reporter.check(std::abs(row.volume - 0.85F) < 1e-6F,
                           QStringLiteral("模型：代表音量取**最响**的那一份（0.85），不是最小/平均"),
                           QString::number(row.volume, 'f', 3));
            reporter.check(row.subtitle.contains(QStringLiteral("2 个会话"))
                               && row.subtitle.contains(QStringLiteral("85%")),
                           QStringLiteral("模型：副标题同时写出会话数与代表音量"),
                           row.subtitle);
            reporter.check(row.adjustable(), QStringLiteral("模型：两份都活着，这一行可调"));
        }
    }

    // ---- ② 读不到音量：绝不拿 0% 冒充已静音 ----
    {
        MixerSessionInput input = makeInput(QStringLiteral("id-x"), 100,
                                            QStringLiteral("protected.exe"), -1.0F);
        input.volumeError = QStringLiteral("拒绝访问（该进程权限更高）");

        const MixerSnapshot snapshot = buildSnapshot({ input }, MixerOptions());
        const MixerRow &row = snapshot.rows.first();
        reporter.check(row.volume < 0.0F && row.volumeError == input.volumeError,
                       QStringLiteral("模型：读不到音量时如实带出原因"),
                       row.volumeError);
        reporter.check(!row.subtitle.contains(QStringLiteral("0%")),
                       QStringLiteral("★ 模型：读不到音量时副标题**不含** 0%（不冒充已静音）"),
                       row.subtitle);
        reporter.check(row.subtitle.contains(input.volumeError),
                       QStringLiteral("模型：副标题写出读不到的原因"),
                       row.subtitle);
        reporter.check(snapshot.unreadableCount == 1,
                       QStringLiteral("模型：读不到音量的项被计数（状态行要能如实说出来）"),
                       QString::number(snapshot.unreadableCount));

        // 反过来：读得到时不能误报"读不到"
        const MixerSnapshot ok = buildSnapshot(
            { makeInput(QStringLiteral("id-y"), 101, QStringLiteral("ok.exe"), 0.50F) },
            MixerOptions());
        const MixerRow &okRow = ok.rows.first();
        reporter.check(okRow.subtitle.contains(QStringLiteral("50%"))
                           && !okRow.subtitle.contains(unavailableVolumeText(QString())),
                       QStringLiteral("模型：读得到时副标题是真实百分比，不误报读不到"),
                       okRow.subtitle);
    }

    // ---- ③ 过期会话：默认隐藏 + 如实计数 + 打开后不可调、id 不混进来 ----
    {
        MixerSessionInput live = makeInput(QStringLiteral("live"), 7, QStringLiteral("a.exe"),
                                           0.50F);
        MixerSessionInput dead = makeInput(QStringLiteral("dead"), 9, QStringLiteral("gone.exe"),
                                          -1.0F);
        dead.volumeError = QStringLiteral("会话已过期（进程已退出）");
        dead.expired = true;
        dead.active = false;
        dead.stateText = QStringLiteral("已过期");

        const MixerSnapshot hidden = buildSnapshot({ live, dead }, MixerOptions());
        reporter.check(hidden.rows.size() == 1 && hidden.hiddenExpired == 1,
                       QStringLiteral("模型：过期会话默认不进列表，但**如实计数**（不悄悄吞掉）"),
                       QStringLiteral("行 %1 / 隐藏 %2")
                           .arg(hidden.rows.size())
                           .arg(hidden.hiddenExpired));
        if (!hidden.rows.isEmpty()) {
            reporter.check(hidden.rows.first().instanceIds
                               == QStringList { QStringLiteral("live") },
                           QStringLiteral("★ 模型：过期会话的 id **不混进** instanceIds"
                                          "（否则按应用调音量必然失败几条）"),
                           hidden.rows.first().instanceIds.join(QStringLiteral("、")));
        }

        MixerOptions shown;
        shown.showExpired = true;
        const MixerSnapshot withDead = buildSnapshot({ live, dead }, shown);
        int deadRow = -1;
        for (int index = 0; index < withDead.rows.size(); ++index) {
            if (withDead.rows.at(index).key == QStringLiteral("p:gone.exe")) {
                deadRow = index;
            }
        }
        reporter.check(deadRow >= 0,
                       QStringLiteral("模型：打开「显示已过期的会话」后过期项重新出现"),
                       QString::number(withDead.rows.size()));
        if (deadRow >= 0) {
            const MixerRow &row = withDead.rows.at(deadRow);
            reporter.check(row.expired && !row.adjustable(),
                           QStringLiteral("★ 模型：过期行判为不可调（面板必须把滑块灰掉）"));
            reporter.check(row.instanceIds.isEmpty(),
                           QStringLiteral("模型：过期行的 instanceIds 为空"),
                           row.instanceIds.join(QStringLiteral("、")));
            reporter.check(row.subtitle.contains(QStringLiteral("已过期")),
                           QStringLiteral("模型：过期行的副标题明写「已过期」"),
                           row.subtitle);
        }
    }

    // ---- ④ 静音聚合：部分静音必须说出来，二选一都是骗用户 ----
    {
        MixerSessionInput mutedOne = makeInput(QStringLiteral("m1"), 55,
                                              QStringLiteral("mix.exe"), 0.50F);
        mutedOne.muted = true;
        const MixerSessionInput loudOne = makeInput(QStringLiteral("m2"), 55,
                                                   QStringLiteral("mix.exe"), 0.50F);
        const MixerRow &row = buildSnapshot({ mutedOne, loudOne }, MixerOptions()).rows.first();
        reporter.check(row.muteMixed && !row.muted,
                       QStringLiteral("★ 模型：一份静音一份不静音 → 明确标成「部分会话已静音」"));
        reporter.check(row.subtitle.contains(QStringLiteral("部分会话已静音")),
                       QStringLiteral("模型：副标题写出「部分会话已静音」"),
                       row.subtitle);

        MixerSessionInput alsoMuted = mutedOne;
        alsoMuted.instanceId = QStringLiteral("m3");
        const MixerRow &allRow =
            buildSnapshot({ mutedOne, alsoMuted }, MixerOptions()).rows.first();
        reporter.check(allRow.muted && !allRow.muteMixed,
                       QStringLiteral("模型：两份都静音 → 整行标成已静音"));
        reporter.check(allRow.subtitle.contains(QStringLiteral("已静音")),
                       QStringLiteral("模型：副标题写出「已静音」"),
                       allRow.subtitle);
    }

    // ---- ⑤ 系统声音：单独一行、排在最后、pid 归零 ----
    {
        const MixerSessionInput app = makeInput(QStringLiteral("app"), 66,
                                               QStringLiteral("game.exe"), 0.40F);
        MixerSessionInput sys;
        sys.instanceId = QStringLiteral("sys");
        sys.systemSounds = true;
        sys.pid = 2372; // 实测：系统声音也可能挂在真实进程上，不能靠 pid 判
        sys.volume = 0.60F;
        sys.muteValid = true;
        sys.active = true;
        sys.stateText = QStringLiteral("活动");

        const MixerSnapshot snapshot = buildSnapshot({ sys, app }, MixerOptions());
        reporter.check(snapshot.rows.size() == 2,
                       QStringLiteral("模型：应用与系统声音各占一行"),
                       QString::number(snapshot.rows.size()));
        if (snapshot.rows.size() == 2) {
            reporter.check(snapshot.rows.at(0).kind == MixerRowKind::Application
                               && snapshot.rows.at(1).kind == MixerRowKind::SystemSounds,
                           QStringLiteral("模型：应用在前、系统声音固定排最后"
                                          "（别把系统声音进程当成一个应用给用户看）"));
            reporter.check(snapshot.rows.at(1).title == QStringLiteral("系统声音")
                               && snapshot.rows.at(1).pid == 0,
                           QStringLiteral("模型：系统声音行标题是「系统声音」且不显示 pid"),
                           snapshot.rows.at(1).title);
        }
    }

    // ---- ⑥ 关掉"按应用合并"：两份会话应当分开成两行（开关真的有效） ----
    {
        MixerOptions options;
        options.groupByProcess = false;
        const MixerSnapshot snapshot = buildSnapshot(
            { makeInput(QStringLiteral("s1"), 77, QStringLiteral("split.exe"), 0.30F),
              makeInput(QStringLiteral("s2"), 77, QStringLiteral("split.exe"), 0.70F) },
            options);
        reporter.check(snapshot.rows.size() == 2,
                       QStringLiteral("模型：关掉合并后同一应用的两份会话分成两行"),
                       QString::number(snapshot.rows.size()));
        if (snapshot.rows.size() == 2) {
            reporter.check(snapshot.rows.at(0).instanceIds.size() == 1
                               && snapshot.rows.at(1).instanceIds.size() == 1,
                           QStringLiteral("模型：分开后每行只管自己那一份会话"));
        }
    }
}

} // namespace

int runMixerGroupTests(Reporter &reporter,
                       WinEase::PluginManager &manager,
                       StubServices &services)
{
    const int failuresBefore = reporter.failures();

    // =======================================================================
    //  第一段：聚合模型（纯函数，插件与自检编译同一份源码）
    // =======================================================================
    runModelCases(reporter);

    // =======================================================================
    //  第二段：真实会话 + 真实插件 DLL
    // =======================================================================
    WinEase::IFeaturePlugin *plugin = manager.plugin(kPluginId);
    reporter.check(plugin != nullptr,
                   QStringLiteral("插件：media.mixer 已从 build/bin/plugins 加载（真实 DLL）"),
                   QStringLiteral("manager.plugin(\"media.mixer\") 为空"));
    if (plugin == nullptr) {
        return reporter.failures() - failuresBefore;
    }

    reporter.check(manager.nameOf(plugin) == QStringLiteral("音量混合器")
                       && !manager.descriptionOf(plugin).isEmpty(),
                   QStringLiteral("插件：名称与描述能被主程序的缓存读接口取到"),
                   QStringLiteral("%1 / %2")
                       .arg(manager.nameOf(plugin), manager.descriptionOf(plugin)));
    reporter.check(!plugin->supportsHotkey(),
                   QStringLiteral("插件：面板型功能，不注册全局快捷键"));
    reporter.check(!plugin->requiresAdmin(),
                   QStringLiteral("插件：不需要管理员权限（会话音量本来就属于当前用户）"));

    // ---- 前置：自检自己造两份**真实的**音频会话（不同进程、同一进程名）----
    AudioSessionProbe probeA;
    AudioSessionProbe probeB;
    QString probeError;
    const bool startedA = probeA.start(0.20F, &probeError);
    reporter.check(startedA,
                   QStringLiteral("前置：能起一份真实音频会话（子进程 WASAPI 渲染客户端）"),
                   probeError);
    bool startedB = false;
    if (startedA) {
        probeError.clear();
        startedB = probeB.start(0.85F, &probeError);
        reporter.check(startedB,
                       QStringLiteral("前置：能起第二份真实音频会话（另一个进程）"),
                       probeError);
    }
    if (!startedA || !startedB) {
        reporter.info(QStringLiteral("本机拿不到两份真实音频会话，P3-09 的端到端断言无法进行；"
                                     "当前会话快照：%1")
                          .arg(describeSessions()));
        return reporter.failures() - failuresBefore;
    }

    const quint32 pidA = probeA.pid();
    const quint32 pidB = probeB.pid();

    const bool bothVisible = waitForStable(
        [&] { return instanceIdsOfPid(pidA).size() == 1 && instanceIdsOfPid(pidB).size() == 1; },
        4000,
        3,
        60);
    reporter.check(bothVisible,
                   QStringLiteral("前置：两份探针会话都出现在默认输出设备的会话枚举里"),
                   describeSessions());
    if (!bothVisible) {
        return reporter.failures() - failuresBefore;
    }

    int sessionCount = 0;
    QString actualVolumes;
    const bool initialVolumes =
        allVolumesAre(pidA, 0.20F, &sessionCount, &actualVolumes)
        && allVolumesAre(pidB, 0.85F, &sessionCount, &actualVolumes);
    reporter.check(initialVolumes,
                   QStringLiteral("前置：两份会话各自读回自己设的初值（20% / 85%）"
                                  "—— 证明读的是真会话，不是自检自己写进去的缓存"),
                   actualVolumes);

    const QStringList idsA = instanceIdsOfPid(pidA);
    const QStringList idsB = instanceIdsOfPid(pidB);

    // ---- 启用 + 面板 ----
    reporter.check(manager.setPluginEnabled(kPluginId, true),
                   QStringLiteral("插件：能启用"),
                   manager.pluginFailureReason(kPluginId));
    settleEvents(150);

    QWidget *panel = manager.createSettingsWidget(kPluginId);
    MixerPanel ui = grabPanel(panel);
    reporter.check(ui.missing.isEmpty(),
                   QStringLiteral("面板：全部自检控件都能按 objectName 定位"),
                   QStringLiteral("缺：%1").arg(ui.missing.join(QStringLiteral("、"))));

    // 收尾：停用 + 归还面板（面板是裸 widget，由自检负责）
    const auto finish = [&]() -> int {
        manager.setPluginEnabled(kPluginId, false);
        settleEvents(100);
        if (panel != nullptr) {
            panel->deleteLater();
            settleEvents(60);
        }
        return reporter.failures() - failuresBefore;
    };

    if (!ui.missing.isEmpty() || panel == nullptr) {
        return finish();
    }
    settleEvents(150);

    const QString deviceName = WinEase::Win32::defaultRenderDeviceName();
    reporter.check(!deviceName.isEmpty() && ui.statusLabel->text().contains(deviceName),
                   QStringLiteral("面板：状态行说出**真实**输出设备名（不是空串、不是占位符）"),
                   ui.statusLabel->text());
    reporter.check(ui.statusLabel->text().contains(QStringLiteral("可调项")),
                   QStringLiteral("面板：状态行给出可调项数量"),
                   ui.statusLabel->text());

    // ---- ★ 两个同进程名的会话应当合并成一行 ----
    const int probeRow = rowIndexByKey(ui.list, kProbeRowKey);
    reporter.check(probeRow >= 0,
                   QStringLiteral("★ 面板：两个同进程名的会话合并成一行（%1）").arg(kProbeRowKey),
                   QStringLiteral("实际行：%1").arg(rowKeys(ui.list).join(QStringLiteral("、"))));
    if (probeRow < 0) {
        return finish();
    }

    const QStringList rowIds = instanceIdsOfRow(ui.list, probeRow);
    QSet<QString> expectedIds;
    for (const QString &id : idsA) {
        expectedIds.insert(id);
    }
    for (const QString &id : idsB) {
        expectedIds.insert(id);
    }
    reporter.check(rowIds.size() == 2 && QSet<QString>(rowIds.begin(), rowIds.end()) == expectedIds,
                   QStringLiteral("★★ 面板：这一行带着**两份**真实会话的实例 id（不多不少）"),
                   QStringLiteral("行内 %1 / 真实 %2")
                       .arg(rowIds.join(QStringLiteral("、")),
                            expectedIds.values().join(QStringLiteral("、"))));
    reporter.check(rowIds.contains(idsA.first()) && rowIds.contains(idsB.first()),
                   QStringLiteral("★★ 面板：两份 id 分别来自两个不同进程（同一进程名的两份会话）"));
    const quint32 rowPid = ui.list->item(probeRow)->data(kPidRole).toUInt();
    reporter.check(rowPid == pidA || rowPid == pidB,
                   QStringLiteral("面板：行的 pid 是其中一份真实会话的 pid"),
                   QString::number(rowPid));
    reporter.check(ui.list->item(probeRow)->text().contains(QStringLiteral("×2")),
                   QStringLiteral("面板：行标题标出 ×2（用户看得见合并了几份）"),
                   ui.list->item(probeRow)->text());
    reporter.check(ui.list->item(probeRow)->text().contains(QStringLiteral("85%")),
                   QStringLiteral("面板：行上显示的是**最响**那一份（85%）"),
                   ui.list->item(probeRow)->text());

    // ---- 选中该行：滑块必须停在真实读回来的值上 ----
    ui.list->setCurrentRow(probeRow);
    settleEvents(80);
    reporter.check(ui.slider->isEnabled(), QStringLiteral("面板：选中可调行后滑块可用"));
    reporter.check(ui.slider->value() == 85,
                   QStringLiteral("面板：滑块停在真实读回的 85%（不是默认值/上次残留）"),
                   QString::number(ui.slider->value()));
    reporter.check(ui.detailLabel->text().contains(QStringLiteral("会话：2 份")),
                   QStringLiteral("面板：详情明写这一行是 2 份会话合并来的"),
                   ui.detailLabel->text());

    // ---- ★★ 核心：拖一次滑块，两份真实会话**都**要变；且**不动系统主音量** ----
    const float masterBefore = masterVolume();
    reporter.check(masterBefore >= 0.0F,
                   QStringLiteral("前置：能读到系统主音量（用来断言「调应用不动主音量」）"),
                   QString::number(masterBefore, 'f', 3));

    ui.slider->setValue(45);
    settleEvents(150);

    int changedCount = 0;
    QString changedActual;
    reporter.check(allVolumesAre(pidA, 0.45F, &changedCount, &changedActual),
                   QStringLiteral("★★ 拖滑块：第一份真实会话的音量变成了 45%"),
                   QStringLiteral("%1 份：%2").arg(changedCount).arg(changedActual));
    reporter.check(allVolumesAre(pidB, 0.45F, &changedCount, &changedActual),
                   QStringLiteral("★★ 拖滑块：**第二份**真实会话也变成了 45%"
                                  "（按应用调音量就是要覆盖该进程全部会话）"),
                   QStringLiteral("%1 份：%2").arg(changedCount).arg(changedActual));
    reporter.check(ui.valueLabel->text() == QStringLiteral("45%"),
                   QStringLiteral("面板：数值标签立刻反馈 45%（不等下一轮轮询）"),
                   ui.valueLabel->text());

    const float masterAfter = masterVolume();
    reporter.check(masterAfter >= 0.0F && std::abs(masterAfter - masterBefore) < 0.01F,
                   QStringLiteral("★★ 分界线：调**会话**音量没有动系统**主音量**"
                                  "（会话级与端点级是两回事）"),
                   QStringLiteral("%1 → %2")
                       .arg(QString::number(masterBefore, 'f', 3),
                            QString::number(masterAfter, 'f', 3)));

    // ---- 静音同样要覆盖全部会话 ----
    ui.muteCheck->setChecked(true);
    settleEvents(150);
    int muteCount = 0;
    QString muteActual;
    reporter.check(allMutedAre(pidA, true, &muteCount, &muteActual),
                   QStringLiteral("★★ 勾「静音该应用」：第一份真实会话真的被静音"),
                   QStringLiteral("%1 份：%2").arg(muteCount).arg(muteActual));
    reporter.check(allMutedAre(pidB, true, &muteCount, &muteActual),
                   QStringLiteral("★★ 勾「静音该应用」：**第二份**真实会话也真的被静音"),
                   QStringLiteral("%1 份：%2").arg(muteCount).arg(muteActual));

    ui.muteCheck->setChecked(false);
    settleEvents(150);
    reporter.check(allMutedAre(pidA, false, &muteCount, &muteActual)
                       && allMutedAre(pidB, false, &muteCount, &muteActual),
                   QStringLiteral("面板：取消静音后两份真实会话都恢复未静音"),
                   muteActual);

    // ---- 停用：滑块要如实变灰，且**不还原**音量（音量是用户在用的系统设置）----
    reporter.check(manager.setPluginEnabled(kPluginId, false),
                   QStringLiteral("插件：能停用"),
                   manager.pluginFailureReason(kPluginId));
    settleEvents(200);
    reporter.check(!ui.slider->isEnabled(),
                   QStringLiteral("面板：停用后滑块不可调（如实反映停用，不是留一个假的可点滑块）"));
    reporter.check(ui.statusLabel->text().startsWith(QStringLiteral("已停用")),
                   QStringLiteral("面板：停用后状态行明说「已停用」"),
                   ui.statusLabel->text());
    reporter.check(!manager.isPluginEnabled(kPluginId),
                   QStringLiteral("插件：停用后状态确实是未启用"));
    reporter.check(allVolumesAre(pidA, 0.45F, &changedCount, &changedActual)
                       && allVolumesAre(pidB, 0.45F, &changedCount, &changedActual),
                   QStringLiteral("★★ 纪律：停用**没有**把音量悄悄还原（停用 ≠ 撤销用户的设置）"),
                   changedActual);

    // 自检自己负责复原（探针值本来就是自检设的，收尾时还回去，不留痕）
    QString restoreError;
    WinEase::Win32::setAudioSessionVolume(idsA.first(), 0.20F, &restoreError);
    WinEase::Win32::setAudioSessionVolume(idsB.first(), 0.85F, &restoreError);
    reporter.check(allVolumesAre(pidA, 0.20F, &changedCount, &changedActual)
                       && allVolumesAre(pidB, 0.85F, &changedCount, &changedActual),
                   QStringLiteral("收尾：两份会话的音量已被自检复原（不留痕）"),
                   changedActual);

    // ---- 音量记忆：开关关着 → 改音量**不落盘** ----
    reporter.check(manager.setPluginEnabled(kPluginId, true),
                   QStringLiteral("插件：重新启用（继续验证音量记忆）"),
                   manager.pluginFailureReason(kPluginId));
    settleEvents(200);
    reporter.check(!ui.rememberCheck->isChecked(),
                   QStringLiteral("面板：rememberVolume 的预设 false 被如实读回（开关是关的）"));
    reporter.check(rememberedValueOf(services, kProbeMemoryKey) < 0.0,
                   QStringLiteral("前置：配置里本来就没有本应用的记忆"),
                   rememberedText(services));

    ui.list->setCurrentRow(rowIndexByKey(ui.list, kProbeRowKey));
    settleEvents(80);
    reporter.check(ui.slider->value() == 85,
                   QStringLiteral("前置：重新启用后滑块又按真实值复位到 85%"),
                   QString::number(ui.slider->value()));
    ui.slider->setValue(45);
    settleEvents(300);
    reporter.check(rememberedValueOf(services, kProbeMemoryKey) < 0.0,
                   QStringLiteral("★ 记忆：开关关着时改音量**不写配置**（关掉就真的不记）"),
                   rememberedText(services));

    // ---- 打开开关 → 当前应用的音量被记住并落盘 ----
    ui.rememberCheck->setChecked(true);
    const bool remembered = waitForStable(
        [&] { return rememberedValueOf(services, kProbeMemoryKey) >= 0.0; }, 6000, 3, 100);
    reporter.check(remembered,
                   QStringLiteral("★ 记忆：打开开关后，当前应用的音量被记住并写进配置"),
                   rememberedText(services));
    reporter.check(
        nearlyEqual(static_cast<float>(rememberedValueOf(services, kProbeMemoryKey)), 0.45F),
                   QStringLiteral("★ 记忆：记住的是**真实**音量 45%（不是滑块值/以为值）"),
                   QString::number(rememberedValueOf(services, kProbeMemoryKey), 'f', 3));

    // ---- ★ 新出现的会话自动套用记忆 ----
    //      先把两份老会话停掉：否则它们会继续刷新记忆，测不出"新会话套用"
    probeA.stop();
    probeB.stop();
    reporter.check(waitForStable([&] { return rowIndexByKey(ui.list, kProbeRowKey) < 0; },
                                 5000,
                                 3,
                                 120),
                   QStringLiteral("★ 会话来去：探针退出后这一行从默认视图消失"
                                  "（过期会话默认隐藏，只计数不显示）"),
                   QStringLiteral("行：%1").arg(rowKeys(ui.list).join(QStringLiteral("、"))));

    AudioSessionProbe probeC;
    QString probeCError;
    const bool startedC = probeC.start(0.99F, &probeCError);
    reporter.check(startedC,
                   QStringLiteral("前置：第三个探针会话已创建（初值 99%）"),
                   probeCError);
    if (startedC) {
        const quint32 pidC = probeC.pid();
        reporter.check(waitForStable([&] { return allVolumesAre(pidC, 0.45F, nullptr, nullptr); },
                                     8000,
                                     3,
                                     120),
                       QStringLiteral("★★ 记忆：新出现的会话（同进程名）被自动套用记住的 45%，"
                                      "而不是它自己的 99%"),
                       describeSessions());
        probeC.stop();
    }

    // ---- 清除记忆：清完再起新会话，必须**保持自己的音量** ----
    //      注：探针会话都停掉后再清，避免"当前观察到的音量"被立刻记回去
    reporter.check(waitForStable([&] { return rowIndexByKey(ui.list, kProbeRowKey) < 0; },
                                 5000,
                                 3,
                                 120),
                   QStringLiteral("前置：探针会话已全部退出（清除记忆前先断掉观察源）"),
                   QStringLiteral("行：%1").arg(rowKeys(ui.list).join(QStringLiteral("、"))));
    ui.forgetButton->click();
    settleEvents(60);
    reporter.check(rememberedValueOf(services, kProbeMemoryKey) < 0.0,
                   QStringLiteral("★ 记忆：点「清除已记住的音量」后，本应用的记忆从配置里消失"),
                   rememberedText(services));

    AudioSessionProbe probeD;
    QString probeDError;
    const bool startedD = probeD.start(0.30F, &probeDError);
    reporter.check(startedD,
                   QStringLiteral("前置：第四个探针会话已创建（初值 30%）"),
                   probeDError);
    if (startedD) {
        const quint32 pidD = probeD.pid();
        // 等过两轮轮询（预设 1 秒/轮）再断言：确认**没有**任何东西把它改成别的值
        settleEvents(2600);
        int dCount = 0;
        QString dActual;
        reporter.check(allVolumesAre(pidD, 0.30F, &dCount, &dActual),
                       QStringLiteral("★★ 记忆：清除之后新会话保持自己的 30%"
                                      "（记忆真的被清掉了，不只是配置显示被清空）"),
                       QStringLiteral("%1 份：%2").arg(dCount).arg(dActual));
    }

    return finish();
}

} // namespace FeatureSmoke
