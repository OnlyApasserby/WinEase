#pragma once

// ============================================================================
//  hardware_hud_plugin.h —— 硬件监控悬浮窗（P3-07 第一批）
//
//  形态与 P1-07 屏幕标尺 / P1-08 焦点高亮一致：
//      插件自己 new 一个 OverlayWindow 子类（HudOverlay）→ overlayHost()->adoptOverlay()
//      → 所有权归宿主，插件永不 delete（停用时用 closeOverlaysOfOwner 请求回收）。
//
//  取数一律复用平台层 src/win32/SystemInfo（它头注释里写明用途就是本功能）：
//      * 一次性：Win32::memoryInfo() / Win32::diskTemperatures()
//      * 有状态：Win32::CpuSampler / NetworkSampler / GpuSampler
//        —— SystemInfo 刻意不带全局状态，采样状态必须由**插件成员**持有
//
//  本批范围（ROADMAP-P3 的 P3-07 第一批）：
//      磁盘温度 + CPU/内存/网速/GPU 利用率，五项零依赖指标。
//      CPU / 主板温度需要 PawnIOLib.dll（本机缺失），按"如实报不可用 + 给获取指引"
//      处理，留给第二里程碑。
// ============================================================================

#include "HudMetrics.h"
#include "sdk/IFeaturePlugin.h"
#include "win32/SystemInfo.h"

#include <QPoint>
#include <QPointer>
#include <QTimer>

class QCheckBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace WinEase::FeaturePlugins {

class HudOverlay;

class HardwareHudPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "hardware_hud_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit HardwareHudPlugin(QObject *parent = nullptr);
    ~HardwareHudPlugin() override;

    // ---------------- 元信息 ----------------
    QString id() const override;
    QString name() const override;
    QString description() const override;
    QString detailedDescription() const override;
    QIcon icon() const override;
    WinEase::FeatureCategory category() const override;
    QStringList tags() const override;

    // ---------------- 能力 ----------------
    bool supportsHotkey() const override;
    QKeySequence defaultHotkey() const override;

    bool initialize() override;
    void shutdown() override;
    QWidget *createSettingsWidget(QWidget *parent = nullptr) override;

protected:
    bool canEnable(QString *reason) const override;
    bool onEnable() override;
    void onDisable() override;
    void onHotkey(const QString &hotkeyId) override;

private:
    /// 采一次样 → 组装显示模型 → 推给面板 + 上报状态（定时器的槽）
    void tick();
    /// 采集原始读数（含"CPU 温度本批不可用"的固定原因）。
    /// ⚠ 非 const：采样器要推进内部基准（SystemInfo 刻意不带全局状态，状态就在这两个成员上）
    Hud::MetricInput collectInput();

    bool ensureOverlay();
    void destroyOverlay();
    void pushSnapshot();

    void loadOptions();
    void setInteractive(bool interactive);
    void resetAnchor();
    /// 默认锚点：主显示器工作区右上角（面板尺寸按真实尺寸算，避免估算误差）
    QPoint defaultAnchor() const;

    QString summaryText(const Hud::HudSnapshot &snapshot) const;

    // ---------------- 采样状态（必须由本对象持有，见 SystemInfo.h）--------
    WinEase::Win32::CpuSampler m_cpu;
    WinEase::Win32::NetworkSampler m_network;
    WinEase::Win32::GpuSampler m_gpu;

    QTimer m_tickTimer;
    /// 面板由宿主托管，可能被宿主回收 → 用 QPointer 防悬垂
    QPointer<HudOverlay> m_overlay;

    Hud::MetricOptions m_options;
    int m_intervalMs = 1000;
    QPoint m_anchor;             ///< 面板左上角（物理像素）
    bool m_anchorKnown = false;  ///< 配置里有没有存过位置
    bool m_interactive = false;  ///< false = 穿透（默认），true = 可拖拽
    bool m_running = false;

    QString m_lastSummary;       ///< 状态文本去重（每秒刷新会不断变化，不必去重得太狠）

    // ---------------- 设置面板控件（用于从其它入口回填）----------------
    QPointer<QCheckBox> m_interactiveCheck;
    QPointer<QSpinBox> m_intervalSpin;
    QPointer<QLabel> m_detailLabel;
};

} // namespace WinEase::FeaturePlugins
