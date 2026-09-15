#pragma once

// ============================================================================
//  WindowFeatureState.h —— 窗口类插件共用的"目标窗口"状态
//
//  复用原因（不是"为了少写代码"，而是为了行为一致）：
//    1. 四个窗口插件必须给出**完全相同**的目标解析语义，
//       否则用户会遇到"置顶跟随鼠标、分屏却用前台窗口"这种不一致
//    2. 配置键名统一（targetMode），设置面板里的"目标窗口"区块长得一样
//
//  ⚠ 本头文件**不含 Q_OBJECT**：它只是普通 C++ 辅助类，
//     这样四个插件各自 AUTOMOC 时不会牵扯到公共头文件的 moc 产物
//     （跨插件共享带 Q_OBJECT 的头文件，moc 输出会重复定义）。
//
//  锁定窗口为什么**不落盘**：
//      HWND 是进程内的句柄值，重启后会被复用，落盘再读回几乎必然指向别的窗口。
//      因此只持久化"模式"，锁定目标本身在下次启动时重新锁定。
// ============================================================================

#include "sdk/PluginServices.h"
#include "win32/WindowTarget.h"

#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

namespace WinEase::FeaturePlugins {

using Win32::WindowHandle;
using Win32::WindowTargetMode;

class WindowTargetState
{
public:
    /// 从配置恢复（构造函数不能读配置，因为那时还没有 services）
    void load(PluginServices *services, const QString &pluginId)
    {
        m_services = services;
        m_pluginId = pluginId;
        m_locked = nullptr;
        if (m_services == nullptr) {
            return;
        }
        const QString key = m_services->configValue(pluginId,
                                                    QStringLiteral("targetMode"),
                                                    QStringLiteral("follow_cursor"))
                                .toString();
        m_mode = Win32::targetModeFromKey(key, WindowTargetMode::FollowCursor);
    }

    WindowTargetMode mode() const { return m_mode; }

    bool isLocked() const
    {
        return m_mode == WindowTargetMode::LockedWindow && Win32::isValidWindow(m_locked);
    }

    WindowHandle lockedWindow() const { return m_locked; }

    /// 本次操作应该作用在哪个窗口上（找不到返回 nullptr）
    WindowHandle resolveTarget() const
    {
        return Win32::resolveWindowTarget(m_mode, m_locked);
    }

    /// 锁定"当前能解析到的窗口"（无论当前是什么模式），并切到锁定模式
    WindowHandle lockCurrent()
    {
        const WindowHandle target = Win32::resolveWindowTarget(WindowTargetMode::FollowCursor, nullptr);
        if (target == nullptr) {
            return nullptr;
        }
        m_locked = target;
        m_mode = WindowTargetMode::LockedWindow;
        persist();
        return target;
    }

    void unlock()
    {
        m_locked = nullptr;
        m_mode = WindowTargetMode::FollowCursor;
        persist();
    }

    /// 目标窗口的人话描述（用于状态文本与设置面板）
    static QString describe(WindowHandle hwnd)
    {
        if (!Win32::isValidWindow(hwnd)) {
            return QStringLiteral("（没有可操作的目标窗口）");
        }
        const QString title = Win32::windowTitle(hwnd).trimmed();
        const QString process = Win32::windowProcessName(hwnd);
        if (title.isEmpty()) {
            return process.isEmpty() ? QStringLiteral("(无标题窗口)") : process;
        }
        return process.isEmpty() ? title : QStringLiteral("%1 —— %2").arg(title, process);
    }

    // ---------------- 锁定动作（所有窗口插件共用同一套语义） ----------------
    //
    //  为什么把"锁定"做成每个窗口插件都有的动作：
    //    1. 用户体验一致：不管是置顶、分屏、透明度还是置底，都能先把窗口钉住再反复操作
    //    2. 语义必须只有一个实现：如果四个插件各写一份，"锁定"到底锁哪个窗口一定会打架
    //    3. 锁定后所有操作都不再依赖"当前鼠标在哪"，行为可预期（也便于自检复现）

