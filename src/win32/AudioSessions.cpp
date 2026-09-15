#include "win32/AudioSessions.h"

#include "win32/ComApartment.h"
#include "win32/CoreAudio.h"
#include "win32/ProcessUtils.h"
#include "win32/Win32Error.h"

#include <QSet>

#include <algorithm>

#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <wrl/client.h>

namespace WinEase::Win32 {

using Microsoft::WRL::ComPtr;

namespace {

/// COM 初始化检查：Core Audio 要求调用线程已初始化 COM（与 CoreAudio.cpp 同口径）
bool ensureComReady(QString *errorOut)
{
    if (isThreadComReady()) {
        return true;
    }
    if (errorOut != nullptr) {
        *errorOut = QStringLiteral("当前线程尚未初始化 COM，无法访问音频会话");
    }
    return false;
}

QString sessionStateText(AudioSessionState state)
{
    switch (state) {
    case AudioSessionStateActive:
        return QStringLiteral("活动");
    case AudioSessionStateInactive:
        return QStringLiteral("不活动");
    case AudioSessionStateExpired:
        return QStringLiteral("已过期");
    }
    return QStringLiteral("状态未知");
}

/// 会话实例标识的唯一取法（实现见下方；此处前置声明以便读取路径也共用同一份）
QString sessionInstanceId(IAudioSessionControl *control);

/// 取一个会话的显示模型（只读，任何字段读不到都如实留在 error 字段里）
AudioSessionInfo describeSession(IAudioSessionControl *control)
{
    AudioSessionInfo info;
    if (control == nullptr) {
        info.stateText = QStringLiteral("状态未知");
        info.volumeError = QStringLiteral("会话控制接口为空");
        info.muteError = info.volumeError;
        return info;
    }

    AudioSessionState state = AudioSessionStateInactive;
    if (SUCCEEDED(control->GetState(&state))) {
        info.expired = (state == AudioSessionStateExpired);
        info.active = (state == AudioSessionStateActive);
        info.stateText = sessionStateText(state);
    } else {
        info.stateText = QStringLiteral("状态未知");
    }

    ComPtr<IAudioSessionControl2> control2;
    control->QueryInterface(IID_PPV_ARGS(&control2));

    if (control2) {
        DWORD pid = 0;
        if (SUCCEEDED(control2->GetProcessId(&pid))) {
            info.pid = static_cast<quint32>(pid);
        }
        // 判"系统声音"用的是 IsSystemSoundsSession()，**不是 PID 0**：
        // 实测系统声音会话也可能属于一个真实进程（本机 pid=2372），拿 pid 判会误判
        info.systemSounds = (control2->IsSystemSoundsSession() == S_OK);

        LPWSTR rawId = nullptr;
        if (SUCCEEDED(control2->GetSessionIdentifier(&rawId)) && rawId != nullptr) {
            info.sessionIdentifier = QString::fromWCharArray(rawId);
            ::CoTaskMemFree(rawId);
        }
    }

    // 实例标识：与写入路径共用同一份实现（两侧一旦各写一遍就可能对不上，见踩坑 #62）
    info.instanceId = sessionInstanceId(control);

    LPWSTR rawName = nullptr;
    if (SUCCEEDED(control->GetDisplayName(&rawName)) && rawName != nullptr) {
        info.displayName = QString::fromWCharArray(rawName);
        ::CoTaskMemFree(rawName);
    }

    // 进程信息：**读不到就留空**（跨权限读别的用户会话会被拒），不编造
    if (!info.systemSounds && info.pid != 0) {
        info.processName = processName(info.pid);
        info.executablePath = processPath(info.pid);
    }

    // 音量与静音：三态（读到了 / 读不到但**有原因**）
    ComPtr<ISimpleAudioVolume> volume;
    const HRESULT hr = control->QueryInterface(IID_PPV_ARGS(&volume));
    if (FAILED(hr) || !volume) {
        info.volumeError = describeHresultFailure(QStringLiteral("打开会话音量接口"), hr);
        info.muteError = info.volumeError;
        return info;
    }

    float value = 0.0F;
    const HRESULT volumeHr = volume->GetMasterVolume(&value);
    if (SUCCEEDED(volumeHr)) {
        info.volume = std::clamp(value, 0.0F, 1.0F);
    } else {
        info.volumeError = describeHresultFailure(QStringLiteral("读取会话音量"), volumeHr);
    }

    BOOL muted = FALSE;
    const HRESULT muteHr = volume->GetMute(&muted);
    if (SUCCEEDED(muteHr)) {
        info.muted = (muted != FALSE);
        info.muteValid = true;
    } else {
        info.muteError = describeHresultFailure(QStringLiteral("读取会话静音状态"), muteHr);
    }

    return info;
}

/// 绑定默认输出设备上的会话管理器（枚举与会话写入的共同前置）
bool bindSessionManager(ComPtr<IAudioSessionManager2> *out, QString *errorOut)
{
    if (!ensureComReady(errorOut)) {
        return false;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                    nullptr,
                                    CLSCTX_ALL,
                                    IID_PPV_ARGS(&enumerator));
    if (FAILED(hr) || !enumerator) {
        if (errorOut != nullptr) {
            *errorOut = describeHresultFailure(QStringLiteral("创建音频设备枚举器"), hr);
        }
        return false;
    }

    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr) || !device) {
        if (errorOut != nullptr) {
            *errorOut = (hr == E_NOTFOUND)
                            ? QStringLiteral("系统当前没有可用的默认输出设备")
                            : describeHresultFailure(QStringLiteral("获取默认输出设备"), hr);
        }
        return false;
    }

