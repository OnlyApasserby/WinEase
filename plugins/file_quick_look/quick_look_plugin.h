#pragma once

// ============================================================================
//  QuickLookPlugin —— P2-02 快速文件预览
//
//  行为：
//    * 空格键（**仅当前台窗口是资源管理器文件窗口时**）：预览剪贴板里的文件；
//      预览窗口已打开时，再按空格 = 关闭（与"按空格看一眼、再按一下收起来"的习惯一致）
//    * 快捷键 `default`（Ctrl+Alt+Space）：不依赖输入钩子的入口（自检也走它）
//    * 预览窗口：← / → 在同目录内换文件，Esc / 空格 关闭，点顶部信息条可拖动
//
//  ★ 取键为什么用 Direct（可吞事件）：
//      Explorer 自己也会响应空格（切换选中项）。不吞掉的话，用户"看一眼"的同时
//     选中项也跟着变了 —— 这是 P2-04/P2-05 之外唯一需要吞键的功能。
//    代价是回调必须极快返回：这里只做三件事（键码比对、窗口类名比对、排队投递），
//    "取剪贴板文件"与"渲染预览"都在界面线程上做。
//
//  ★ 有意不做的事（也是裁剪记录里的 C9）：
//    **不模拟 Ctrl+C 去抓资源管理器里的选中项**。那会覆盖用户剪贴板，而且
//    "文件列表型"剪贴板内容无法完整还原（只能还原文本/图片），属于本项目的红线。
//    用户要预览文件，在资源管理器里 Ctrl+C（复制文件）即可。
// ============================================================================

#include "sdk/IFeaturePlugin.h"

#include <QPointer>
#include <QString>

class QuickLookWindow;
class SpaceKeyListener;

class QuickLookPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "quick_look_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit QuickLookPlugin(QObject *parent = nullptr);

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

    // ========================================================================
    //  面板与自检共用的接口
    // ========================================================================

    /// 预览"剪贴板里能找到的那个文件"
    /// @return 是否成功定位到文件（false 时 messageOut 给出中文原因，窗口仍会显示提示）
    bool previewCurrentTarget(QString *messageOut = nullptr);

    /// 预览指定路径（← → 换文件、自检、以及"打开已知文件"都走它）
    bool previewPath(const QString &path, QString *messageOut = nullptr);

    /// 关闭预览（已打开时返回 true）
    bool closePreview();
    bool isPreviewVisible() const;
    /// 预览窗口（未打开为 nullptr；自检用它读控件内容）
    QWidget *previewWindow() const;

    /// 同目录里的前/后一个文件（offset = ∓1）；找不到返回空串
    QString siblingPath(const QString &path, int offset) const;

    /// 剪贴板文件解析（自检直接调它断言"从剪贴板拿到了什么"）
    static QString fileFromClipboard(QString *messageOut = nullptr);

    // ---------------- 配置（自检与设置面板都读这里）----------------
    qint64 maxBytes() const { return m_maxBytes; }
    bool spaceKeyEnabled() const { return m_spaceKey; }
    /// 空格键触发入口（由 SpaceKeyListener 从钩子线程排队调用）
    void toggleFromSpaceKey();

protected:
    bool onEnable() override;
    void onDisable() override;
    void onHotkey(const QString &hotkeyId) override;

private:
    void loadFromConfig();
    /// 打开/更新预览窗口并显示该文件
    void showWindowFor(const QString &path);

    SpaceKeyListener *m_listener = nullptr;
    QPointer<QuickLookWindow> m_window;
    QString m_lastPath;
    qint64 m_maxBytes = 512 * 1024;
    bool m_spaceKey = true;
};
