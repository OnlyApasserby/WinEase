#include "win32/CoreAudio.h"

#include "win32/ComApartment.h"
#include "win32/Win32Error.h"

#include <QDebug>

#include <algorithm>
#include <utility>

#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <propidl.h>
#include <propsys.h>

#include <wrl/client.h>

namespace WinEase::Win32 {

using Microsoft::WRL::ComPtr;

namespace {

/// PKEY_Device_FriendlyName：{A45C254E-DF1C-4EFD-8020-67D146A850E0}, pid 14
/// 自行定义常量，避免依赖 propsys.lib / uuid.lib 提供该符号。
const PROPERTYKEY kDeviceFriendlyNameKey = {
    { 0xA45C254E, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } }, 14
};

} // namespace

// ============================================================================
//  实现（pimpl：不把 mmdeviceapi.h 泄漏到公共头）
// ============================================================================

struct AudioEndpoint::Impl {
    AudioDirection direction = AudioDirection::Render;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioEndpointVolume> volume;
    QString name;
    QString id;

    bool bind(AudioDirection wanted, QString *errorOut);
};

namespace {

/// COM 初始化检查：Core Audio 要求调用线程已初始化 COM
bool ensureComReady(QString *errorOut)
{
    if (isThreadComReady()) {
        return true;
    }
    if (errorOut != nullptr) {
        *errorOut = QStringLiteral("当前线程尚未初始化 COM，无法访问音频设备");
    }
    return false;
}

/// 从设备属性里取友好名。手动解析 PROPVARIANT，避免额外依赖 propsys.lib。
QString deviceFriendlyName(IMMDevice *device)
{
    if (device == nullptr) {
        return QString();
    }

    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store)) || !store) {
        return QString();
    }

    // 聚合初始化即等价于 PropVariantInit（vt 置为 VT_EMPTY）
    PROPVARIANT value{};
    QString name;
    if (SUCCEEDED(store->GetValue(kDeviceFriendlyNameKey, &value))) {
        if (value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
            name = QString::fromWCharArray(value.pwszVal);
        }
    }
    ::PropVariantClear(&value);
    return name;
}

} // namespace

bool AudioEndpoint::Impl::bind(AudioDirection wanted, QString *errorOut)
{
    direction = wanted;

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

    const EDataFlow flow = (wanted == AudioDirection::Render) ? eRender : eCapture;
    hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device);
    if (FAILED(hr) || !device) {
        if (errorOut != nullptr) {
            *errorOut = (hr == E_NOTFOUND)
                            ? QStringLiteral("系统当前没有可用的默认%1设备")
                                  .arg(wanted == AudioDirection::Render ? QStringLiteral("输出")
                                                                        : QStringLiteral("输入"))
                            : describeHresultFailure(QStringLiteral("获取默认音频端点"), hr);
        }
        return false;
    }

    hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &volume);
    if (FAILED(hr) || !volume) {
        if (errorOut != nullptr) {
            *errorOut = describeHresultFailure(QStringLiteral("打开端点音量接口"), hr);
        }
        return false;
    }

    LPWSTR rawId = nullptr;
    if (SUCCEEDED(device->GetId(&rawId)) && rawId != nullptr) {
        id = QString::fromWCharArray(rawId);
        ::CoTaskMemFree(rawId);
    }
    name = deviceFriendlyName(device.Get());
    return true;
}

// ============================================================================
//  AudioEndpoint
// ============================================================================

AudioEndpoint::AudioEndpoint() = default;

AudioEndpoint::~AudioEndpoint() = default;

AudioEndpoint::AudioEndpoint(AudioEndpoint &&other) noexcept
    : m_impl(std::move(other.m_impl))
{
}

AudioEndpoint &AudioEndpoint::operator=(AudioEndpoint &&other) noexcept
{
    if (this != &other) {
        m_impl = std::move(other.m_impl);
    }
    return *this;
}

AudioEndpoint AudioEndpoint::defaultEndpoint(AudioDirection direction, QString *errorOut)
{
    AudioEndpoint endpoint;

    if (!ensureComReady(errorOut)) {
        return endpoint;
    }

    auto impl = std::make_unique<Impl>();
    if (!impl->bind(direction, errorOut)) {
        return endpoint;
    }

    endpoint.m_impl = std::move(impl);
    return endpoint;
}

