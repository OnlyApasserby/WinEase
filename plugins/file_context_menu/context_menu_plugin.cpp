#include "context_menu_plugin.h"

#include "sdk/PluginServices.h"
#include "win32/RegistryUtils.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

using WinEase::Win32::RegistryKey;
using WinEase::Win32::RegistryRoot;
using WinEase::Win32::RegistryValueType;
using WinEase::Win32::RegistryView;

namespace {

/// 三个 root：所有文件 / 文件夹 / 文件夹空白处
const QString kRootFile = QStringLiteral("*");
const QString kRootDirectory = QStringLiteral("Directory");
const QString kRootBackground = QStringLiteral("Directory\\Background");

/// 复制完整路径：`for %I in (%1) do @echo %~I` 里的 `%~I` 会去掉 explorer
/// 在"路径含空格"时加上的引号 —— 直接 `echo %1|clip` 会把引号一起复制进去（实测）
const QString kCopyPathFileCommand =
    QStringLiteral("cmd.exe /c for %I in (%1) do @echo %~I|clip");
const QString kCopyPathBackgroundCommand =
    QStringLiteral("cmd.exe /c for %I in (%V) do @echo %~I|clip");

} // namespace

QString ContextMenuPlugin::Entry::subKeyPath() const
{
    return QStringLiteral("Software\\Classes\\%1\\shell\\%2").arg(root, key);
}

ContextMenuPlugin::ContextMenuPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString ContextMenuPlugin::id() const
{
    return QStringLiteral("file.context_menu");
}

QString ContextMenuPlugin::name() const
{
    return QStringLiteral("右键菜单扩展");
}

QString ContextMenuPlugin::description() const
{
    return QStringLiteral("给资源管理器右键菜单加几项：复制完整路径、算哈希、在此处打开终端");
}

QIcon ContextMenuPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::FileEnhancement);
}

WinEase::FeatureCategory ContextMenuPlugin::category() const
{
    return WinEase::FeatureCategory::FileEnhancement;
}

QStringList ContextMenuPlugin::tags() const
{
    return { QStringLiteral("右键"), QStringLiteral("菜单"), QStringLiteral("context"),
             QStringLiteral("menu"), QStringLiteral("哈希"), QStringLiteral("路径") };
}

bool ContextMenuPlugin::supportsHotkey() const
{
    return true;
}

