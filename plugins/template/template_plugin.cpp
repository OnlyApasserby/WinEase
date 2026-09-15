#include "template_plugin.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>

TemplatePlugin::TemplatePlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

TemplatePlugin::~TemplatePlugin() = default;

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString TemplatePlugin::id() const
{
    // 全局唯一，发布后不可更改：它同时是配置 section 与快捷键前缀
    return QStringLiteral("dev.template");
}

QString TemplatePlugin::name() const
{
    return QStringLiteral("插件模板");
}

QString TemplatePlugin::description() const
{
    return QStringLiteral("用于演示插件契约的示例功能，可安全删除");
}

QIcon TemplatePlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::Development);
}

WinEase::FeatureCategory TemplatePlugin::category() const
{
    return WinEase::FeatureCategory::Development;
}

QStringList TemplatePlugin::tags() const
{
    // 补充搜索关键词（别名 / 拼音首字母），主界面搜索框会一并匹配
    return { QStringLiteral("模板"), QStringLiteral("示例"), QStringLiteral("mb") };
}

// ---------------------------------------------------------------------------
//  能力标记
// ---------------------------------------------------------------------------

bool TemplatePlugin::requiresAdmin() const
{
    return false;
}

bool TemplatePlugin::supportsHotkey() const
{
    return true;
}

QKeySequence TemplatePlugin::defaultHotkey() const
{
    return QKeySequence(QStringLiteral("Ctrl+Alt+T"));
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool TemplatePlugin::initialize()
{
    // 通过宿主服务读取本插件专属配置（落在 [Plugins/dev.template] 段）
    if (WinEase::PluginServices *svc = services()) {
        m_demoOption = svc->configValue(id(), QStringLiteral("demoOption"), false).toBool();
        svc->log(id(), WinEase::PluginLogLevel::Info, QStringLiteral("模板插件初始化完成"));
    }
    return true;
}

void TemplatePlugin::shutdown()
{
    // 退出时把界面上的选择写回配置
    if (WinEase::PluginServices *svc = services()) {
        svc->setConfigValue(id(), QStringLiteral("demoOption"), m_demoOption);
        svc->syncConfig();
    }
}

QWidget *TemplatePlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QVBoxLayout(widget);

    auto *form = new QFormLayout();
    auto *demoCheck = new QCheckBox(QStringLiteral("启用示例选项"), widget);
    demoCheck->setChecked(m_demoOption);
    form->addRow(QStringLiteral("示例项："), demoCheck);
    layout->addLayout(form);

    layout->addWidget(new QLabel(QStringLiteral("提示：设置项的读写通过 PluginServices 完成，"
                                                "插件无需关心配置文件位置。"),
                                 widget));
    layout->addStretch(1);

    connect(demoCheck, &QCheckBox::toggled, widget, [this](bool checked) {
        m_demoOption = checked;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("demoOption"), checked);
        }
    });

    return widget;
}

// ---------------------------------------------------------------------------
//  启用 / 停用
// ---------------------------------------------------------------------------

bool TemplatePlugin::canEnable(QString *reason) const
{
    // 可在此校验运行环境；返回 false 时主程序会回滚开关并提示
    Q_UNUSED(reason)
    return true;
}

bool TemplatePlugin::onEnable()
{
    if (WinEase::PluginServices *svc = services()) {
        svc->notify(name(), QStringLiteral("模板插件已启用"));
    }
    return true;
}

void TemplatePlugin::onDisable()
{
    if (WinEase::PluginServices *svc = services()) {
        svc->log(id(), WinEase::PluginLogLevel::Info, QStringLiteral("模板插件已停用"));
    }
}

void TemplatePlugin::onHotkey(const QString &hotkeyId)
{
    // 默认实现会转发到 hotkeyTriggered 信号；此处可处理具体动作
    WinEase::IFeaturePlugin::onHotkey(hotkeyId);

    if (WinEase::PluginServices *svc = services()) {
        svc->notify(name(), QStringLiteral("快捷键已触发：%1").arg(hotkeyId));
    }
}