bool AudioEndpoint::isValid() const
{
    return m_impl != nullptr && m_impl->volume != nullptr;
}

AudioDirection AudioEndpoint::direction() const
{
    return m_impl != nullptr ? m_impl->direction : AudioDirection::Render;
}

QString AudioEndpoint::deviceName() const
{
    return m_impl != nullptr ? m_impl->name : QString();
}

QString AudioEndpoint::deviceId() const
{
    return m_impl != nullptr ? m_impl->id : QString();
}

// ---------------------------------------------------------------------------
//  音量
// ---------------------------------------------------------------------------

float AudioEndpoint::volume(QString *errorOut) const
{
    if (!isValid()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("音频端点未绑定");
        }
        return -1.0F;
    }

    float value = 0.0F;
    const HRESULT hr = m_impl->volume->GetMasterVolumeLevelScalar(&value);
    if (FAILED(hr)) {
        if (errorOut != nullptr) {
            *errorOut = describeHresultFailure(QStringLiteral("读取音量"), hr);
        }
        return -1.0F;
    }
    return std::clamp(value, 0.0F, 1.0F);
}

bool AudioEndpoint::setVolume(float volume, QString *errorOut)
{
    if (!isValid()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("音频端点未绑定");
        }
        return false;
    }

    // 第二个参数是事件上下文 GUID，nullptr 表示使用默认（不发送自定义事件）
    const HRESULT hr = m_impl->volume->SetMasterVolumeLevelScalar(std::clamp(volume, 0.0F, 1.0F),
                                                                  nullptr);
    if (FAILED(hr)) {
        if (errorOut != nullptr) {
            *errorOut = describeHresultFailure(QStringLiteral("设置音量"), hr);
        }
        return false;
    }
    return true;
}

bool AudioEndpoint::stepVolume(float delta, float *newVolumeOut, QString *errorOut)
{
    const float current = volume(errorOut);
    if (current < 0.0F) {
        return false;
    }

    const float target = std::clamp(current + delta, 0.0F, 1.0F);
    if (!setVolume(target, errorOut)) {
        return false;
    }
    if (newVolumeOut != nullptr) {
        *newVolumeOut = target;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  静音
// ---------------------------------------------------------------------------

bool AudioEndpoint::muteState(bool *mutedOut, QString *errorOut) const
{
    if (mutedOut == nullptr) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("muteState 必须提供输出参数");
        }
        return false;
    }
    if (!isValid()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("音频端点未绑定");
        }
        return false;
    }

    BOOL muted = FALSE;
    const HRESULT hr = m_impl->volume->GetMute(&muted);
    if (FAILED(hr)) {
        if (errorOut != nullptr) {
            *errorOut = describeHresultFailure(QStringLiteral("读取静音状态"), hr);
        }
        return false;
    }
    *mutedOut = (muted != FALSE);
    return true;
}

bool AudioEndpoint::isMuted(QString *errorOut) const
{
    // 只给展示用：读失败按"未静音"返回（决策请用 muteState）
    bool muted = false;
    return muteState(&muted, errorOut) && muted;
}

bool AudioEndpoint::setMuted(bool muted, QString *errorOut)
{
    if (!isValid()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("音频端点未绑定");
        }
        return false;
    }

    const HRESULT hr = m_impl->volume->SetMute(muted ? TRUE : FALSE, nullptr);
    if (FAILED(hr)) {
        if (errorOut != nullptr) {
            *errorOut = describeHresultFailure(QStringLiteral("设置静音状态"), hr);
        }
        return false;
    }
    return true;
}

bool AudioEndpoint::toggleMute(bool *newStateOut, QString *errorOut)
{
    // 先读后写，且**读失败就拒绝切换**：读不到当前状态时切换等于盲猜，
    // 用户会得到"按了没反应"或者更糟的"按了反而开麦"
    bool muted = false;
    if (!muteState(&muted, errorOut)) {
        return false;
    }

    const bool target = !muted;
    if (!setMuted(target, errorOut)) {
        return false;
    }
    if (newStateOut != nullptr) {
        *newStateOut = target;
    }
    return true;
}

} // namespace WinEase::Win32
