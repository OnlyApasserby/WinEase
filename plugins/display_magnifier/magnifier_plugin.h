#pragma once

// ============================================================================
//  magnifier_plugin.h —— P2-09 放大镜增强（display.magnifier）
//
//  放大本身由平台层 `WinEase::Win32::Magnifier`（Windows Magnification API）完成，
//  本插件只管**策略**：什么时候开、开多大、什么形状、跟着光标走。
//
//  三条策略上的取舍：
//   1. **启用功能 ≠ 立刻弹出一个放大窗**。启用只是"待命"（用户可能正在设置面板里
//      点来点去，突然冒出一块放大区域很讨厌）；真正显示由快捷键或面板按钮触发。
//      于是"功能启用"与"放大中"是两个独立状态，面板按钮/状态栏如实反映后者。
//   2. **跟随用定时器 30ms（≈33fps）**：每个周期只有 3 次轻调用（SetWindowPos /
//      MagSetWindowSource / InvalidateRect），放大画面的合成在系统侧完成，
//      所以跟随几乎不吃 CPU（自检里有"自抓屏参考实现"当场对照）。
//   3. **靠边翻转**：放大窗默认放在光标右下方，右下放不下就翻到左上，
//      并且夹在显示器**可用区**内（不盖任务栏）。放大镜是"看"的工具，
//      它挡住自己要放大的那块内容就本末倒置了。
// ============================================================================

#include "sdk/IFeaturePlugin.h"
#include "win32/Magnifier.h"

#include <QPointer>
#include <QString>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTimer;
class QWidget;

class MagnifierPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "magnifier_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit MagnifierPlugin(QObject *parent = nullptr);

    // ---------------- 元信息 ----------------
    QString id() const override;
    QString name() const override;
    QString description() const override;
    QIcon icon() const override;
    WinEase::FeatureCategory category() const override;
    QStringList tags() const override;

    bool supportsHotkey() const override;
    QKeySequence defaultHotkey() const override;

    // ---------------- 生命周期 ----------------
    bool initialize() override;
    void shutdown() override;
    QWidget *createSettingsWidget(QWidget *parent = nullptr) override;

protected:
    bool canEnable(QString *reason) const override;
    bool onEnable() override;
    void onDisable() override;
    void onHotkey(const QString &hotkeyId) override;

private:
    /// 开始/停止放大（两者都幂等）
    bool startMagnifier();
    void stopMagnifier(const QString &reason);
    /// 快捷键与面板按钮共用的切换
    void toggleMagnifier();

    /// 跟随定时器回调：窗口位置 + 放大源区域
    void followTick();

    /// 应用一项设置到"正在运行的放大镜"（没在跑就只存值）
    void applyFactor(double factor, bool persist);
    void applySize(int sizePx, bool persist);
    void applyShape(WinEase::Win32::MagnifierShape shape, bool persist);

    void refreshPanel();
    /// 面板状态文本：倍率 / 源区尺寸 / 遮罩 / 当前是否跟随中
    QString statusText() const;

    WinEase::Win32::Magnifier m_magnifier;
    QTimer *m_followTimer = nullptr;

    double m_factor = 2.5;
    int m_sizePx = 240;
    WinEase::Win32::MagnifierShape m_shape = WinEase::Win32::MagnifierShape::Rounded;

    /// 连续跟随失败次数：达到阈值就如实停下（不静默重试到天荒地老）
    int m_followFailures = 0;

    QPointer<QWidget> m_panel;
    QPointer<QLabel> m_statusLabel;
    QPointer<QPushButton> m_toggleButton;
    QPointer<QDoubleSpinBox> m_factorSpin;
    QPointer<QSpinBox> m_sizeSpin;
    QPointer<QComboBox> m_shapeCombo;
};
