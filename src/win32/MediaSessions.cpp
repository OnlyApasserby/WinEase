#include "win32/MediaSessions.h"

#include "win32/ComApartment.h"
#include "win32/InputUtils.h"
#include "win32/ProcessUtils.h"
#include "win32/Win32Error.h"
#include "win32/WinRtSupport.h"

#include <QByteArray>
#include <QFileInfo>
#include <QHash>

#include <functional>
#include <thread>

#include <windows.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.Streams.h>

namespace WinEase::Win32 {

namespace {

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media;
using namespace winrt::Windows::Media::Control;

/// WinRT 的 TimeSpan / DateTime 都是 100ns tick（踩坑 #75：别把 tick 当毫秒用）
constexpr qint64 kTicksPerMs = 10000LL;
/// DateTime 的 0 点是 1601-01-01，转 Unix 毫秒要减掉这一段
constexpr qint64 kUnixEpochTicks = 116444736000000000LL;
/// 封面超过这个大小就不读了（读进来也没有意义，还会卡住界面）
constexpr qint64 kMaxArtworkBytes = 16LL * 1024 * 1024;

/// 会话枚举/控制都以"当前线程已初始化 COM"为前提（与 AudioSessions 同口径）
bool ensureComReady(QString *errorOut)
{
    if (isThreadComReady()) {
        return true;
    }
    if (errorOut != nullptr) {
        *errorOut = QStringLiteral("当前线程尚未初始化 COM，无法访问系统媒体会话");
    }
    return false;
}

qint64 ticksToMs(qint64 ticks)
{
    return ticks / kTicksPerMs;
}

qint64 dateTimeToUnixMs(DateTime moment)
{
    return (moment.time_since_epoch().count() - kUnixEpochTicks) / kTicksPerMs;
}

QString fromHString(const winrt::hstring &text)
{
    return QString::fromWCharArray(text.c_str(), static_cast<int>(text.size()));
}

const void *abiOf(const GlobalSystemMediaTransportControlsSession &session)
{
    if (!session) {
        return nullptr;
    }
    return static_cast<const void *>(winrt::get_abi(session));
}

/// 来源串 → 应用名。未打包程序给的是 exe 全路径，打包应用给的是 AUMID。
QString appNameFromSource(const QString &sourceAppId)
{
    if (sourceAppId.isEmpty()) {
        return QStringLiteral("未知来源");
    }
    if (sourceAppId.contains(QLatin1Char('\\')) || sourceAppId.contains(QLatin1Char('/'))
        || sourceAppId.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
        const QString fileName = QFileInfo(sourceAppId).fileName();
        if (!fileName.isEmpty()) {
            return fileName;
        }
    }
    return sourceAppId;
}

/// 来源串 → 进程 id。**只有名字能精确对上**才给 pid，对不上宁可留 0（不猜）。
quint32 resolvePid(const QString &sourceAppId)
{
    const QString appName = appNameFromSource(sourceAppId);
    if (!appName.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
        return 0;
    }
    for (const ProcessInfo &info : findProcessesByName(appName)) {
        if (info.name.compare(appName, Qt::CaseInsensitive) == 0) {
            return info.pid;
        }
    }
    return 0;
}

MediaPlaybackState toState(GlobalSystemMediaTransportControlsSessionPlaybackStatus status)
{
    switch (status) {
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Closed:
        return MediaPlaybackState::Closed;
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Opened:
        return MediaPlaybackState::Opened;
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Changing:
        return MediaPlaybackState::Changing;
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Stopped:
        return MediaPlaybackState::Stopped;
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing:
        return MediaPlaybackState::Playing;
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused:
        return MediaPlaybackState::Paused;
    default:
        return MediaPlaybackState::Unknown;
    }
}

QString unknownExceptionText(const QString &action)
{
    return QStringLiteral("%1 失败：发生未知异常").arg(action);
}

void readMediaProperties(const GlobalSystemMediaTransportControlsSession &session,
                         MediaSessionInfo *info)
{
    try {
        const auto properties = session.TryGetMediaPropertiesAsync().get();
        info->title = fromHString(properties.Title());
        info->artist = fromHString(properties.Artist());
        info->albumTitle = fromHString(properties.AlbumTitle());
    } catch (const winrt::hresult_error &error) {
        info->propertiesError = describeWinRtFailure(QStringLiteral("读取曲目信息"),
                                                     static_cast<long>(error.code().value));
    } catch (...) {
        info->propertiesError = unknownExceptionText(QStringLiteral("读取曲目信息"));
    }
}

void readPlayback(const GlobalSystemMediaTransportControlsSession &session, MediaSessionInfo *info)
{
    try {
        const auto playback = session.GetPlaybackInfo();
        info->playback = toState(playback.PlaybackStatus());
        if (const auto controls = playback.Controls()) {
            info->canPlay = controls.IsPlayEnabled();
            info->canPause = controls.IsPauseEnabled();
            info->canNext = controls.IsNextEnabled();
            info->canPrevious = controls.IsPreviousEnabled();
        }
    } catch (const winrt::hresult_error &error) {
        info->playback = MediaPlaybackState::Unknown;
        info->playbackError = describeWinRtFailure(QStringLiteral("读取播放状态"),
                                                   static_cast<long>(error.code().value));
    } catch (...) {
        info->playback = MediaPlaybackState::Unknown;
        info->playbackError = unknownExceptionText(QStringLiteral("读取播放状态"));
    }
}

/// 进度：三态 —— 完全没提供（位置/时长都 -1 + 原因）、只提供位置（直播）、都提供（普通曲目）
void readTimeline(const GlobalSystemMediaTransportControlsSession &session, MediaSessionInfo *info)
{
    try {
        const auto timeline = session.GetTimelineProperties();
        const qint64 positionTicks = timeline.Position().count();
        const qint64 startTicks = timeline.StartTime().count();
        const qint64 endTicks = timeline.EndTime().count();
        const qint64 updatedTicks = timeline.LastUpdatedTime().time_since_epoch().count();

        const bool anyValue = (positionTicks != 0) || (startTicks != 0) || (endTicks != 0)
                              || (updatedTicks != 0);
        if (!anyValue) {
            info->timelineError = QStringLiteral("该播放器没有提供播放进度");
            return;
        }

        const qint64 durationTicks = endTicks - startTicks;
        info->positionMs = (positionTicks >= 0) ? ticksToMs(positionTicks) : -1;
        info->durationMs = (durationTicks > 0) ? ticksToMs(durationTicks) : -1;
        info->updatedAtMs = (updatedTicks != 0) ? dateTimeToUnixMs(timeline.LastUpdatedTime()) : -1;
    } catch (const winrt::hresult_error &error) {
        info->positionMs = -1;
        info->durationMs = -1;
        info->timelineError = describeWinRtFailure(QStringLiteral("读取播放进度"),
                                                   static_cast<long>(error.code().value));
    } catch (...) {
        info->positionMs = -1;
        info->durationMs = -1;
        info->timelineError = unknownExceptionText(QStringLiteral("读取播放进度"));
    }
}

/// 把一份会话读成 `MediaSessionInfo`。
/// `sessionId` 的拼法：来源串唯一时就是来源串，同一来源有多份会话时追加 `#序号`
/// （SMTC 没有公开唯一 id，只能这么定位）
MediaSessionInfo describeSession(const GlobalSystemMediaTransportControlsSession &session,
                                 QHash<QString, int> *seedCount,
                                 bool isCurrent)
{
    MediaSessionInfo info;
    info.sourceAppId = fromHString(session.SourceAppUserModelId());
    info.appName = appNameFromSource(info.sourceAppId);
    info.pid = resolvePid(info.sourceAppId);

    const int ordinal = seedCount->value(info.sourceAppId, 0);
    seedCount->insert(info.sourceAppId, ordinal + 1);
    info.sessionId = (ordinal == 0)
                         ? info.sourceAppId
                         : QStringLiteral("%1#%2").arg(info.sourceAppId).arg(ordinal);
    info.current = isCurrent;

    readMediaProperties(session, &info);
    readPlayback(session, &info);
    readTimeline(session, &info);
    return info;
}

/// 遍历系统媒体会话。
/// * `wantId` 为空：把每一份会话交给 `visit`（枚举用）；
/// * `wantId` 非空：只把**匹配那一份**交给 `visit`，一份都没匹配上就如实报"会话已消失"。
///
/// ⚠ 这里**不能**把 `GlobalSystemMediaTransportControlsSession` 存进 Qt 容器
/// （C++/WinRT 的投影类型带自己的 `operator new`，Qt 容器里的 placement new 会编不过，
/// 踩坑 #76），所以"找到之后再操作"一律在同一个遍历里用回调完成。
bool visitSessions(const QString &wantId,
                   QString *errorOut,
                   const std::function<void(const MediaSessionInfo &,
                                            const GlobalSystemMediaTransportControlsSession &)>
                       &visit)
{
    if (!ensureComReady(errorOut)) {
        return false;
    }

    bool matched = false;
    const bool ok = runWinRt(QStringLiteral("枚举系统媒体会话"), errorOut, [&] {
        const auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        const auto sessions = manager.GetSessions();
        const auto current = manager.GetCurrentSession();

        // 当前会话：优先按对象同一性认，认不出来再按来源串兜底（占不到就如实不标）
        int currentIndex = -1;
        const void *currentAbi = abiOf(current);
        if (currentAbi != nullptr) {
            int index = 0;
            for (const auto &session : sessions) {
                if (abiOf(session) == currentAbi) {
                    currentIndex = index;
                    break;
                }
                ++index;
            }
            if (currentIndex < 0) {
                const QString currentSource = fromHString(current.SourceAppUserModelId());
                index = 0;
                for (const auto &session : sessions) {
                    if (fromHString(session.SourceAppUserModelId()) == currentSource) {
                        currentIndex = index;
                        break;
                    }
                    ++index;
                }
            }
        }

        QHash<QString, int> seedCount;
        int index = 0;
        for (const auto &session : sessions) {
            const MediaSessionInfo info = describeSession(session, &seedCount, index == currentIndex);
            ++index;
            if (!wantId.isEmpty() && info.sessionId != wantId) {
                continue;
            }
            matched = true;
            visit(info, session);
            if (!wantId.isEmpty()) {
                break;
            }
        }
    });

    if (!ok) {
        return false;
    }
    if (!wantId.isEmpty() && !matched) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("媒体会话「%1」已消失（播放器可能已退出或停止播放）")
                            .arg(wantId);
        }
        return false;
    }
    return true;
}