    hr = device->Activate(__uuidof(IAudioSessionManager2),
                          CLSCTX_ALL,
                          nullptr,
                          reinterpret_cast<void **>(out->ReleaseAndGetAddressOf()));
    if (FAILED(hr) || !*out) {
        if (errorOut != nullptr) {
            *errorOut = describeHresultFailure(QStringLiteral("打开音频会话管理器"), hr);
        }
        return false;
    }
    return true;
}

/// 会话实例标识的**唯一**取法：读取路径与写入路径都必须走这里。
///
/// 为什么单独抽出来（踩坑 #62）：读写两条路径各自写一遍这段逻辑时，
/// 一旦有一处用了 GetSessionIdentifier（会话标识）而不是 GetSessionInstanceIdentifier
/// （实例标识），就会出现"面板上明明列出了这个会话，滑块拖了却没反应"——
/// 两个字符串长得很像，肉眼比对看不出来。共用一处实现才能保证两侧一定对得上。
QString sessionInstanceId(IAudioSessionControl *control)
{
    if (control == nullptr) {
        return QString();
    }
    ComPtr<IAudioSessionControl2> control2;
    if (FAILED(control->QueryInterface(IID_PPV_ARGS(&control2))) || !control2) {
        return QString();
    }
    LPWSTR rawInstance = nullptr;
    if (FAILED(control2->GetSessionInstanceIdentifier(&rawInstance)) || rawInstance == nullptr) {
        return QString();
    }
    const QString instanceId = QString::fromWCharArray(rawInstance);
    ::CoTaskMemFree(rawInstance);
    return instanceId;
}

QString missingSessionText(const QString &instanceId)
{
    Q_UNUSED(instanceId)
    return QStringLiteral("找不到该音频会话（应用可能已关闭，或会话已过期）");
}

