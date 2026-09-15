#pragma once

// ============================================================================
//  WallpaperPlugin —— P1-14 壁纸自动切换
//
//  行为：
//    * `default`（默认快捷键）：切到文件夹里的下一张（按序 / 随机，见配置）
//    * `prev`：回上一张（等价于历史回滚）
//    * `restore`：立即恢复"启用本功能之前"的那张壁纸
//    * 配置 `intervalMinutes > 0` 时自动定时轮换（0 = 只手动）
//    * `perMonitor`：把选中的图轮流分给各显示器（多屏各一张）
//
//  写入路径：平台层 `win32/DesktopWallpaper`（IDesktopWallpaper 优先，SPI 兜底）。
//
//  ★ 还原契约：**停用功能 → 恢复启用前的壁纸**；**退出 WinEase → 保持**
//      （验收原文就是"重启后保持"；壁纸是用户看得见的结果，不该因为退出而跳变）。
//    因此这里**不做**"启动时兜底还原"：那会把用户重启后想保留的壁纸翻回去。
//
//  ⚠ dryRun：只报"下一张会是谁"，不真改壁纸 —— 用于"先看看效果"与自动化校验。
// ============================================================================

#include "sdk/IFeaturePlugin.h"

#include <QStringList>
#include <QTimer>

class WallpaperPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "wallpaper_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit WallpaperPlugin(QObject *parent = nullptr);

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
    /// 扫描配置目录里的图片（按文件名排序，保证"按序"是可预期的）
    QStringList imageFiles() const;
    /// 当前正在用第几张（按路径匹配；找不到返回 -1）
    int currentImageIndex(const QStringList &files) const;
    /// 切到第 index 张（越界自动回绕）；dryRun 时只回报不改
    bool applyImage(int index, QString *appliedOut);
    /// 相对切换（step = +1 / -1）
    bool step(int delta);
    /// 恢复启用前的壁纸
    bool restoreOriginal();
    void restartTimer();
    QString describeCurrent() const;

    QString m_folder;
    QString m_mode = QStringLiteral("sequential"); ///< sequential / random
    int m_intervalMinutes = 0;
    bool m_dryRun = false;
    bool m_perMonitor = false;

    QString m_original;   ///< 启用前的壁纸（还原用）
    bool m_hasOriginal = false;
    QString m_lastApplied; ///< 最近一次是我们设的那张
    QStringList m_history; ///< 最近 10 次（便于回滚与排查）
    int m_cursor = -1;     ///< 按序播放的游标

    QTimer m_timer;
};