QString rejectedText(const QString &action)
{
    return QStringLiteral("%1 没有被播放器接受（它可能不允许这个动作，或当前没有可操作的内容）")
        .arg(action);
}

/// 在当前线程上把封面读出来（调用方保证线程已初始化 COM，**且是 MTA**）
QImage readArtworkHere(const QString &sessionId, QString *errorOut)
{
    QImage artwork;
    const bool ok = visitSessions(
        sessionId, errorOut, [&artwork, errorOut](const MediaSessionInfo &, const auto &session) {
            runWinRt(QStringLiteral("读取媒体封面"), errorOut, [&] {
                const auto properties = session.TryGetMediaPropertiesAsync().get();
                const auto reference = properties.Thumbnail();
                if (!reference) {
                    // 不是错误：这份会话就是没有封面（调用方按"没有封面"显示）
                    return;
                }
                const auto stream = reference.OpenReadAsync().get();
                const qint64 size = static_cast<qint64>(stream.Size());
                if (size <= 0) {
                    if (errorOut != nullptr) {
                        *errorOut = QStringLiteral("该会话的封面是空数据");
                    }
                    return;
                }
                if (size > kMaxArtworkBytes) {
                    if (errorOut != nullptr) {
                        *errorOut = QStringLiteral("该会话的封面过大（%1 字节），已跳过").arg(size);
                    }
                    return;
                }

                QByteArray bytes(static_cast<int>(size), Qt::Uninitialized);
                winrt::Windows::Storage::Streams::DataReader reader(stream);
                reader.LoadAsync(static_cast<uint32_t>(size)).get();
                reader.ReadBytes(winrt::array_view<uint8_t>(
                    reinterpret_cast<uint8_t *>(bytes.data()),
                    reinterpret_cast<uint8_t *>(bytes.data()) + bytes.size()));
                reader.Close();

                QImage decoded = QImage::fromData(bytes);
                if (decoded.isNull()) {
                    if (errorOut != nullptr) {
                        *errorOut = QStringLiteral("该会话的封面格式无法识别（%1 字节）").arg(size);
                    }
                    return;
                }
                artwork = decoded;
            });
        });
    if (!ok) {
        return QImage();
    }
    return artwork;
}

} // namespace

