#pragma once

// ============================================================================
//  CoreAudio.h —— 音频端点音量控制（WASAPI / Core Audio）
//
//  用途：滚轮调音量（P2-05）、麦克风一键静音（P2-10）、音量混合器（P3-09 基础）
//
//  设计说明：
//      使用 Core Audio 的 IAudioEndpointVolume，作用对象是**默认音频端点**。
//      端点句柄构造时绑定，因此：
//        * 设备热插拔（插入耳机导致默认设备切换）后需要重新创建实例
//        * 需要调用线程已初始化 COM（见 ComApartment）
//
//  注意：本类**不产生任何系统级状态**，只做即时读写，因此无需快照/还原。
//        （音量是用户可见的系统设置，功能停用时不应"还原"用户手动调过的值）
// ============================================================================

#include <QString>

#include <memory>

namespace WinEase::Win32 {

/// 音频端点方向
enum class AudioDirection {
    Render, ///< 输出：扬声器 / 耳机 / HDMI
    Capture ///< 输入：麦克风
};

class AudioEndpoint
{
public:
    AudioEndpoint();
    ~AudioEndpoint();

    AudioEndpoint(AudioEndpoint &&other) noexcept;
    AudioEndpoint &operator=(AudioEndpoint &&other) noexcept;
    AudioEndpoint(const AudioEndpoint &) = delete;
    AudioEndpoint &operator=(const AudioEndpoint &) = delete;

    /// 绑定到默认音频端点
    static AudioEndpoint defaultEndpoint(AudioDirection direction, QString *errorOut = nullptr);

    bool isValid() const;
    AudioDirection direction() const;

    /// 设备友好名，例如 "扬声器 (Realtek(R) Audio)"
    QString deviceName() const;

    /// 设备 ID（用于将来做设备切换时定位）
    QString deviceId() const;

    /// ⚠ 仅供**展示**（"当前处于静音"这种一句话提示）：读失败时返回 false，
    ///    与"未静音"无法区分。任何**决策**（切换、判断、上报告警）都必须用 muteState()。
    bool isMuted(QString *errorOut = nullptr) const;

    // ---------------- 音量 ----------------

    /// 主音量，范围 0.0 ~ 1.0；失败返回 -1.0
    float volume(QString *errorOut = nullptr) const;

    /// 设置主音量，入参会收敛到 [0, 1]
    bool setVolume(float volume, QString *errorOut = nullptr);

    /// 按步长增减音量（滚轮调节用）
    /// @param delta 例如 +0.02f / -0.02f
    /// @param newVolumeOut 可选，返回调整后的音量
    bool stepVolume(float delta, float *newVolumeOut = nullptr, QString *errorOut = nullptr);

    // ---------------- 静音 ----------------

    /// 读静音状态（**权威接口**）：成功返回 true 并写入 mutedOut。
    ///
    /// ⚠ 静音状态本质是**三态**的：静音 / 未静音 / 读不到。
    ///    失败的路径（端点未绑定、`GetMute` 失败）必须由返回值和 errorOut 表达出来 ——
    ///    对"一键闭麦"这类隐私功能，把"读不到"当成"未静音"会直接导致
    ///    "用户以为闭麦了、其实开着"，这比不给答案危险得多。
    bool muteState(bool *mutedOut, QString *errorOut = nullptr) const;

    bool setMuted(bool muted, QString *errorOut = nullptr);

    /// 切换静音状态（内部走 muteState，读不到就拒绝切换，绝不猜）
    /// @param newStateOut 可选，返回切换后是否处于静音
    bool toggleMute(bool *newStateOut = nullptr, QString *errorOut = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace WinEase::Win32
