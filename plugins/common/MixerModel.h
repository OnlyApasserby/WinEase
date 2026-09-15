#pragma once

// ============================================================================
//  MixerModel.h —— 音量混合器（P3-09）的显示模型
//
//  为什么放在 plugins/common（而不是插件私有）：
//      本功能最硬的纪律是"**按应用调音量必须覆盖该进程的全部会话**"
//      （一个进程可以有好几份会话，只调一份 = 用户拖了滑块却听不出变化），
//      以及"**读不到音量必须写出原因，绝不显示 0%**"。
//      这两条必须由**插件与自检编译同一份源码**，否则自检断言的是另一套实现，
//      等于没验（同 HudMetrics / TextTools / RenameEngine 的纪律）。
//
//  ⚠ 本文件与实现**不含任何 Win32 头**，只吃纯数据：
//      取数在 src/win32/AudioSessions（平台层）完成，插件负责把结果填进
//      MixerSessionInput，本层只做"聚合 + 过滤 + 格式化 + 不可用文案"。
//      （自检因此能直接构造出真实机器上很难出现的组合，例如"读不到音量"、
//        "同一进程两份会话音量不一致"、"只有系统声音会话"。）
//
//  ⚠ 本文件不含 Q_OBJECT，可被任意插件/自检直接链接。
// ============================================================================

#include <QList>
#include <QString>
#include <QStringList>

namespace WinEase::FeaturePlugins::Mixer {

// ============================================================================
//  输入
// ============================================================================

/// 一个音频会话的原始读数（由插件从 WinEase::Win32::AudioSessionInfo 填充）
struct MixerSessionInput {
    QString instanceId; ///< 会话实例标识（调音量靠它）
    quint32 pid = 0;
    QString processName;    ///< "chrome.exe"；读不到为空
    QString displayName;    ///< 会话显示名；多数应用为空
    QString executablePath; ///< 读不到为空（不猜）
    double volume = -1.0;   ///< 0~1；**< 0 表示读不到**（volumeError 有原因）
    QString volumeError;
    bool muted = false;     ///< 仅 muteValid == true 时有意义
    bool muteValid = false;
    QString muteError;
    bool expired = false;   ///< 进程已退出 → 不可操作
    bool active = false;    ///< 正在出声
    bool systemSounds = false; ///< PID 0：系统提示音
    QString stateText;      ///< "活动 / 不活动 / 已过期"
};

// ============================================================================
//  显示模型
// ============================================================================

/// 面板一行的种类（决定标题怎么起）
enum class MixerRowKind {
    Application,  ///< 某个应用
    SystemSounds, ///< 系统声音（PID 0）
    Unknown       ///< 进程名读不到（跨权限）
};

/// 面板上的一行 = 一个"可调对象"
struct MixerRow {
    /// 稳定标识：按进程聚合时是 "p:<小写进程名>"，逐会话时是 "i:<实例id>"。
    /// ⚠ 刷新时靠它保持选中项（会话会来去，下标会变）
    QString key;
    MixerRowKind kind = MixerRowKind::Application;

    QString title;    ///< "chrome.exe" / "系统声音" / "未知进程（PID 4321）"
    QString subtitle; ///< "PID 1234 · 2 个会话 · 78%"
    QString tooltip;  ///< 完整说明（路径、逐会话状态、读不到的原因）

    quint32 pid = 0;             ///< 该行的进程（系统声音为 0）
    QString executablePath;      ///< 进程路径；读不到为空（不猜）
    QStringList sessionDetails;  ///< 逐会话明细（进 tooltip）

    /// 这一行背后的**全部**会话实例 id —— 调音量/静音时一个都不能漏（见文件头纪律）
    QStringList instanceIds;

    /// 代表音量 0~1；**< 0 表示这一行读不到音量**（此时 volumeError 有原因）
    double volume = -1.0;
    QString volumeError;

    bool muted = false;      ///< 全部（读得到的）会话都已静音
    bool muteValid = false;  ///< 至少有一个会话读到了静音状态
    bool muteMixed = false;  ///< 会话之间静音状态不一致（部分静音）
    QString muteError;

    bool expired = false;    ///< 整行的会话全都过期了
    bool active = false;     ///< 至少一份会话正在出声
    /// 这一行背后的会话份数（由 buildSnapshot 累加得出）
    /// ⚠ 必须是 **0** 而不是 1：累加从默认值起步，默认 1 会让每个应用都多报一份
    ///   会话（踩坑 #74），并且 row.expired 的判定（expiredCount == sessionCount）
    ///   会永远算不出"整行都过期"。
    int sessionCount = 0;

    /// 本行是否可被调音量/静音（过期行、拿不到实例标识的行都不行）
    bool adjustable() const { return !expired && !instanceIds.isEmpty(); }
};

/// 组装选项（对应面板上的三个勾选框）
struct MixerOptions {
    bool groupByProcess = true; ///< 把同一进程的多份会话合并成一行（默认开，这是本功能的核心增强）
    bool showExpired = false;   ///< 是否列出已过期的会话（默认关，与 Windows 自带合成器一致）
};

/// 一次刷新的完整结果
struct MixerSnapshot {
    QList<MixerRow> rows;      ///< 已过滤、已排序（应用在前、系统声音在后，同类按标题）
    int hiddenExpired = 0;     ///< 被隐藏的已过期会话数（**要如实告诉用户**，不能悄悄吞掉）
    int unreadableCount = 0;   ///< 音量读不到的会话数（面板要给出提示，而不是显示 0%）
};

// ============================================================================
//  组装与格式化（纯函数：同样输入必得同样输出）
// ============================================================================

/// 由原始会话组装显示模型
MixerSnapshot buildSnapshot(const QList<MixerSessionInput> &sessions,
                            const MixerOptions &options);

/// 行的完整 tooltip（含"读不到的原因"与逐会话明细）
QString rowTooltip(const MixerRow &row);

/// 音量百分比："78%"（入参收敛到 0~1）。⚠ 只接受 >= 0 的值：
/// "读不到"必须走 unavailableVolumeText()，绝不允许formatVolume 出 "0%"
QString formatVolume(double volume);

/// 不可用文案（统一口径）：优先用真实原因；原因也空时给一句兜底。
/// ⚠ 保证返回**非空** —— 这是"绝不显示 0% / 空白"的最后一道闸
QString unavailableVolumeText(const QString &error);

/// 未选中任何行时的提示
QString noSelectionText();

/// 面板顶部的一句话状态："输出设备：扬声器 (…) · 应用 3 个 · 已隐藏 1 个过期会话"
QString statusLine(const QString &deviceName, const MixerSnapshot &snapshot);

} // namespace WinEase::FeaturePlugins::Mixer
