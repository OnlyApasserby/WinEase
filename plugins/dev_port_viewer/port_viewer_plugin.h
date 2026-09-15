#pragma once

// ============================================================================
//  PortViewerPlugin —— P1-12 端口占用查看
//
//  行为：
//    * 快捷键（默认 Ctrl+Alt+P）：把**剪贴板里的端口号**查一遍，
//      结果（占用进程名 + 完整路径）写回剪贴板并在卡片上给出状态文本
//      —— 排查"端口被占"时最常见的动作就是"复制到端口号 → 想知道是谁"
//    * 设置面板：按端口 / PID / 进程名查询，列出监听与连接，
//      可选中一行直接**结束进程**（系统进程走提权助手，见 requiresAdmin 说明）
//
//  权限：
//      **枚举端口是只读操作，普通权限即可**（能查到系统进程的端口）。
//      只有"结束进程"需要管理员，因此 requiresAdmin 为 false，
//      真正提权只发生在用户点"结束进程"时（经 ElevationService → WinEaseHelper）。
// ============================================================================

#include "sdk/IFeaturePlugin.h"

#include <QString>

class PortViewerPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "port_viewer_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit PortViewerPlugin(QObject *parent = nullptr);

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
    /// 查询端口并把结果写进剪贴板 + 状态文本；返回是否查到占用
    bool queryPort(quint16 port);

    /// 用提权助手结束进程
    bool terminateByElevation(quint32 pid, QString *messageOut);

    /// 从文本里提取第一个看起来像端口号的数字（1..65535）
    static int extractPort(const QString &text);
};
