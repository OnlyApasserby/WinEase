#pragma once

// ============================================================================
//  TemplatePlugin —— 功能插件模板
//
//  这是编写 WinEase 功能插件时必须遵循的最小骨架，注意四个关键点：
//    1. 只继承 WinEase::IFeaturePlugin
//       （IFeaturePlugin 本身已继承 QObject，再继承一次会产生二义基类，
//        导致 connect() 调用不明确、moc 生成的 qt_metacast 转换失败）
//    2. Q_OBJECT 宏（AUTOMOC 需要）
//    3. Q_PLUGIN_METADATA（IID 必须与 sdk/WinEaseVersion.h 中的宏一致）
//    4. Q_INTERFACES（否则主程序无法用 qobject_cast 识别本插件）
// ============================================================================

#include "sdk/IFeaturePlugin.h"
#include "sdk/PluginServices.h"

class QWidget;

class TemplatePlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "template_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit TemplatePlugin(QObject *parent = nullptr);
    ~TemplatePlugin() override;

    // ---------------- 元信息 ----------------
    QString id() const override;
    QString name() const override;
    QString description() const override;
    QIcon icon() const override;
    WinEase::FeatureCategory category() const override;
    QStringList tags() const override;

    // ---------------- 能力标记 ----------------
    bool requiresAdmin() const override;
    bool supportsHotkey() const override;
    QKeySequence defaultHotkey() const override;

    // ---------------- 生命周期 ----------------
    bool initialize() override;
    void shutdown() override;
    QWidget *createSettingsWidget(QWidget *parent = nullptr) override;

protected:
    bool onEnable() override;
    void onDisable() override;
    bool canEnable(QString *reason) const override;
    void onHotkey(const QString &hotkeyId) override;

private:
    /// 从配置中读取的示例参数
    bool m_demoOption = false;
};
