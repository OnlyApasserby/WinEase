#pragma once

// ============================================================================
//  MixerPlugin —— 音量混合器（media.mixer）
//
//  把 Windows 自带音量合成器里"每个应用一根滑块"搬到主界面面板里，并补上
//  自带合成器没有的几件事：按应用合并同类会话、音量读不到时给出原因、可选的
//  "记住每个应用的音量"。
//
//  ---------------------------------------------------------------------------
//  四条纪律（写在这里，因为改这份代码的人最容易违反它们）：
//
//   1. **按应用调音量必须覆盖该进程的全部会话**。一个进程可以有好几份会话，
//      只调其中一份 = 用户"拖了滑块却听不出变化"。见 MixerModel 的聚合逻辑与
//      plugins/common/MixerModel.h 顶部说明。
//
//   2. **绝不动系统主音量**。本插件只管会话音量；主音量归 P2 的滚轮调音量。
//      两条链路混在一起会让用户"拖了一个应用，整个系统都变小声了"。
//
//   3. **停用不还原**。音量是用户可见、用户关心的系统设置；停用功能时把它
//      悄悄改回去属于"静默破坏"。停用只做两件事：停轮询、摘掉自己的界面状态。
//      （对照 mic_mute 对"静音"的处理：停用不自动开麦。）
//
//   4. **读不到就写原因，绝不显示 0%**。会话音量读不到（跨权限、会话刚过期）
//      时界面必须写清原因 —— 显示 0% 等于告诉用户"这个应用被静音了"。
//
//  ---------------------------------------------------------------------------
//  自检接口（tests/feature_smoke/mixer_group.cpp）：面板控件的 objectName 固定为
//  mixerStatusLabel / mixerSessionList / mixerVolumeSlider / mixerVolumeValueLabel /
//  mixerMuteCheck / mixerRefreshButton / mixerShowExpiredCheck / mixerGroupCheck /
//  mixerRememberCheck / mixerForgetButton / mixerDetailLabel；
//  列表项上另挂三个数据槽供自检按"行 / pid / 会话"定位（避免去解析界面文本）：
//      Qt::UserRole     → 行 key（"p:xxx" / "i:<实例id>"）
//      Qt::UserRole + 1 → 行对应的 pid（quint32）
//      Qt::UserRole + 2 → 行背后的全部会话实例 id（QStringList）
// ============================================================================

#include "MixerModel.h"
#include "sdk/IFeaturePlugin.h"
#include "win32/AudioSessions.h"

#include <QHash>
#include <QPointer>
#include <QSet>
#include <QStringList>

class QCheckBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSlider;
class QTimer;
class QWidget;

class MixerPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "mixer_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit MixerPlugin(QObject *parent = nullptr);
    ~MixerPlugin() override;

    // ---------------- 元信息 ----------------
    QString id() const override;
    QString name() const override;
    QString description() const override;
    QIcon icon() const override;
    WinEase::FeatureCategory category() const override;
    QString version() const override;
    QString author() const override;
    QString detailedDescription() const override;
    QStringList tags() const override;

    // ---------------- 能力标记 ----------------
    bool requiresAdmin() const override;
    bool supportsHotkey() const override;

    // ---------------- 生命周期 ----------------
    bool initialize() override;
    void shutdown() override;
    QWidget *createSettingsWidget(QWidget *parent = nullptr) override;

protected:
    bool onEnable() override;
    void onDisable() override;
    bool canEnable(QString *reason) const override;

private:
    // ---------------- 主流程 ----------------
    /// 读一次真实会话状态并重画面板。@param fromUser 用户点了"刷新"（未启用时也允许只读刷新）
    void refreshSessions(bool fromUser);
    /// 定时器回调（未启用时直接返回）
    void pollOnce();

    // ---------------- 界面 ----------------
    void rebuildList(const WinEase::FeaturePlugins::Mixer::MixerSnapshot &snapshot);
    void restoreSelection(const QString &rowKey);
    void updateDetailForCurrentRow();
    void refreshPanel();

    // ---------------- 写入 ----------------
    /// 把当前选中行的**全部**会话设成同一音量（percent 为滑块值 0~100）
    void applyVolumeToCurrentRow(int percent);
    /// 把当前选中行的**全部**会话设成同一静音状态
    void applyMuteToCurrentRow(bool muted);
    /// 当前选中行（找不到返回空行）
    WinEase::FeaturePlugins::Mixer::MixerRow currentRow() const;
    bool rowByKey(const QString &rowKey,
                  WinEase::FeaturePlugins::Mixer::MixerRow *rowOut) const;

    // ---------------- 音量记忆 ----------------
    /// 一轮记忆处理：给"本轮首次出现"的会话套用记忆值，再把观察到的音量记下来。
    /// ⚠ **不要**在内部判 isEnabled() —— 它是被 onEnable() 直接调用的，那时是旧值（踩坑 #28）
    void applyMemoryPass();
    void loadRememberedVolumes();
    void saveRememberedVolumes();
    /// 把当前观察到的会话音量记进记忆（有变化才写配置）
    void observeVolumesForMemory(const QList<WinEase::Win32::AudioSessionInfo> &sessions);
    void forgetRememberedVolumes();

    // ---------------- 状态 ----------------
    /// 记下状态行文本 + 推给卡片（未启用时的"已停用"前缀由 updateStatusLabel 补）
    void publishStatus(const QString &text);
    void updateStatusLabel();
    void reportFailure(const QString &text);

    // ---------------- 成员 ----------------
    QTimer *m_pollTimer = nullptr;

    // 选项（持久化在 [Plugins/media.mixer]）
    bool m_groupByProcess = true;
    bool m_showExpired = false;
    bool m_rememberVolume = true;
    int m_pollSeconds = 2;

    // 刷新状态
    WinEase::FeaturePlugins::Mixer::MixerSnapshot m_snapshot;
    QString m_statusLine;     ///< 最近一次算出来的状态行（未启用时前面会补"已停用"）
    QString m_enumError;      ///< 枚举失败原因（空表示这一轮枚举正常）
    QString m_deviceId;       ///< 上一轮看到的默认输出设备（热插拔判据）
    QString m_deviceName;
    QString m_selectedKey;    ///< 当前选中的行 key
    QSet<QString> m_lastInstanceIds; ///< 上一轮存在的会话实例 id（用来判"首次出现"）
    QSet<QString> m_deviceSessions;  ///< 本轮存在的会话实例 id

    // 音量记忆：键 = 小写进程名
    QHash<QString, double> m_remembered;
    bool m_rememberedDirty = false;

    // 界面
    QPointer<QWidget> m_panel;
    QPointer<QLabel> m_statusLabel;
    QPointer<QListWidget> m_sessionList;
    QPointer<QSlider> m_volumeSlider;
    QPointer<QLabel> m_volumeValueLabel;
    QPointer<QCheckBox> m_muteCheck;
    QPointer<QPushButton> m_refreshButton;
    QPointer<QCheckBox> m_showExpiredCheck;
    QPointer<QCheckBox> m_groupCheck;
    QPointer<QCheckBox> m_rememberCheck;
    QPointer<QPushButton> m_forgetButton;
    QPointer<QLabel> m_detailLabel;
    /// 程序化改控件时置位，挡住"改控件 → 信号 → 回调又改控件"导致的栈溢出（踩坑 #57）
    bool m_syncingUi = false;
};
