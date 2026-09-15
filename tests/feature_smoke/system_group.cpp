// ============================================================================
//  feature_smoke / system_group.cpp —— P1-D 系统组端到端用例
//
//  三条纪律（与前几组一致）：
//      ① 先读原值做前置（没有基准就没资格谈"还原"）；
//      ② 只经公开入口驱动（dispatchHotkey），断言**系统层面的真实状态**
//         （HKCU 注册表、剪贴板链接、主题 DWORD、当前壁纸路径）；
//      ③ 用例结束必须让系统回到原样，并**再读一遍验证**。
//
//  引号坑（P1-05，实测得来）：explorer 传 `%1` 时只有"路径含空格"才会加引号，
//      所以命令里自己写引号会导致含空格路径被复制成带引号的字符串。
//      正确写法是 cmd 的 `for %I in (%1) do @echo %~I`（`%~I` 会去掉引号）。
// ============================================================================

#include "system_group.h"

#include "app/core/PluginManager.h"
#include "win32/DesktopWallpaper.h"
#include "win32/RegistryUtils.h"
#include "win32/WindowUtils.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <QVariant>
#include <QWidget>

#include <windows.h>

namespace FeatureSmoke {

namespace {

const QString kMenuId = QStringLiteral("file.context_menu");
const QString kSearchId = QStringLiteral("launcher.web_search");
const QString kThemeId = QStringLiteral("personal.theme_switch");
const QString kWallpaperId = QStringLiteral("personal.wallpaper");

using WinEase::Win32::RegistryRoot;
using WinEase::Win32::RegistryKey;

// ---- 右键菜单：测试侧独立写一遍"应该装在哪"（不用插件的常量来测插件）----
const QStringList kMenuRoots = { QStringLiteral("*"),
                                 QStringLiteral("Directory"),
                                 QStringLiteral("Directory\\Background") };

QString menuKeyPath(const QString &root, const QString &key)
{
    return QStringLiteral("Software\\Classes\\%1\\shell\\%2").arg(root, key);
}

/// 各 root 下残留的 WinEase.* 子键（返回完整 HKCU 子键路径）
QStringList leftoverMenuKeys()
{
    QStringList leftovers;
    for (const QString &root : kMenuRoots) {
        const QString shellPath = QStringLiteral("Software\\Classes\\%1\\shell").arg(root);
        RegistryKey shell = RegistryKey::open(RegistryRoot::CurrentUser, shellPath, true);
        if (!shell.isValid()) {
            continue;
        }
        for (const QString &name : shell.subKeyNames()) {
            if (name.startsWith(QStringLiteral("WinEase."))) {
                leftovers.append(shellPath + QLatin1Char('\\') + name);
            }
        }
    }
    return leftovers;
}

// ---- 主题 ----
QString personalizePath()
{
    return QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize");
}

int themeValue(const QString &name)
{
    const QVariant value = WinEase::Win32::readValue(RegistryRoot::CurrentUser, personalizePath(), name);
    return value.isValid() ? value.toInt() : -1;
}

/// 抓 WM_SETTINGCHANGE 广播的隐藏窗口。
/// P1-13 的验收是"切换后立即生效"—— 而"立即"完全依赖广播：不广播就得重启资源管理器。
/// 所以这里不放任"值写进去了就算过"，而是真的抓一次广播来证明。
class SettingChangeSniffer : public QWidget
{
public:
    explicit SettingChangeSniffer(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setWindowTitle(QStringLiteral("WinEase 自检·广播嗅探"));
        resize(120, 60);
        // 不显示也要有真实的 HWND：WM_SETTINGCHANGE 是发给所有顶层窗口的
        // （包括未显示的），但没有 HWND 就收不到
        (void)winId();
    }

