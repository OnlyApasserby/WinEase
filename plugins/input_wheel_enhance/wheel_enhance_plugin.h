#pragma once

// ============================================================================
//  WheelEnhancePlugin —— P2-05 滚轮增强：任务栏任意位置滚动调节音量
//
//  为什么只做"任务栏"这一处（路线图里的判断）：
//      "滚轮增强"最容易摊大饼 —— 浏览器标签栏切标签、任意位置调音量、
//      窗口不聚焦也能滚……这些都会跟用户已有的肌肉记忆打架。
//      任务栏滚动在 Windows 上**本来什么也不做**，接管它不夺走任何既有行为，
//      是收益/风险比最高的那一个。
//
//  ★ 两条纪律：
//    ① **只吞自己该吞的事件**：光标不在任务栏上（或修饰键不符合配置）→
//       回调立刻返回 false，事件原样放行；绝不做"全局吞滚轮"。
//    ② **钩子线程里不做慢活**：Direct 回调只干三件事 —— 判类型、判位置、
//       把"几格"投递到主线程；读音量/写音量（COM 调用，毫秒级）全在主线程做。
//       理由：钩子回调卡住会拖慢**全局**输入，超预算还会被自动降级（见 HookService.h）。
//       代价是"算音量时读到的当前值可能已被别的程序改过" —— 这个代价必须付，
//       而且我们在主线程里**重新读一次再算**，所以不会算错，只会反映最新值。
//
//  ⚠ 有意不做：不抢浏览器标签栏（P2-05 的裁剪项 C-x）、不做"任意位置滚轮调音量"。
// ============================================================================

#include "sdk/HookService.h"
#include "sdk/IFeaturePlugin.h"
#include "win32/CoreAudio.h"

#include <QElapsedTimer>
#include <QString>

class WheelVolumeListener;

class WheelEnhancePlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "wheel_enhance_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit WheelEnhancePlugin(QObject *parent = nullptr);

    // ---------------- 元信息 ----------------
    QString id() const override;
    QString name() const override;
    QString description() const override;
    QIcon icon() const override;
    WinEase::FeatureCategory category() const override;
    QStringList tags() const override;

    // ---------------- 能力标记 ----------------
    bool supportsHotkey() const override;
    bool hasSettings() const override;

    // ---------------- 生命周期 ----------------
    bool initialize() override;
    void shutdown() override;
    QWidget *createSettingsWidget(QWidget *parent = nullptr) override;

    // ---------------- 自检共用的只读接口 ----------------
    /// 本次启用以来真正调过音量的格数（含方向，正负相抵）
    int appliedNotches() const { return m_appliedNotches; }
    /// 最近一次动作的中文说明（"音量 45%（扬声器 …）" / 失败原因）
    QString lastStatusText() const { return m_lastStatusText; }
    /// 最近一次用到的音频设备名（空 = 还没成功拿到端点）
    QString deviceName() const { return m_deviceName; }
    /// 订阅是否生效（钩子服务在跑且监听器已建）
    bool isListening() const { return m_listener != nullptr; }

    int stepPercent() const { return m_stepPercent; }
    QString requireModifiersKey() const { return m_requireModifiers; }
    bool invert() const { return m_invert; }

protected:
    bool canEnable(QString *reason) const override;
    bool onEnable() override;
    void onDisable() override;

private:
    void loadFromConfig();
    /// 钩子线程回调（必须极快返回）；返回 true 表示吞掉该滚轮事件
    bool handleWheelOnHookThread(const WinEase::HookEvent &event);
    /// 主线程执行真正的音量调整（COM 调用）
    void applyNotches(int notches);
    /// 通知卡片状态（带节流，避免连续滚动把界面刷爆）
    void reportStatus(const QString &text, bool force = false);
    /// 匹配配置里的修饰键要求
    WinEase::HookModifiers requiredModifiers() const;

    WheelVolumeListener *m_listener = nullptr;
    /// 音频端点按需创建并缓存（设备热插拔后失效 → 自动重建）
    WinEase::Win32::AudioEndpoint m_endpoint;

    int m_stepPercent = 5;
    QString m_requireModifiers = QStringLiteral("none");
    bool m_invert = false;
    bool m_showStatus = true;

    /// 滚轮余量（< WHEEL_DELTA 的部分攒着）—— 只在钩子线程里读写
    int m_accumulator = 0;

    int m_appliedNotches = 0;
    QString m_lastStatusText;
    QString m_deviceName;
    QElapsedTimer m_statusThrottle;
};