QString MediaSessionInfo::describe() const
{
    if (pid != 0) {
        return QStringLiteral("%1（PID %2）").arg(appName).arg(pid);
    }
    return appName;
}

QList<MediaSessionInfo> mediaSessions(QString *errorOut)
{
    QList<MediaSessionInfo> infos;
    const bool ok = visitSessions(QString(),
                                 errorOut,
                                 [&infos](const MediaSessionInfo &info, const auto &) {
                                     infos.append(info);
                                 });
    if (!ok) {
        return {};
    }
    return infos;
}

QString currentMediaSessionId(QString *errorOut)
{
    QString currentId;
    const bool ok = visitSessions(QString(),
                                 errorOut,
                                 [&currentId](const MediaSessionInfo &info, const auto &) {
                                     if (info.current && currentId.isEmpty()) {
                                         currentId = info.sessionId;
                                     }
                                 });
    if (!ok) {
        return QString();
    }
    return currentId;
}

QImage mediaSessionArtwork(const QString &sessionId, QString *errorOut)
{
    // ⚠ 踩坑 #78（实测出来的，不是理论风险）：
    //    `IRandomAccessStreamReference::OpenReadAsync()` 的**完成回调要回到调用线程的公寓**。
    //    在 STA 线程上 `.get()`：如果那个线程正在跑消息循环（主程序的事件循环）也许还能活，
    //    但自检这种"在 main() 里顺序跑、没有消息循环"的 STA 会**当场死等**（实测挂死，
    //    而同一份代码在工作线程上 200ms 内返回）。
    //    → 封面读取一律丢到**自己起的 MTA 工作线程**上做，调用方（界面 / 自检）爱在哪个线程
    //      调都不会炸；线程用完即回收，封面读取本来就是低频操作。
    QImage artwork;
    QString localError;
    std::thread worker([&artwork, &localError, &sessionId] {
        const ComApartment apartment = ComApartment::initialize(ApartmentModel::MultiThreaded);
        if (!apartment.succeeded()) {
            localError = QStringLiteral("读取媒体封面失败：%1").arg(apartment.message());
            return;
        }
        artwork = readArtworkHere(sessionId, &localError);
    });
    worker.join();

    if (errorOut != nullptr) {
        *errorOut = localError;
    }
    return artwork;
}

