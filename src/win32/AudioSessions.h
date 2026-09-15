#pragma once

// ============================================================================
//  AudioSessions.h —— 音频会话（"每个应用用多大声"）的读取与写入（P3-09 音量混合器）
//
//  与 CoreAudio.h 的分工（**并列关系，不是上下级**）：
//      CoreAudio.h     管**端点主音量** —— "系统音量"，滚轮调音量 / 麦克风静音用
//      本文件          管**会话音量**   —— Windows 音量合成器里"每个应用一个滑块"
//   改会话音量**绝不会**动主音量；反之亦然（自检把这条当作分界线断言）。
//
//  ---------------------------------------------------------------------------
//  三条必须记住的事实（都是实测出来的，写在这里省得下一次再踩）：
//
//   1. **会话 ≠ 进程**。同一进程可以在同一设备上开出多份会话（浏览器就是这么干的：
//      主进程一份、音频服务进程一份、插件一份）。只调其中一份，用户会得到
//      "我拖了滑块但音量没变" —— 因为响的是另一份。所以"按应用调音量"必须
//      **覆盖该进程的全部会话**，这就是本文件提供批量入口（applySessionVolume /
//      applySessionMuted）的原因，也是插件侧必须按进程聚合的原因。
//
//   2. **会话会过期**。进程退出后会话变成 `AudioSessionStateExpired`，一段时间内
//      仍能枚举到，但读音量/静音会失败。过期会话**不是可操作对象**，必须按
//      expired 标出来（Windows 自带合成器也是直接隐藏）。本层**照实返回**过期会话，
//      隐藏与否交给上层决定 —— 平台层不做取舍。
//
//   3. **系统声音不能靠 PID 判**。判据是 `IsSystemSoundsSession()` 返回 S_OK；
//      实测该会话也可能挂在**真实进程**上（本机 pid=2372，属 AudioSrv），
//      拿 `pid == 0` 当判据会漏判。系统声音不属于任何应用，当成应用展示会误导用户。
//
//  ---------------------------------------------------------------------------
//  读不到就如实报（与 CoreAudio.h 的三态纪律一致）：
//      * volume < 0 表示**读不到**，此时 volumeError 必有原因 —— 绝不返回 0 冒充"静音"
//      * muted 必须配 muteValid 一起看 —— 把"读不到"当"未静音"会上报假状态
// ============================================================================

#include <QList>
#include <QString>
#include <QStringList>

namespace WinEase::Win32 {

/// 一个音频会话（某个进程在**默认输出设备**上的一份播放流）
struct AudioSessionInfo {
    /// 会话实例标识（`IAudioSessionControl2::GetSessionInstanceIdentifier`）。
    /// 调音量/静音都靠它定位；**进程每次重新开流都会变** —— 不能当"应用 id"长期保存
    QString instanceId;
    /// 会话标识（`GetSessionIdentifier`）。同一应用的多份会话共享它；可能为空
    QString sessionIdentifier;
    /// 会话所属进程；0 表示系统声音（见 systemSounds）
    quint32 pid = 0;
    /// 进程映像名，如 "chrome.exe"；读不到为空（跨权限读别的用户会话的路径会被拒）
    QString processName;
    /// 会话显示名（应用自己起的名字，例如"正在播放"）；多数应用留空
    QString displayName;
    /// 进程完整路径；读不到为空（**读不到就留空，不猜**）
    QString executablePath;

    /// 会话音量 0.0~1.0；**< 0 表示读不到**（此时 volumeError 有原因）
    float volume = -1.0F;
    QString volumeError;

    /// 是否静音；仅当 muteValid == true 时有意义
    bool muted = false;
    bool muteValid = false;
    QString muteError;

    /// 会话已过期（进程已退出）：不可作为可调对象
    bool expired = false;
    /// 正在出声（AudioSessionStateActive）
    bool active = false;
    /// 状态中文："活动 / 不活动 / 已过期 / 状态未知"
    QString stateText;
    /// 系统声音会话（`IsSystemSoundsSession()` 为真；**不是** pid == 0，见顶部事实 3）
    bool systemSounds = false;

    /// 是否可被调音量/静音（过期会话、拿不到实例标识的会话都不行）
    bool adjustable() const { return !expired && !instanceId.isEmpty(); }

    /// 一句话描述："chrome.exe（PID 1234）"、"系统声音"
    QString describe() const;
};

/// 默认输出设备的 id。
/// ⚠ 它是**设备热插拔的判据**：这个值变了说明默认设备换了，会话集合必须整体重枚举
///   （会话实例 id 是"每设备"的，旧设备上的实例 id 在新设备上不存在）
QString defaultRenderDeviceId();

/// 默认输出设备的友好名，例如 "扬声器 (Realtek(R) Audio)"；没有设备时为空
QString defaultRenderDeviceName();

/// 枚举**默认输出设备**上的全部音频会话（**含已过期的**，隐藏与否交给上层）。
///
/// ⚠ 返回值与 errorOut 的组合有明确含义，调用方不要混：
///      errorOut 为空 + 空列表  → 真的一个会话都没有（正常情形，例如静音的系统）
///      errorOut 非空           → 枚举失败，**此时列表不可信**
/// @param errorOut 失败原因（没有默认输出设备 / 枚举器创建失败 / 中途出错）
QList<AudioSessionInfo> audioSessions(QString *errorOut = nullptr);

/// 设置**一个**会话的音量（入参收敛到 [0, 1]）。只影响这一个会话，绝不动主音量。
/// fail-closed：定位不到该会话就返回 false 并写原因，**不会**"差不多找一份改掉"
bool setAudioSessionVolume(const QString &instanceId, float volume, QString *errorOut = nullptr);

/// 设置**一个**会话的静音状态。只影响这一个会话。
bool setAudioSessionMuted(const QString &instanceId, bool muted, QString *errorOut = nullptr);

/// 批量：把一批会话设成同一音量。
/// **按应用调音量时必须用它**（见顶部事实 1：一个应用可能有好几份会话）。
/// @param failuresOut 逐条收集 "<会话实例id>：原因"（失败就是失败，不吞、不重试）
/// @return 成功笔数（== instanceIds.size() 才算全部成功）
int applySessionVolume(const QStringList &instanceIds,
                       float volume,
                       QStringList *failuresOut = nullptr);

/// 批量：把一批会话设成同一静音状态。语义同 applySessionVolume。
int applySessionMuted(const QStringList &instanceIds,
                      bool muted,
                      QStringList *failuresOut = nullptr);

} // namespace WinEase::Win32
