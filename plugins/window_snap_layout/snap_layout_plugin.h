#pragma once

// ============================================================================
//  SnapLayoutPlugin —— P1-02 窗口快速分屏
//
//  行为：
//    * 把**目标窗口**摆到预设区域：左半 / 右半 / 上半 / 下半 / 四象限 / 居中 / 左右三分之一
//    * 区域一律基于**窗口所在显示器的工作区**（workArea，已排除任务栏），
//      多屏下窗口不会跑到别的屏幕上
//    * 相邻区域按"剩余像素"切分（右半用 width - width/2），
//      保证奇数宽度下两个窗口精确铺满、既不重叠也不留缝
//    * 落到 moveWindow 之前会用 windowRectForVisualRect() 扣掉不可见投影边框，
//      否则实际显示出来会有约 7px 的缝隙（分屏功能最常见的观感缺陷）
//
//  不留副作用：移动窗口属于"用户随时可再改"的操作，
//  因此停用功能时**不做**任何还原（这是有意的，见 ROADMAP 裁剪说明）。
// ============================================================================

#include "sdk/IFeaturePlugin.h"

#include "WindowFeatureState.h"

class SnapLayoutPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "snap_layout_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    /// 预设区域
    enum class Zone {
        Left,
        Right,
        Top,
        Bottom,
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight,
        Center,
        LeftThird,
        RightThird
    };

    explicit SnapLayoutPlugin(QObject *parent = nullptr);

    // ---------------- 元信息 ----------------
    QString id() const override;
    QString name() const override;
    QString description() const override;
    QIcon icon() const override;
    WinEase::FeatureCategory category() const override;
    QStringList tags() const override;

    // ---------------- 能力标记 ----------------
    bool supportsHotkey() const override;
    QKeySequence defaultHotkey() const override;

    // ---------------- 生命周期 ----------------
    bool initialize() override;
    void shutdown() override;
    QWidget *createSettingsWidget(QWidget *parent = nullptr) override;

protected:
    bool onEnable() override;
    void onDisable() override;
    void onHotkey(const QString &hotkeyId) override;

    /// 把目标窗口摆到指定区域（返回值仅用于自检/日志，界面行为通过状态文本体现）
    bool snapTo(Zone zone);

private:
    void registerZoneHotkeys();

    WinEase::FeaturePlugins::WindowTargetState m_target;
};