bool controlMediaSession(const QString &sessionId, MediaCommand command, QString *errorOut)
{
    if (sessionId.isEmpty()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("请先选择一个媒体会话");
        }
        return false;
    }

    bool accepted = false;
    const bool ok = visitSessions(
        sessionId, errorOut, [&accepted, command, errorOut](const MediaSessionInfo &, const auto &session) {
            runWinRt(QStringLiteral("控制媒体会话"), errorOut, [&] {
                switch (command) {
                case MediaCommand::TogglePlayPause:
                    accepted = session.TryTogglePlayPauseAsync().get();
                    break;
                case MediaCommand::Play:
                    accepted = session.TryPlayAsync().get();
                    break;
                case MediaCommand::Pause:
                    accepted = session.TryPauseAsync().get();
                    break;
                case MediaCommand::Next:
                    accepted = session.TrySkipNextAsync().get();
                    break;
                case MediaCommand::Previous:
                    accepted = session.TrySkipPreviousAsync().get();
                    break;
                case MediaCommand::Stop:
                    accepted = session.TryStopAsync().get();
                    break;
                }
            });
        });
    if (!ok) {
        return false;
    }
    if (!accepted) {
        if (errorOut != nullptr) {
            *errorOut = rejectedText(QStringLiteral("该操作"));
        }
        return false;
    }
    return true;
}