/// 在**一次枚举快照**内，对给定的会话实例 id 逐个执行动作。
///
/// 为什么必须"先枚举一次再逐个写"（而不是每个 id 各枚举一遍）：
/// 每次枚举都是独立快照，中间会有会话来去；同一批操作必须落在同一个快照上，
/// 否则会出现"改了 3 个里 2 个，第 3 个已经不在快照里"这种解释不清的失败。
///
/// 为什么不建"id → 音量接口"的表（踩坑 #61）：**COM 接口指针不放进 Qt 容器**。
/// 实测把 ComPtr<ISimpleAudioVolume> 塞进 QHash 后，用**逐字符完全相同**的 key
/// 也查不回来（qHash 命不中），结果是"列表里读得到的会话反而写不进去"。
/// 这里的容器只装字符串，COM 指针全程留在枚举循环里。
template <typename Action>
int applyToSessions(const QStringList &instanceIds,
                    const Action &action,
                    QStringList *failuresOut)
{
    if (instanceIds.isEmpty()) {
        return 0;
    }

    ComPtr<IAudioSessionManager2> manager;
    QString error;
    if (!bindSessionManager(&manager, &error)) {
        if (failuresOut != nullptr) {
            failuresOut->append(error);
        }
        return 0;
    }

    ComPtr<IAudioSessionEnumerator> sessions;
    HRESULT hr = manager->GetSessionEnumerator(&sessions);
    if (FAILED(hr) || !sessions) {
        if (failuresOut != nullptr) {
            failuresOut->append(describeHresultFailure(QStringLiteral("取音频会话枚举器"), hr));
        }
        return 0;
    }

    int count = 0;
    hr = sessions->GetCount(&count);
    if (FAILED(hr)) {
        if (failuresOut != nullptr) {
            failuresOut->append(describeHresultFailure(QStringLiteral("统计音频会话数量"), hr));
        }
        return 0;
    }

    QSet<QString> pending;
    for (const QString &instanceId : instanceIds) {
        if (!instanceId.isEmpty()) {
            pending.insert(instanceId);
        }
    }

    int succeeded = 0;
    for (int index = 0; index < count; ++index) {
        ComPtr<IAudioSessionControl> control;
        if (FAILED(sessions->GetSession(index, &control)) || !control) {
            continue; // 单个会话取不到不拖垮整批（最后按"找不到"如实报）
        }

        const QString instanceId = sessionInstanceId(control.Get());
        if (instanceId.isEmpty() || !pending.contains(instanceId)) {
            continue;
        }
        pending.remove(instanceId); // 一个 id 在一次枚举里只会命中一份会话

        ComPtr<ISimpleAudioVolume> volume;
        const HRESULT volumeHr = control->QueryInterface(IID_PPV_ARGS(&volume));
        if (FAILED(volumeHr) || !volume) {
            if (failuresOut != nullptr) {
                failuresOut->append(QStringLiteral("%1：%2")
                                        .arg(instanceId,
                                             describeHresultFailure(
                                                 QStringLiteral("打开会话音量接口"), volumeHr)));
            }
            continue;
        }

        QString actionError;
        if (action(volume.Get(), &actionError)) {
            ++succeeded;
        } else if (failuresOut != nullptr) {
            failuresOut->append(QStringLiteral("%1：%2").arg(instanceId, actionError));
        }
    }

    // 快照里压根没找到的 id：必须如实报，不能"少改几个也算成功"
    for (const QString &missing : pending) {
        if (failuresOut != nullptr) {
            failuresOut->append(QStringLiteral("%1：%2").arg(missing, missingSessionText(missing)));
        }
    }
    return succeeded;
}

} // namespace

// ============================================================================
//  AudioSessionInfo
// ============================================================================

QString AudioSessionInfo::describe() const
{
    if (systemSounds) {
        return QStringLiteral("系统声音");
    }
    const QString label = processName.isEmpty() ? QStringLiteral("未知进程") : processName;
    return QStringLiteral("%1（PID %2）").arg(label).arg(pid);
}

// ============================================================================
//  设备
// ============================================================================

QString defaultRenderDeviceId()
{
    QString error;
    const AudioEndpoint endpoint = AudioEndpoint::defaultEndpoint(AudioDirection::Render, &error);
    return endpoint.isValid() ? endpoint.deviceId() : QString();
}

QString defaultRenderDeviceName()
{
    QString error;
    const AudioEndpoint endpoint = AudioEndpoint::defaultEndpoint(AudioDirection::Render, &error);
    return endpoint.isValid() ? endpoint.deviceName() : QString();
}

// ============================================================================
//  枚举
// ============================================================================