    int count() const { return m_count; }
    QStringList areas() const { return m_areas; }
    void reset()
    {
        m_count = 0;
        m_areas.clear();
    }

protected:
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override
    {
        Q_UNUSED(eventType);
        Q_UNUSED(result);
        auto *msg = static_cast<MSG *>(message);
        if (msg != nullptr && msg->message == WM_SETTINGCHANGE) {
            ++m_count;
            m_areas.append(msg->lParam != 0
                               ? QString::fromWCharArray(reinterpret_cast<const wchar_t *>(msg->lParam))
                               : QString());
        }
        return false; // 交给默认处理
    }

private:
    int m_count = 0;
    QStringList m_areas;
};

/// 路径比较前必须归一化（大小写 + 分隔符 + 相对段）：踩坑 #13
bool samePath(const QString &a, const QString &b)
{
    if (a.isEmpty() || b.isEmpty()) {
        return false;
    }
    return QDir::cleanPath(QDir::fromNativeSeparators(a)).toLower()
           == QDir::cleanPath(QDir::fromNativeSeparators(b)).toLower();
}

} // namespace

int runSystemGroupTests(Reporter &reporter,
                        WinEase::PluginManager &manager,
                        StubServices &services,
                        ProbeWindow &probe)
{
    Q_UNUSED(probe);
    const int failuresAtStart = reporter.failures();
    const ClipboardGuard clipboardGuard;

    // =======================================================================
    //  P1-05 右键菜单扩展
    // =======================================================================
    reporter.info(QStringLiteral("---- P1-05 右键菜单扩展 ----"));
    {
        WinEase::IFeaturePlugin *plugin = manager.plugin(kMenuId);
        reporter.check(plugin != nullptr,
                       QStringLiteral("P1-05 插件已从 plugins 目录加载（file.context_menu）"));
        if (plugin == nullptr) {
            return reporter.failures() - failuresAtStart;
        }

        // 前置：注册表里不应该有上次残留（真有的话删掉——这是我们的键，属于自检消毒）
        const QStringList leftovers = leftoverMenuKeys();
        reporter.check(leftovers.isEmpty(),
                       QStringLiteral("P1-05 前置：HKCU 里没有上次运行残留的 WinEase 菜单项"),
                       leftovers.join(QStringLiteral("；")));
        for (const QString &path : leftovers) {
            WinEase::Win32::deleteKeyRecursively(RegistryRoot::CurrentUser, path);
        }

        StatusLog status;
        status.attach(plugin);
        reporter.check(manager.setPluginEnabled(kMenuId, true),
                       QStringLiteral("P1-05 插件启用成功"));
        reporter.check(status.contains(QStringLiteral("已安装")),
                       QStringLiteral("P1-05 状态文本反馈安装结果"),
                       status.last());
        reporter.check(status.contains(QStringLiteral("显示更多选项")),
                       QStringLiteral("P1-05 状态文本提醒 Win11 会把菜单项折叠（避免被当成 bug）"),
                       status.last());

        // 期望装出来的 6 个键
        const QStringList expectedKeys = {
            menuKeyPath(kMenuRoots.at(0), QStringLiteral("WinEase.CopyPath")),
            menuKeyPath(kMenuRoots.at(1), QStringLiteral("WinEase.CopyPath")),
            menuKeyPath(kMenuRoots.at(2), QStringLiteral("WinEase.CopyPath")),
            menuKeyPath(kMenuRoots.at(0), QStringLiteral("WinEase.HashFile")),
            menuKeyPath(kMenuRoots.at(1), QStringLiteral("WinEase.OpenTerminal")),
            menuKeyPath(kMenuRoots.at(2), QStringLiteral("WinEase.OpenTerminal")),
        };
        reporter.check(leftoverMenuKeys().size() == expectedKeys.size(),
                       QStringLiteral("P1-05 安装了 6 项（复制路径×3 / 哈希×1 / 终端×2）"),
                       QStringLiteral("实际 %1 项：%2")
                           .arg(leftoverMenuKeys().size())
                           .arg(leftoverMenuKeys().join(QStringLiteral(", "))));

        const auto commandOf = [](const QString &path) {
            return WinEase::Win32::readValue(RegistryRoot::CurrentUser,
                                             path + QStringLiteral("\\command"), QString())
                .toString();
        };
        const QString copyPathCommand = commandOf(expectedKeys.at(0));
        reporter.check(copyPathCommand.contains(QStringLiteral("%~I"))
                           && copyPathCommand.contains(QStringLiteral("clip")),
                       QStringLiteral("P1-05 「复制完整路径」用的是 for %I + %~I（含空格路径不会带引号）"),
                       copyPathCommand);
        reporter.check(commandOf(expectedKeys.at(3)).contains(QStringLiteral("certutil")),
                       QStringLiteral("P1-05 「计算哈希」调用 certutil"),
                       commandOf(expectedKeys.at(3)));
        reporter.check(commandOf(expectedKeys.at(4)).contains(QStringLiteral("cd /d")),
                       QStringLiteral("P1-05 「在此处打开终端」用 cd /d 切到该目录"),
                       commandOf(expectedKeys.at(4)));

        const QString titleDefault = WinEase::Win32::readValue(RegistryRoot::CurrentUser,
                                                              expectedKeys.at(0), QString())
                                         .toString();
        const QString titleMui = WinEase::Win32::readValue(RegistryRoot::CurrentUser,
                                                           expectedKeys.at(0),
                                                           QStringLiteral("MUIVerb"))
                                     .toString();
        reporter.check(!titleDefault.isEmpty() && !titleMui.isEmpty(),
                       QStringLiteral("P1-05 菜单文字同时写默认值与 MUIVerb（新旧资源管理器都认）"),
                       QStringLiteral("默认值「%1」/ MUIVerb「%2」").arg(titleDefault, titleMui));

        reporter.check(!WinEase::Win32::keyExists(RegistryRoot::LocalMachine, expectedKeys.at(0)),
                       QStringLiteral("P1-05 只写 HKCU，没有碰 HKLM（整个功能不需要管理员权限）"));

        reporter.check(
            services.configValue(kMenuId, QStringLiteral("installed"), QVariant()).toStringList().size()
                == expectedKeys.size(),
            QStringLiteral("P1-05 写入清单落盘到配置（卸载时按它删除的依据）"),
            services.configValue(kMenuId, QStringLiteral("installed"), QVariant()).toStringList().join(
                QStringLiteral(", ")));

        // ---- 移除 / 重装 ----
        status.clear();
        reporter.check(dispatchAction(manager, kMenuId, QStringLiteral("uninstall")),
                       QStringLiteral("P1-05 触发「移除全部」"));
        reporter.check(leftoverMenuKeys().isEmpty(),
                       QStringLiteral("P1-05 移除后注册表里一个 WinEase 项都不剩"),
                       leftoverMenuKeys().join(QStringLiteral(", ")));

        reporter.check(dispatchAction(manager, kMenuId, QStringLiteral("default")),
                       QStringLiteral("P1-05 触发「重装」"));
        reporter.check(leftoverMenuKeys().size() == expectedKeys.size(),
                       QStringLiteral("P1-05 重装把 6 项装回来（改完配置不用重启）"),
                       QStringLiteral("实际 %1 项").arg(leftoverMenuKeys().size()));

        // ---- 停用 = 完整清理（本插件的验收指标）----
        reporter.check(manager.setPluginEnabled(kMenuId, false), QStringLiteral("P1-05 插件停用成功"));
        reporter.check(leftoverMenuKeys().isEmpty(),
                       QStringLiteral("P1-05 ★ 停用后注册表**完整清理**（验收：卸载功能时无残留）"),
                       leftoverMenuKeys().join(QStringLiteral(", ")));
    }

    // =======================================================================
    //  P1-09 网页快速搜索
    // =======================================================================
    reporter.info(QStringLiteral("---- P1-09 网页快速搜索 ----"));
    {
        WinEase::IFeaturePlugin *plugin = manager.plugin(kSearchId);
        reporter.check(plugin != nullptr,
                       QStringLiteral("P1-09 插件已从 plugins 目录加载（launcher.web_search）"));
        if (plugin == nullptr) {
            return reporter.failures() - failuresAtStart;
        }

        StatusLog status;
        status.attach(plugin);
        reporter.check(manager.setPluginEnabled(kSearchId, true), QStringLiteral("P1-09 插件启用成功"));
        status.clear();

        // 配置 dryRun=true：链接写剪贴板而不是弹浏览器（自检才能逐字符校验）
        clipboardGuard.set(QStringLiteral("hello world"));
        reporter.check(dispatchAction(manager, kSearchId, QStringLiteral("default")),
                       QStringLiteral("P1-09 触发搜索（默认引擎 bing）"));
        reporter.check(clipboardText() == QStringLiteral("https://www.bing.com/search?q=hello%20world"),
                       QStringLiteral("P1-09 拼出的搜索链接正确，空格编码成 %%20"),
                       clipboardText());

        clipboardGuard.set(QStringLiteral("gh: qt widgets"));
        dispatchAction(manager, kSearchId, QStringLiteral("default"));
        reporter.check(clipboardText() == QStringLiteral("https://github.com/search?q=qt%20widgets"),
                       QStringLiteral("P1-09 「gh:」前缀切到 GitHub，且前缀不会进搜索词"),
                       clipboardText());

        clipboardGuard.set(QStringLiteral("bd：中文 词"));
        dispatchAction(manager, kSearchId, QStringLiteral("default"));
        reporter.check(
            clipboardText()
                == QStringLiteral("https://www.baidu.com/s?wd=%E4%B8%AD%E6%96%87%20%E8%AF%8D"),
            QStringLiteral("P1-09 全角冒号「：」也认，中文按 UTF-8 做百分号编码"),
            clipboardText());

        clipboardGuard.set(QStringLiteral("rust"));
        dispatchAction(manager, kSearchId, QStringLiteral("dx"));
        reporter.check(clipboardText() == QStringLiteral("https://duckduckgo.com/?q=rust"),
                       QStringLiteral("P1-09 配置里的自定义引擎生效（dx → DuckDuckGo）"),
                       clipboardText());

        clipboardGuard.set(QStringLiteral("zzz: hello"));
        dispatchAction(manager, kSearchId, QStringLiteral("default"));
        reporter.check(clipboardText().contains(QStringLiteral("zzz%3A%20hello")),
                       QStringLiteral("P1-09 不认识的前缀当普通关键词搜（不把用户的话吃掉）"),
                       clipboardText());

        status.clear();
        clipboardGuard.set(QString());
        dispatchAction(manager, kSearchId, QStringLiteral("default"));
        reporter.check(status.contains(QStringLiteral("没有取到文本")),
                       QStringLiteral("P1-09 没取到文字时给出明确原因（不会打开一个空搜索页）"),
                       status.last());

        reporter.check(manager.setPluginEnabled(kSearchId, false),
                       QStringLiteral("P1-09 插件停用成功"));
    }

    // =======================================================================
    //  P1-13 系统主题切换
    // =======================================================================
    reporter.info(QStringLiteral("---- P1-13 系统主题切换 ----"));
    {
        WinEase::IFeaturePlugin *plugin = manager.plugin(kThemeId);
        reporter.check(plugin != nullptr,
                       QStringLiteral("P1-13 插件已从 plugins 目录加载（personal.theme_switch）"));
        if (plugin == nullptr) {
            return reporter.failures() - failuresAtStart;
        }

        // 前置：读到原值（还原的基准）
        const int originalApps = themeValue(QStringLiteral("AppsUseLightTheme"));
        const int originalSystem = themeValue(QStringLiteral("SystemUsesLightTheme"));
        reporter.check(originalApps >= 0 && originalSystem >= 0,
                       QStringLiteral("P1-13 前置：读到当前主题设置（准确保存后才能谈还原）"),
                       QStringLiteral("应用=%1 系统=%2").arg(originalApps).arg(originalSystem));

        SettingChangeSniffer sniffer;
        StatusLog status;
        status.attach(plugin);
        reporter.check(manager.setPluginEnabled(kThemeId, true), QStringLiteral("P1-13 插件启用成功"));
        reporter.check(status.contains(QStringLiteral("当前：")),
                       QStringLiteral("P1-13 启用时报告当前主题模式"),
                       status.last());

        sniffer.reset();
        status.clear();
        reporter.check(dispatchAction(manager, kThemeId, QStringLiteral("light")),
                       QStringLiteral("P1-13 触发「浅色」"));
        reporter.check(themeValue(QStringLiteral("AppsUseLightTheme")) == 1
                           && themeValue(QStringLiteral("SystemUsesLightTheme")) == 1,
                       QStringLiteral("P1-13 浅色 = 应用与系统**两处都写 1**（漏一处就成了自定义模式）"),
                       QStringLiteral("应用=%1 系统=%2")
                           .arg(themeValue(QStringLiteral("AppsUseLightTheme")))
                           .arg(themeValue(QStringLiteral("SystemUsesLightTheme"))));
        reporter.check(status.contains(QStringLiteral("浅色")),
                       QStringLiteral("P1-13 状态文本报告切换结果"),
                       status.last());
        reporter.check(sniffer.count() > 0
                           && sniffer.areas().contains(QStringLiteral("ImmersiveColorSet")),
                       QStringLiteral("P1-13 ★ 切换后广播 WM_SETTINGCHANGE(\"ImmersiveColorSet\")"
                                      "（验收：无需重启立即生效）"),
                       QStringLiteral("收到 %1 次广播：[%2]")
                           .arg(sniffer.count())
                           .arg(sniffer.areas().join(QStringLiteral(" | "))));

        dispatchAction(manager, kThemeId, QStringLiteral("dark"));
        reporter.check(themeValue(QStringLiteral("AppsUseLightTheme")) == 0
                           && themeValue(QStringLiteral("SystemUsesLightTheme")) == 0,
                       QStringLiteral("P1-13 深色 = 两处都写 0"),
                       QStringLiteral("应用=%1 系统=%2")
                           .arg(themeValue(QStringLiteral("AppsUseLightTheme")))
                           .arg(themeValue(QStringLiteral("SystemUsesLightTheme"))));

        dispatchAction(manager, kThemeId, QStringLiteral("custom"));
        reporter.check(themeValue(QStringLiteral("AppsUseLightTheme")) == 1
                           && themeValue(QStringLiteral("SystemUsesLightTheme")) == 0,
                       QStringLiteral("P1-13 自定义 = 应用浅色 + 系统深色（Win11 的自定义模式）"),
                       QStringLiteral("应用=%1 系统=%2")
                           .arg(themeValue(QStringLiteral("AppsUseLightTheme")))
                           .arg(themeValue(QStringLiteral("SystemUsesLightTheme"))));

        // ---- 停用 = 完整还原（本组的硬指标）----
        sniffer.reset();
        reporter.check(manager.setPluginEnabled(kThemeId, false), QStringLiteral("P1-13 插件停用成功"));
        reporter.check(themeValue(QStringLiteral("AppsUseLightTheme")) == originalApps
                           && themeValue(QStringLiteral("SystemUsesLightTheme")) == originalSystem,
                       QStringLiteral("P1-13 ★ 停用后主题**逐项还原**成启用前的值"),
                       QStringLiteral("期望 应用=%1 系统=%2 / 实际 应用=%3 系统=%4")
                           .arg(originalApps)
                           .arg(originalSystem)
                           .arg(themeValue(QStringLiteral("AppsUseLightTheme")))
                           .arg(themeValue(QStringLiteral("SystemUsesLightTheme"))));
        reporter.check(sniffer.count() > 0,
                       QStringLiteral("P1-13 还原时同样广播了变更（否则任务栏不会立刻变回去）"),
                       QStringLiteral("收到 %1 次广播").arg(sniffer.count()));
    }

    // =======================================================================
    //  P1-14 壁纸自动切换
    // =======================================================================
    reporter.info(QStringLiteral("---- P1-14 壁纸自动切换 ----"));
    {
        WinEase::IFeaturePlugin *plugin = manager.plugin(kWallpaperId);
        reporter.check(plugin != nullptr,
                       QStringLiteral("P1-14 插件已从 plugins 目录加载（personal.wallpaper）"));
        if (plugin == nullptr) {
            return reporter.failures() - failuresAtStart;
        }

        const QString originalWallpaper = WinEase::Win32::currentWallpaper();
        reporter.check(!originalWallpaper.isEmpty(),
                       QStringLiteral("P1-14 前置：读到了当前壁纸路径（这是「还原」的基准）"),
                       QStringLiteral("当前壁纸：%1").arg(originalWallpaper));

        const QString folder =
            services.configValue(kWallpaperId, QStringLiteral("folder"), QVariant()).toString();
        const QStringList files =
            QDir(folder).entryList({ QStringLiteral("*.bmp") }, QDir::Files, QDir::Name);
        reporter.check(files.size() == 3,
                       QStringLiteral("P1-14 前置：素材目录里有 3 张测试壁纸"),
                       QStringLiteral("%1 → %2").arg(folder, files.join(QStringLiteral(", "))));
        if (files.size() != 3 || originalWallpaper.isEmpty()) {
            return reporter.failures() - failuresAtStart;
        }
        const QStringList fullPaths = { QDir(folder).absoluteFilePath(files.at(0)),
                                        QDir(folder).absoluteFilePath(files.at(1)),
                                        QDir(folder).absoluteFilePath(files.at(2)) };

        StatusLog status;
        status.attach(plugin);
        reporter.check(manager.setPluginEnabled(kWallpaperId, true),
                       QStringLiteral("P1-14 插件启用成功"));
        reporter.check(status.contains(QStringLiteral("3 张图")),
                       QStringLiteral("P1-14 启用时报告素材数量"),
                       status.last());
        reporter.check(samePath(services.configValue(kWallpaperId,
                                                     QStringLiteral("originalWallpaper"), QVariant())
                                    .toString(),
                                originalWallpaper),
                       QStringLiteral("P1-14 启用时把原壁纸记进配置（停用时还原的依据）"),
                       services.configValue(kWallpaperId, QStringLiteral("originalWallpaper"), QVariant())
                           .toString());

        // ---- 按序轮换：三张 + 回绕 ----
        const auto currentIs = [](const QString &expected) {
            return samePath(WinEase::Win32::currentWallpaper(), expected);
        };
        const auto waitForWallpaper = [&currentIs](const QString &expected) {
            return waitFor([&currentIs, &expected] { return currentIs(expected); }, 3000);
        };

        reporter.check(dispatchAction(manager, kWallpaperId, QStringLiteral("default")),
                       QStringLiteral("P1-14 触发「下一张」"));
        reporter.check(waitForWallpaper(fullPaths.at(0)),
                       QStringLiteral("P1-14 第 1 张：按文件名顺序切到 1_winease.bmp"),
                       QStringLiteral("当前壁纸：%1（期望 %2）")
                           .arg(WinEase::Win32::currentWallpaper(), fullPaths.at(0)));

        dispatchAction(manager, kWallpaperId, QStringLiteral("default"));
        reporter.check(waitForWallpaper(fullPaths.at(1)),
                       QStringLiteral("P1-14 第 2 张：按序切到 2_winease.bmp"),
                       WinEase::Win32::currentWallpaper());

        dispatchAction(manager, kWallpaperId, QStringLiteral("default"));
        reporter.check(waitForWallpaper(fullPaths.at(2)),
                       QStringLiteral("P1-14 第 3 张：按序切到 3_winease.bmp"),
                       WinEase::Win32::currentWallpaper());

        dispatchAction(manager, kWallpaperId, QStringLiteral("default"));
        reporter.check(waitForWallpaper(fullPaths.at(0)),
                       QStringLiteral("P1-14 到最后一张后绕回第 1 张（不会卡死）"),
                       WinEase::Win32::currentWallpaper());

        dispatchAction(manager, kWallpaperId, QStringLiteral("prev"));
        reporter.check(waitForWallpaper(fullPaths.at(2)),
                       QStringLiteral("P1-14 「上一张」按反向回滚（历史回滚）"),
                       WinEase::Win32::currentWallpaper());

        reporter.check(services.configValue(kWallpaperId, QStringLiteral("history"), QVariant())
                               .toStringList()
                               .size()
                           >= 4,
                       QStringLiteral("P1-14 轮换历史写回配置（便于回滚与排查）"),
                       QStringLiteral("%1 条")
                           .arg(services.configValue(kWallpaperId, QStringLiteral("history"),
                                                    QVariant())
                                    .toStringList()
                                    .size()));

        reporter.check(dispatchAction(manager, kWallpaperId, QStringLiteral("restore")),
                       QStringLiteral("P1-14 触发「恢复原壁纸」"));
        reporter.check(waitForWallpaper(originalWallpaper),
                       QStringLiteral("P1-14 「恢复原壁纸」把桌面换回启用前那张"),
                       WinEase::Win32::currentWallpaper());

        // ---- 停用 = 还原（本组的硬指标）----
        // 注意游标：上一步「上一张」把游标退到了最后一张，所以这一次「下一张」会**绕回第 1 张**
        status.clear();
        dispatchAction(manager, kWallpaperId, QStringLiteral("default"));
        reporter.check(waitForWallpaper(fullPaths.at(0)),
                       QStringLiteral("P1-14 停用前再换一张（用于验证停用确实会还原）"),
                       QStringLiteral("当前 %1 / 期望 %2 / 目录里 %3 张 / 状态「%4」")
                           .arg(WinEase::Win32::currentWallpaper(), fullPaths.at(0))
                           .arg(QDir(folder)
                                    .entryList({ QStringLiteral("*.bmp") }, QDir::Files)
                                    .size())
                           .arg(status.last()));

        reporter.check(manager.setPluginEnabled(kWallpaperId, false),
                       QStringLiteral("P1-14 插件停用成功"));
        reporter.check(waitForWallpaper(originalWallpaper),
                       QStringLiteral("P1-14 ★ 停用后壁纸**还原**成启用前那张"),
                       QStringLiteral("当前 %1 / 期望 %2")
                           .arg(WinEase::Win32::currentWallpaper(), originalWallpaper));
    }

    reporter.info(QStringLiteral("系统组失败项：%1").arg(reporter.failures() - failuresAtStart));
    return reporter.failures() - failuresAtStart;
}

} // namespace FeatureSmoke