    /// 注册"锁定 / 解除锁定"辅助快捷键；返回 false 表示被占用（不致命）
    bool registerLockHotkey(const QKeySequence &sequence)
    {
        if (m_services == nullptr) {
            return false;
        }
        return m_services->registerHotkey(m_pluginId,
                                          QStringLiteral("lock"),
                                          sequence,
                                          QStringLiteral("WinEase：锁定 / 解除锁定目标窗口"));
    }

    void unregisterLockHotkey()
    {
        if (m_services != nullptr) {
            m_services->unregisterHotkey(m_pluginId, QStringLiteral("lock"));
        }
    }

    /// 处理 "lock" 动作；返回 true 表示该动作已被消费（插件应直接返回）
    bool handleLockAction(const QString &action, QString *statusText)
    {
        if (action != QLatin1String("lock")) {
            return false;
        }
        if (isLocked()) {
            unlock();
            if (statusText != nullptr) {
                *statusText = QStringLiteral("已解除锁定（回到跟随鼠标）");
            }
            return true;
        }

        const WindowHandle locked = lockCurrent();
        if (statusText != nullptr) {
            *statusText = (locked != nullptr)
                              ? QStringLiteral("已锁定：%1").arg(describe(locked))
                              : QStringLiteral("没有找到可锁定的窗口");
        }
        return true;
    }

    /// 设置面板里的"目标窗口"区块：模式选择 + 锁定/解锁 + 当前目标说明
    QWidget *createGroup(QWidget *parent)
    {
        auto *group = new QGroupBox(QStringLiteral("目标窗口"), parent);
        auto *layout = new QVBoxLayout(group);

        auto *modeBox = new QComboBox(group);
        modeBox->addItem(QStringLiteral("跟随鼠标下的窗口"), static_cast<int>(WindowTargetMode::FollowCursor));
        modeBox->addItem(QStringLiteral("锁定窗口（反复操作同一个）"), static_cast<int>(WindowTargetMode::LockedWindow));
        layout->addWidget(modeBox);

        auto *buttons = new QHBoxLayout();
        auto *lockButton = new QPushButton(QStringLiteral("锁定当前窗口"), group);
        auto *unlockButton = new QPushButton(QStringLiteral("解除锁定"), group);
        buttons->addWidget(lockButton);
        buttons->addWidget(unlockButton);
        buttons->addStretch(1);
        layout->addLayout(buttons);

        auto *detail = new QLabel(group);
        detail->setWordWrap(true);
        layout->addWidget(detail);

        // refresh 被按钮与模式切换共用：任何改变都立刻反映到界面上
        const auto refresh = [this, modeBox, detail] {
            modeBox->setCurrentIndex(isLocked() ? 1 : 0);
            detail->setText(isLocked()
                                ? QStringLiteral("已锁定：%1").arg(describe(m_locked))
                                : QStringLiteral("当前跟随鼠标；触发时作用在光标下的窗口上。"));
        };
        refresh();

        // 本类不是 QObject 子类，因此必须写全 QObject::connect
        QObject::connect(modeBox, &QComboBox::currentIndexChanged, group, [this, refresh](int index) {
            m_mode = (index == 1) ? WindowTargetMode::LockedWindow : WindowTargetMode::FollowCursor;
            if (m_mode == WindowTargetMode::FollowCursor) {
                m_locked = nullptr;
            }
            persist();
            refresh();
        });
        QObject::connect(lockButton, &QPushButton::clicked, group, [this, refresh] {
            lockCurrent(); // 失败时下面仍会刷新出"没有可操作的目标窗口"
            refresh();
        });
        QObject::connect(unlockButton, &QPushButton::clicked, group, [this, refresh] {
            unlock();
            refresh();
        });

        return group;
    }

private:
    void persist()
    {
        if (m_services != nullptr) {
            m_services->setConfigValue(m_pluginId,
                                       QStringLiteral("targetMode"),
                                       Win32::targetModeKey(m_mode));
        }
    }

    PluginServices *m_services = nullptr;
    QString m_pluginId;
    WindowTargetMode m_mode = WindowTargetMode::FollowCursor;
    WindowHandle m_locked = nullptr;
};

} // namespace WinEase::FeaturePlugins