QKeySequence ContextMenuPlugin::defaultHotkey() const
{
    return QKeySequence(QStringLiteral("Ctrl+Alt+M"));
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool ContextMenuPlugin::initialize()
{
    if (WinEase::PluginServices *svc = services()) {
        m_copyPath = svc->configValue(id(), QStringLiteral("copyPath"), true).toBool();
        m_hashFile = svc->configValue(id(), QStringLiteral("hashFile"), true).toBool();
        m_openTerminal = svc->configValue(id(), QStringLiteral("openTerminal"), true).toBool();
        loadInstalled();
    }
    return true;
}

void ContextMenuPlugin::shutdown()
{
    // 退出即清理：菜单项只在 WinEase 运行期间存在。
    // 这样"删掉 WinEase 目录"不会在用户注册表里留下指向空路径的菜单项。
    m_installedFlag = false;
    removeEntries();
}

bool ContextMenuPlugin::onEnable()
{
    if (services() == nullptr) {
        setLastError(QStringLiteral("宿主服务未注入，插件无法启用"));
        return false;
    }

    QStringList errors;
    const int installed = installEntries(&errors);
    m_installedFlag = installed > 0;
    if (installed == 0) {
        setLastError(errors.isEmpty() ? QStringLiteral("没有可安装的菜单项（都被关掉了？）")
                                      : errors.join(QStringLiteral("；")));
        logMessage(WinEase::PluginLogLevel::Warning, lastError());
        Q_EMIT statusMessage(QStringLiteral("安装菜单项失败：%1").arg(lastError()));
        return false;
    }
    if (!errors.isEmpty()) {
        logMessage(WinEase::PluginLogLevel::Warning, errors.join(QStringLiteral("；")));
    }

    Q_EMIT statusMessage(QStringLiteral("已安装 %1 项右键菜单命令%2（Win11 需在「显示更多选项」里找）")
                             .arg(installed)
                             .arg(errors.isEmpty() ? QString()
                                                   : QStringLiteral("，%1 项失败").arg(errors.size())));
    return true;
}

void ContextMenuPlugin::onDisable()
{
    const int removed = removeEntries();
    m_installedFlag = false;
    Q_EMIT statusMessage(QStringLiteral("已移除 %1 项右键菜单命令（注册表无残留）").arg(removed));
}

void ContextMenuPlugin::onHotkey(const QString &hotkeyId)
{
    if (!isEnabled()) {
        return;
    }
    const QString action = hotkeyId.section(QStringLiteral("::"), 1, 1);

    if (action == QLatin1String("uninstall")) {
        const int removed = removeEntries();
        m_installedFlag = false;
        Q_EMIT statusMessage(QStringLiteral("已移除 %1 项右键菜单命令").arg(removed));
        return;
    }

    // "default"：重装（先清后装 —— 用户改了配置想让它立刻生效时最顺手）
    removeEntries();
    QStringList errors;
    const int installed = installEntries(&errors);
    m_installedFlag = installed > 0;
    Q_EMIT statusMessage(QStringLiteral("已重装 %1 项右键菜单命令%2")
                             .arg(installed)
                             .arg(errors.isEmpty() ? QString()
                                                   : QStringLiteral("，%1 项失败").arg(errors.size())));
}

// ---------------------------------------------------------------------------
//  菜单项
// ---------------------------------------------------------------------------

QList<ContextMenuPlugin::Entry> ContextMenuPlugin::buildEntries() const
{
    QList<Entry> entries;

    if (m_copyPath) {
        entries.append({ kRootFile, QStringLiteral("WinEase.CopyPath"),
                         QStringLiteral("复制完整路径"), kCopyPathFileCommand });
        entries.append({ kRootDirectory, QStringLiteral("WinEase.CopyPath"),
                         QStringLiteral("复制完整路径"), kCopyPathFileCommand });
        entries.append({ kRootBackground, QStringLiteral("WinEase.CopyPath"),
                         QStringLiteral("复制完整路径"), kCopyPathBackgroundCommand });
    }

    if (m_hashFile) {
        entries.append({ kRootFile, QStringLiteral("WinEase.HashFile"),
                         QStringLiteral("计算 SHA-256 哈希"),
                         QStringLiteral("cmd.exe /k certutil -hashfile %1 SHA256") });
    }

    if (m_openTerminal) {
        entries.append({ kRootDirectory, QStringLiteral("WinEase.OpenTerminal"),
                         QStringLiteral("在此处打开终端"),
                         QStringLiteral("cmd.exe /k cd /d %1") });
        entries.append({ kRootBackground, QStringLiteral("WinEase.OpenTerminal"),
                         QStringLiteral("在此处打开终端"),
                         QStringLiteral("cmd.exe /k cd /d %V") });
    }

    return entries;
}

int ContextMenuPlugin::installEntries(QStringList *errors)
{
    int installed = 0;
    for (const Entry &entry : buildEntries()) {
        const QString path = entry.subKeyPath();
        QString error;

        RegistryKey key = RegistryKey::create(RegistryRoot::CurrentUser, path,
                                              RegistryView::Default, &error);
        if (!key.isValid()) {
            if (errors != nullptr) {
                *errors << QStringLiteral("%1：%2").arg(path, error);
            }
            continue;
        }
        // 默认值给老版本的资源管理器用；MUIVerb 是新版（含 Win11）优先读的
        key.setValue(QString(), entry.title, RegistryValueType::String);
        key.setValue(QStringLiteral("MUIVerb"), entry.title, RegistryValueType::String);

        RegistryKey command = RegistryKey::create(RegistryRoot::CurrentUser,
                                                  path + QStringLiteral("\\command"),
                                                  RegistryView::Default, &error);
        if (!command.isValid()) {
            if (errors != nullptr) {
                *errors << QStringLiteral("%1\\command：%2").arg(path, error);
            }
            continue;
        }
        command.setValue(QString(), entry.command, RegistryValueType::String);

        if (!m_installedKeys.contains(path)) {
            m_installedKeys.append(path);
        }
        ++installed;
    }

    persistInstalled();
    return installed;
}

int ContextMenuPlugin::removeEntries()
{
    // ① 配置里记录的键
    QStringList targets = m_installedKeys;

    // ② 兜底：把各 root 下所有 WinEase.* 子键都算进来（上次异常退出可能留下残渣）
    const QStringList roots = { kRootFile, kRootDirectory, kRootBackground };
    for (const QString &root : roots) {
        const QString shellPath = QStringLiteral("Software\\Classes\\%1\\shell").arg(root);
        RegistryKey shell = RegistryKey::open(RegistryRoot::CurrentUser, shellPath, true);
        if (!shell.isValid()) {
            continue;
        }
        for (const QString &name : shell.subKeyNames()) {
            if (name.startsWith(QStringLiteral("WinEase."))) {
                targets.append(shellPath + QLatin1Char('\\') + name);
            }
        }
    }
    targets.removeDuplicates();

    int removed = 0;
    for (const QString &path : targets) {
        if (!WinEase::Win32::keyExists(RegistryRoot::CurrentUser, path)) {
            continue;
        }
        if (WinEase::Win32::deleteKeyRecursively(RegistryRoot::CurrentUser, path)) {
            ++removed;
        }
    }

    m_installedKeys.clear();
    persistInstalled();
    return removed;
}

void ContextMenuPlugin::persistInstalled()
{
    if (WinEase::PluginServices *svc = services()) {
        svc->setConfigValue(id(), QStringLiteral("installed"), m_installedKeys);
        svc->setConfigValue(id(), QStringLiteral("installedFlag"), m_installedFlag);
    }
}

void ContextMenuPlugin::loadInstalled()
{
    if (WinEase::PluginServices *svc = services()) {
        m_installedKeys = svc->configValue(id(), QStringLiteral("installed")).toStringList();
        m_installedFlag = svc->configValue(id(), QStringLiteral("installedFlag"), false).toBool();
    }
}

// ---------------------------------------------------------------------------
//  设置面板
// ---------------------------------------------------------------------------

QWidget *ContextMenuPlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QVBoxLayout(widget);

    auto *copyBox = new QCheckBox(QStringLiteral("复制完整路径（文件 / 文件夹 / 空白处）"), widget);
    copyBox->setChecked(m_copyPath);
    layout->addWidget(copyBox);
    auto *hashBox = new QCheckBox(QStringLiteral("计算 SHA-256 哈希（文件）"), widget);
    hashBox->setChecked(m_hashFile);
    layout->addWidget(hashBox);
    auto *terminalBox = new QCheckBox(QStringLiteral("在此处打开终端（文件夹 / 空白处）"), widget);
    terminalBox->setChecked(m_openTerminal);
    layout->addWidget(terminalBox);

    auto *buttonRow = new QHBoxLayout();
    auto *installButton = new QPushButton(QStringLiteral("重新安装"), widget);
    auto *removeButton = new QPushButton(QStringLiteral("全部移除"), widget);
    buttonRow->addWidget(installButton);
    buttonRow->addWidget(removeButton);
    buttonRow->addStretch(1);
    layout->addLayout(buttonRow);

    auto *listLabel = new QLabel(widget);
    listLabel->setWordWrap(true);
    layout->addWidget(listLabel);

    auto *hint = new QLabel(QStringLiteral("写入位置：HKCU\\Software\\Classes\\…（**只写 HKCU，不需要管理员权限**）\n"
                                          "· Windows 11 会把这类项目折叠进「显示更多选项」（Shift+F10）\n"
                                          "· 菜单项只在 WinEase 运行且本功能启用时存在；停用/退出即清理，注册表无残留\n"
                                          "· 修改上面的勾选后点「重新安装」立刻生效")
                                .arg(QString()),
                            widget);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    const auto refreshList = [this, listLabel] {
        if (m_installedKeys.isEmpty()) {
            listLabel->setText(QStringLiteral("当前没有安装任何菜单项。"));
            return;
        }
        listLabel->setText(QStringLiteral("已安装 %1 项：\n· %2")
                               .arg(m_installedKeys.size())
                               .arg(m_installedKeys.join(QStringLiteral("\n· "))));
    };
    refreshList();

    const auto persistFlags = [this] {
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("copyPath"), m_copyPath);
            svc->setConfigValue(id(), QStringLiteral("hashFile"), m_hashFile);
            svc->setConfigValue(id(), QStringLiteral("openTerminal"), m_openTerminal);
        }
    };

    QObject::connect(copyBox, &QCheckBox::toggled, widget, [this, persistFlags](bool checked) {
        m_copyPath = checked;
        persistFlags();
    });
    QObject::connect(hashBox, &QCheckBox::toggled, widget, [this, persistFlags](bool checked) {
        m_hashFile = checked;
        persistFlags();
    });
    QObject::connect(terminalBox, &QCheckBox::toggled, widget, [this, persistFlags](bool checked) {
        m_openTerminal = checked;
        persistFlags();
    });
    QObject::connect(installButton, &QPushButton::clicked, widget,
                     [this, refreshList] {
                         removeEntries();
                         QStringList errors;
                         const int installed = installEntries(&errors);
                         m_installedFlag = installed > 0;
                         Q_EMIT statusMessage(QStringLiteral("已安装 %1 项右键菜单命令%2")
                                                  .arg(installed)
                                                  .arg(errors.isEmpty()
                                                           ? QString()
                                                           : QStringLiteral("，%1 项失败").arg(errors.size())));
                         refreshList();
                     });
    QObject::connect(removeButton, &QPushButton::clicked, widget, [this, refreshList] {
        const int removed = removeEntries();
        m_installedFlag = false;
        Q_EMIT statusMessage(QStringLiteral("已移除 %1 项右键菜单命令").arg(removed));
        refreshList();
    });

    return widget;
}
