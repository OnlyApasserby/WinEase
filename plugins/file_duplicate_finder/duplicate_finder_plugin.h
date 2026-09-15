#pragma once

// ============================================================================
//  DuplicateFinderPlugin —— P2-03 重复文件查找
//
//  界面只做三件事：给个目录、看进度、把**建议删除项**移进回收站。
//  真正的四级流水线在 `plugins/common/DuplicateFinder.*`（纯函数，自检共用）。
//
//  ★ 产品硬约束：重复文件**绝不做静默硬删**。
//    「移到回收站」走的是一律是 `ShellUtils::moveToRecycleBin()`，
//    用户随时能在回收站里翻回来。想永久删？那是粉碎功能的事，且必须自己承担。
//
//  ★ 扫描在后台线程里跑（枚举 + 采样哈希 + 全量 SHA-256 都是 IO/CPU 混合），
//    进度回调**可能来自引擎内部的工作线程** → 一律排队转成界面线程的信号。
// ============================================================================

#include "sdk/IFeaturePlugin.h"

#include <QPointer>

class DuplicateFinderWindow;

class DuplicateFinderPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "duplicate_finder_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit DuplicateFinderPlugin(QObject *parent = nullptr);

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

    // ---------------- 面板与自检共用的接口 ----------------
    /// 移入回收站前是否弹确认（`[Plugins/file.duplicate_finder] confirm`，默认 true）
    bool confirmBeforeRecycle() const { return m_confirm; }
    /// 采样哈希读多少字节（默认 4 KB；自检会核对"抽样不是摆设"）
    int sampleBytes() const { return m_sampleBytes; }
    /// 面板改过抽样大小 → 写回内存与配置（面板是唯一改它的地方，下次打开还是这个值）
    void setSampleBytes(int bytes);
    /// 上次用过的扫描目录（面板打开时的初始值）
    QStringList defaultRoots() const { return m_defaultRoots; }
    QWidget *panelWindow() const;

protected:
    bool onEnable() override;
    void onDisable() override;
    void onHotkey(const QString &hotkeyId) override;

private:
    void loadFromConfig();
    void openPanel();
    void closePanel();

    QPointer<DuplicateFinderWindow> m_window;
    QStringList m_defaultRoots;
    bool m_confirm = true;
    int m_sampleBytes = 4096;
};