QList<AudioSessionInfo> audioSessions(QString *errorOut)
{
    QList<AudioSessionInfo> result;

    ComPtr<IAudioSessionManager2> manager;
    if (!bindSessionManager(&manager, errorOut)) {
        return result;
    }

    ComPtr<IAudioSessionEnumerator> sessions;
    HRESULT hr = manager->GetSessionEnumerator(&sessions);
    if (FAILED(hr) || !sessions) {
        if (errorOut != nullptr) {
            *errorOut = describeHresultFailure(QStringLiteral("取音频会话枚举器"), hr);
        }
        return result;
    }

    int count = 0;
    hr = sessions->GetCount(&count);
    if (FAILED(hr)) {
        if (errorOut != nullptr) {
            *errorOut = describeHresultFailure(QStringLiteral("统计音频会话数量"), hr);
        }
        return result;
    }

    int skipped = 0;
    for (int index = 0; index < count; ++index) {
        ComPtr<IAudioSessionControl> control;
        if (FAILED(sessions->GetSession(index, &control)) || !control) {
            ++skipped;
            continue;
        }
        result.append(describeSession(control.Get()));
    }

    // 有跳过就必须说出来：静默少几条会让用户以为"这个应用没有声音会话"
    if (skipped > 0 && errorOut != nullptr) {
        *errorOut = QStringLiteral("有 %1 个音频会话读取失败（其它 %2 个已列出）")
                        .arg(skipped)
                        .arg(result.size());
    }
    return result;
}

// ============================================================================
//  写入
// ============================================================================

bool setAudioSessionVolume(const QString &instanceId, float volume, QString *errorOut)
{
    if (instanceId.isEmpty()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("会话实例标识为空，无法定位会话");
        }
        return false;
    }

    // 单条入口走同一份实现（不另写一遍查找逻辑）：失败清单的第一行就是具体原因
    QStringList failures;
    if (applySessionVolume({ instanceId }, volume, &failures) == 1) {
        return true;
    }
    if (errorOut != nullptr) {
        *errorOut = failures.isEmpty()
                        ? QStringLiteral("设置会话音量失败（未取到失败原因）")
                        : failures.first();
    }
    return false;
}

bool setAudioSessionMuted(const QString &instanceId, bool muted, QString *errorOut)
{
    if (instanceId.isEmpty()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("会话实例标识为空，无法定位会话");
        }
        return false;
    }

    QStringList failures;
    if (applySessionMuted({ instanceId }, muted, &failures) == 1) {
        return true;
    }
    if (errorOut != nullptr) {
        *errorOut = failures.isEmpty()
                        ? QStringLiteral("设置会话静音失败（未取到失败原因）")
                        : failures.first();
    }
    return false;
}

int applySessionVolume(const QStringList &instanceIds, float volume, QStringList *failuresOut)
{
    const float clamped = std::clamp(volume, 0.0F, 1.0F);
    return applyToSessions(
        instanceIds,
        [clamped](ISimpleAudioVolume *target, QString *actionError) {
            // 第二个参数是事件上下文 GUID，nullptr 表示使用默认（不发送自定义事件）
            const HRESULT hr = target->SetMasterVolume(clamped, nullptr);
            if (FAILED(hr)) {
                if (actionError != nullptr) {
                    *actionError = describeHresultFailure(QStringLiteral("设置会话音量"), hr);
                }
                return false;
            }
            return true;
        },
        failuresOut);
}

int applySessionMuted(const QStringList &instanceIds, bool muted, QStringList *failuresOut)
{
    return applyToSessions(
        instanceIds,
        [muted](ISimpleAudioVolume *target, QString *actionError) {
            // 第二个参数同样是事件上下文 GUID，nullptr 表示使用默认
            const HRESULT hr = target->SetMute(muted ? TRUE : FALSE, nullptr);
            if (FAILED(hr)) {
                if (actionError != nullptr) {
                    *actionError = describeHresultFailure(QStringLiteral("设置会话静音状态"), hr);
                }
                return false;
            }
            return true;
        },
        failuresOut);
}

} // namespace WinEase::Win32