bool seekMediaSession(const QString &sessionId, qint64 positionMs, QString *errorOut)
{
    if (positionMs < 0) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("跳转位置不合法（%1 毫秒）").arg(positionMs);
        }
        return false;
    }

    bool accepted = false;
    const bool ok = visitSessions(
        sessionId,
        errorOut,
        [&accepted, positionMs, errorOut](const MediaSessionInfo &, const auto &session) {
            runWinRt(QStringLiteral("跳转播放位置"), errorOut, [&] {
                accepted = session.TryChangePlaybackPositionAsync(positionMs * kTicksPerMs).get();
            });
        });
    if (!ok) {
        return false;
    }
    if (!accepted) {
        if (errorOut != nullptr) {
            *errorOut = rejectedText(QStringLiteral("跳转"));
        }
        return false;
    }
    return true;
}

bool sendMediaKey(MediaKey key, QString *errorOut)
{
    unsigned int virtualKey = 0;
    switch (key) {
    case MediaKey::PlayPause:
        virtualKey = VK_MEDIA_PLAY_PAUSE;
        break;
    case MediaKey::NextTrack:
        virtualKey = VK_MEDIA_NEXT_TRACK;
        break;
    case MediaKey::PreviousTrack:
        virtualKey = VK_MEDIA_PREV_TRACK;
        break;
    case MediaKey::Stop:
        virtualKey = VK_MEDIA_STOP;
        break;
    }

    if (virtualKey == 0) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("不认识的媒体键");
        }
        return false;
    }

    // 复用平台层既有的按键合成路径（按下 + 抬起，不带修饰键）
    if (!sendKeyChord(virtualKey)) {
        if (errorOut != nullptr) {
            *errorOut = describeFailure(QStringLiteral("发送媒体键（%1）").arg(mediaKeyName(key)),
                                        ::GetLastError());
        }
        return false;
    }
    return true;
}

QString mediaKeyName(MediaKey key)
{
    switch (key) {
    case MediaKey::PlayPause:
        return QStringLiteral("播放/暂停");
    case MediaKey::NextTrack:
        return QStringLiteral("下一首");
    case MediaKey::PreviousTrack:
        return QStringLiteral("上一首");
    case MediaKey::Stop:
        return QStringLiteral("停止");
    }
    return QStringLiteral("未知媒体键");
}

QString mediaPlaybackStateText(MediaPlaybackState state)
{
    switch (state) {
    case MediaPlaybackState::Closed:
        return QStringLiteral("已结束");
    case MediaPlaybackState::Opened:
        return QStringLiteral("已打开（未播放）");
    case MediaPlaybackState::Changing:
        return QStringLiteral("切换中");
    case MediaPlaybackState::Stopped:
        return QStringLiteral("已停止");
    case MediaPlaybackState::Playing:
        return QStringLiteral("正在播放");
    case MediaPlaybackState::Paused:
        return QStringLiteral("已暂停");
    case MediaPlaybackState::Unknown:
        return QStringLiteral("状态读不到");
    }
    return QStringLiteral("状态读不到");
}

QString mediaSessionKey(const MediaSessionInfo &session)
{
    return session.sessionId;
}

} // namespace WinEase::Win32
