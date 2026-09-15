#pragma once

// ============================================================================
//  ColorPickerPlugin —— P1-06 屏幕取色器
//
//  行为：
//    * 快捷键（默认 Ctrl+Alt+C）取**光标处**的屏幕像素颜色，按配置格式
//      （HEX / RGB / HSL / CMYK）写入剪贴板，并在卡片上给出状态文本
//    * 辅助快捷键 Ctrl+Alt+Shift+C：无论默认格式是什么，都复制 HEX
//    * 取色历史（最近 12 个，去重）持久化，设置面板里可点击回填
//
//  为什么必须按**物理像素**取色：
//      GetCursorPos / BitBlt 都是物理像素坐标系；本机是 150% 缩放，
//      若把 Qt 的逻辑坐标当物理坐标用，取到的颜色会整体偏移约 1/3 屏。
//      平台层 colorAt() 收的就是物理像素，插件侧不做任何换算。
// ============================================================================

#include "sdk/IFeaturePlugin.h"

#include "ColorFormats.h"

#include <QStringList>

class ColorPickerPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "color_picker_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit ColorPickerPlugin(QObject *parent = nullptr);

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

private:
    /// 取光标处颜色并按 format 格式化后复制；label 用于状态文本
    bool pickAndCopy(WinEase::FeaturePlugins::ColorFormats::Format format, const QString &label);
    /// 记入历史（去重、最多 12 条）并落盘
    void rememberColor(const QString &hex);
    void persist();

    WinEase::FeaturePlugins::ColorFormats::Format m_format =
        WinEase::FeaturePlugins::ColorFormats::Format::Hex;
    QStringList m_history;
    bool m_includePosition = false; ///< 复制时是否附带 "@ x,y"
};
