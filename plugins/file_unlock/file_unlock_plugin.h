#pragma once

// ============================================================================
//  FileUnlockPlugin —— P3-02 文件锁定解除（file.unlock）
//
//  解决的问题：**"这个文件删不掉，说是被占用了 —— 可到底是哪个程序在用它？"**
//  做法：Restart Manager（系统自己维护的"谁打开了哪个文件"，免提权只读）
//        → 列出占用者（名字 / PID / 路径 / 身份）
//        → 二次确认后结束选中的进程
//        → 再删（走**回收站**，随时能还原）
//
//  ---------------------------------------------------------------------------
//  四条设计纪律：
//
//   1. **先给证据，再给动作**：列表里必须看得到"是谁、什么身份、在哪个路径"。
//      只说"删不掉"是没用的答案；不含证据的"结束进程"按钮更是危险按钮。
//   2. **有些进程绝对不许结束，而且要说清为什么**（规则在 `plugins/common/UnlockPolicy`
//      ：WinEase 自己 / 提权助手 / 一小份 system-owned 名单）。这类行**禁用结束按钮**
//      并写明理由，而不是"点了才报错"。
//      ⚠⚠ 判据**不能**是 Restart Manager 的 `RmCritical`(=1000)：它的含义是
//      "装不下去、得重启才能释放"，成因有三种（确实关键进程 / 没权限关它 /
//      **它就是发起查询的这个程序自己**）。本机实测：同一份 exe 作为查询方被标 1000、
//      作为普通子进程被标 5。拿它当"不许结束"会让普通程序变成杀不掉 ——
//      所以它只配一句如实提示（"系统认为它关不掉，可能只是权限不足"）。
//   3. **二次确认是产品的安全阀**：结束一个可能正在编辑文件的程序，用户会丢数据。
//      确认文案要具体到"哪个进程、可能丢什么"（`UnlockPolicy::terminationWarning`）。
//   4. **删除一律走回收站**（`ShellUtils::moveToRecycleBin`）：解除占用的目的是
//      "让文件能被删掉"，但"删掉"本身不该是不可撤销的。想永久删请用粉碎功能（P2-12），
//      那条路有它自己的安全阀。
//
//  ---------------------------------------------------------------------------
//  ⚠ 两处必须如实说明的"能力边界"（界面上也写了）：
//    * Restart Manager **只看进程**：杀软实时扫描、索引服务、文件系统过滤驱动这类
//      内核态占用它报不出来 → "列表为空"的正确说法是"没有**进程**在占用"，
//      而不是"没人用它"。删除仍失败时如实报错，不要假装成功。
//    * **不做资源管理器右键菜单集成**（有意裁剪）：WinEase.exe 目前没有
//      "带参数打开某个插件面板"的入口，为这一项去改主程序启动流程与单实例转发
//      不划算。面板支持**拖入文件**、**从剪贴板取路径**、文件对话框三条入口。
//
//  ⚠ 结束进程需要管理员时**走提权助手**（D2 决策）；能本地做的绝不惊动它。
// ============================================================================

#include "sdk/IFeaturePlugin.h"
#include "win32/RestartManager.h"

#include <QList>
#include <QPointer>
#include <QString>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QWidget;

class FileUnlockPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "file_unlock_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit FileUnlockPlugin(QObject *parent = nullptr);

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
    bool canEnable(QString *reason) const override;
    bool onEnable() override;
    void onDisable() override;
    void onHotkey(const QString &hotkeyId) override;
    /// 面板支持"把文件/文件夹拖进来"（QWidget 的拖动事件只能在 QObject 层拦，
    /// 这样就不必为面板单独写一个 QWidget 子类）
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // ---- 业务动作 ----
    /// 查询并刷新占用者（只读）
    void scanPath(const QString &path);
    /// 从输入框取路径后查询
    void scanFromEdit();
    /// 结束列表里选中的那个进程（**先过保护规则与二次确认**）
    void killSelected();
    /// 再删一次：把文件移到回收站（失败时如实说出还剩下谁在占用）
    void deleteCurrent();
    /// 结束单个占用者；返回是否成功，messageOut 给出面向用户的一句话
    bool terminateLocker(const WinEase::Win32::FileLocker &locker, QString *messageOut);
    /// 危险动作前的二次确认（受配置 `confirm` 控制；自检把它关掉以免卡在模态框上）
    bool confirmed(const QString &title, const QString &text);

    // ---- 界面 ----
    /// 重填占用者列表（**只在数据变化时调用**；重填会改当前行，选中项变化时不许调它）
    void refreshLockerList();
    /// 只更新"结束/删除"按钮的可用状态（选中项变化时走这里，不动列表）
    void updateActionState();
    void refreshPanel();
    void updateDetail();
    /// 重新查询占用者 + 统计受保护的条数
    void refreshLockers();

    // ---- 数据 ----
    /// 当前查询的文件路径（空 = 还没查过）
    QString m_path;
    QList<WinEase::Win32::FileLocker> m_lockers;
    QString m_lastEvent;       ///< 状态栏（"检测到 N 个占用者"这类）
    QString m_lastResult;      ///< 上一次动作的结果（结束/删除的成败）
    bool m_confirm = true;     ///< 危险动作是否弹二次确认框
    /// 被保护（不允许结束）的占用者条数——面板与自检都要用
    int m_protectedCount = 0;

    QPointer<QWidget> m_panel;
    QPointer<QLabel> m_statusLabel;
    QPointer<QLabel> m_detailLabel;
    QPointer<QLabel> m_resultLabel;
    QPointer<QLineEdit> m_pathEdit;
    QPointer<QListWidget> m_lockerList;
    QPointer<QCheckBox> m_confirmCheck;
    QPointer<QPushButton> m_scanButton;
    QPointer<QPushButton> m_browseButton;
    QPointer<QPushButton> m_pasteButton;
    QPointer<QPushButton> m_killButton;
    QPointer<QPushButton> m_deleteButton;
};
