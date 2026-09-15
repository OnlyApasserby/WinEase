#pragma once

// ============================================================================
//  MediaSessions —— 系统媒体会话（P3-11 媒体控制面板）
//
//  与 AudioSessions（音量会话）刻意分开：这里读的是 **SMTC 媒体会话**
//  （`GlobalSystemMediaTransportControlsSessionManager`），
//  也就是"任务栏那个媒体浮出控件里显示的东西"。
//
//  三态纪律（与 AudioSessions 同口径）：
//      * 读不到就**写原因**，绝不编一个 0（`positionMs = 0` 会被界面写成 "0:00"，
//        那是在说"这首歌刚开始"，是假话）；
//      * 时长缺失 ≠ 进度为 0：很多直播/网页播放器就是不给 EndTime；
//      * 控制失败必须如实回传原因（播放器有可能会拒绝某个动作）。
//
//  ⚠ 时间单位：WinRT 的 TimeSpan 以 **100ns tick** 为单位，
//  本层对外一律用**毫秒**（`ms`），换算只在本层内部发生（踩坑 #75）。
// ============================================================================

#include <QImage>
#include <QList>
#include <QString>

namespace WinEase::Win32 {

/// 媒体键（等价于键盘上的那颗键，作用于**系统当前**会话）
enum class MediaKey {
    PlayPause,
    NextTrack,
    PreviousTrack,
    Stop,
};

/// 对**指定会话**的控制命令（SMTC 精确控制，不走媒体键）
enum class MediaCommand {
    TogglePlayPause,
    Play,
    Pause,
    Next,
    Previous,
    Stop,
};

/// 播放状态（与 `GlobalSystemMediaTransportControlsSessionPlaybackStatus` 一一对应，
/// 另加 Unknown = 读不到）。
/// ⚠ 别用 `Windows.Media.MediaPlaybackStatus`（那是 SMTC **发布方**的枚举，
/// 值序也不一样：`Playing` 在那边是 3、在这边是 4，见踩坑 #75）
enum class MediaPlaybackState {
    Closed = 0,
    Opened,
    Changing,
    Stopped,
    Playing,
    Paused,
    Unknown, ///< 读取失败（原因另见 playbackError）
};

/// 一个媒体会话（一份会话 = 播放器给系统注册的一条播放记录）
struct MediaSessionInfo {
    /// 会话定位串：由来源 + 同名来源的序号构成（SMTC 没有公开的唯一 id，
    /// 所以本层自己拼一个稳定串，控制时重新枚举并按它匹配）
    QString sessionId;
    /// `SourceAppUserModelId` 原文（未打包程序通常是 exe 全路径，打包应用是 AUMID）
    QString sourceAppId;
    /// 由来源推导出的应用名（"chrome.exe" / AUMID 原文）；推不出就给来源原文
    QString appName;
    /// 进程 id；**只有能精确对上进程名时**才非 0（对不上宁可留 0，不猜）
    quint32 pid = 0;

    /// 是否系统"当前"会话 —— 媒体键打到的就是它
    bool current = false;

    MediaPlaybackState playback = MediaPlaybackState::Unknown;
    QString playbackError;

    QString title;
    QString artist;
    QString albumTitle;
    /// 读不到曲目信息的原因（空 = 读到了，但播放器可能确实没给标题）
    QString propertiesError;

    /// 播放位置（毫秒；<0 = 读不到）。注意这是**采样时刻**的位置：
    /// 采样时刻记在 updatedAtMs，界面要显示"这一刻"的位置得由纯函数外推
    qint64 positionMs = -1;
    /// 当前曲目的时长（毫秒；<=0 = 播放器没给时长，不能当成 0）
    qint64 durationMs = -1;
    /// 位置采样时刻（Unix 毫秒；<=0 = 未知）
    qint64 updatedAtMs = -1;
    QString timelineError;

    /// 播放器自己声明的能力（界面据此决定按钮是否可用，不要自己猜）
    bool canPlay = false;
    bool canPause = false;
    bool canNext = false;
    bool canPrevious = false;

    /// 能不能把这个会话作为控制目标（有定位串即可）
    bool controllable() const { return !sessionId.isEmpty(); }
    QString describe() const;
};

/// 枚举系统里的全部媒体会话。
/// `errorOut` 为空 + 空列表 = 真的没有会话；`errorOut` 非空 = 枚举失败（列表不可信）
QList<MediaSessionInfo> mediaSessions(QString *errorOut = nullptr);

/// 系统"当前"会话的定位串（媒体键的作用对象）；没有当前会话时返回空串
QString currentMediaSessionId(QString *errorOut = nullptr);

/// 读某个会话的封面图。
/// 返回空图且 `errorOut` 为空 = 该会话**没有提供封面**（不是错误）；
/// 返回空图且 `errorOut` 非空 = 读失败（原因在里面）
///
/// ⚠ 本函数内部会**换到 MTA 工作线程**去读（读封面用的 `OpenReadAsync` 要求
/// 完成回调能回到调用者公寓，在没在跑消息循环的 STA 线程上直接等会死锁，踩坑 #78），
/// 所以界面线程可以直接调，不会卡住事件循环 —— 但它是**同步**的，会阻塞到读完为止。
QImage mediaSessionArtwork(const QString &sessionId, QString *errorOut = nullptr);

/// 对指定会话下命令（它是否被接受由播放器决定，返回 false 时 errorOut 给原因）
bool controlMediaSession(const QString &sessionId,
                         MediaCommand command,
                         QString *errorOut = nullptr);

/// 跳到指定播放位置（毫秒）
bool seekMediaSession(const QString &sessionId, qint64 positionMs, QString *errorOut = nullptr);

/// 发一个媒体键（`SendInput`）—— 由**系统**决定把它交给哪个会话（当前会话）
bool sendMediaKey(MediaKey key, QString *errorOut = nullptr);

/// 媒体键的中文名（界面与日志共用）
QString mediaKeyName(MediaKey key);
/// 播放状态的中文描述
QString mediaPlaybackStateText(MediaPlaybackState state);
/// 会话定位串（自检与界面共用同一套规则）
QString mediaSessionKey(const MediaSessionInfo &session);

} // namespace WinEase::Win32
