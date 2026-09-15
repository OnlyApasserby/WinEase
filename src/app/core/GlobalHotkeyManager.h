#pragma once

// ============================================================================
//  GlobalHotkeyManager.h —— 全局快捷键管理器（Windows）
//
//  实现方式：
//    * RegisterHotKey(nullptr, id, mods, vk) 把热键绑定到当前线程消息队列
//    * QAbstractNativeEventFilter 拦截线程消息中的 WM_HOTKEY 并派发信号
//    * 绑定关系持久化到 QSettings 的 [Hotkeys] 段，键名即 hotkeyId
//
//  hotkeyId 命名约定：
//    "<pluginId>::<action>"   例如 "window.pin::toggle"
//    "app.<action>"           主程序自身的快捷键，例如 "app.showMainWindow"
//
//  ⚠ 注意：RegisterHotKey 是进程级独占资源，注册失败通常意味着已被其他程序
//    （或被本进程其它绑定）占用，此时会通过 hotkeyFailed 信号告知调用方。
// ============================================================================

#include <QAbstractNativeEventFilter>
#include <QHash>
#include <QKeySequence>
#include <QObject>
#include <QString>
#include <QVector>

namespace WinEase {

/// 一条全局快捷键绑定
struct HotkeyBinding {
    QString hotkeyId;            ///< 全局唯一键
    QString ownerId;             ///< 归属者（插件 id 或 "app"）
    QString description;         ///< 中文描述，展示在设置界面
    QKeySequence sequence;       ///< 当前按键组合
    bool registered = false;     ///< 是否已成功注册到系统
    int systemId = 0;            ///< RegisterHotKey 使用的原子 id

    bool isValid() const { return !hotkeyId.isEmpty() && !sequence.isEmpty(); }
};

class GlobalHotkeyManager : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
    explicit GlobalHotkeyManager(QObject *parent = nullptr);
    ~GlobalHotkeyManager() override;

    GlobalHotkeyManager(const GlobalHotkeyManager &) = delete;
    GlobalHotkeyManager &operator=(const GlobalHotkeyManager &) = delete;

    // ---------------- 注册 / 注销 ----------------

    /// 注册快捷键；若 sequence 为空则仅记录（未启用）
    bool registerHotkey(const QString &hotkeyId,
                        const QKeySequence &sequence,
                        const QString &description = QString(),
                        const QString &ownerId = QString());

    /// 修改已有绑定的按键组合；与其它绑定冲突时返回 false 且保持原值不变
    bool updateHotkey(const QString &hotkeyId, const QKeySequence &sequence);

    bool unregisterHotkey(const QString &hotkeyId);
    void unregisterByOwner(const QString &ownerId);
    void unregisterAll();

    // ---------------- 查询 ----------------

    bool contains(const QString &hotkeyId) const;
    HotkeyBinding binding(const QString &hotkeyId) const;
    QVector<HotkeyBinding> bindings() const;
    QVector<HotkeyBinding> bindingsOfOwner(const QString &ownerId) const;
    bool isRegistered(const QString &hotkeyId) const;

    /// 查找与给定组合冲突的 hotkeyId（excludeId 用于排除自身）；无冲突返回空
    QString findConflict(const QKeySequence &sequence, const QString &excludeId = QString()) const;

    /// 冲突的中文描述（可直接展示给用户）；无冲突返回空字符串
    QString conflictDescription(const QKeySequence &sequence, const QString &excludeId = QString()) const;

    /// 综合校验：按键组合是否可用；不可用时通过 reason 返回中文原因
    bool validateSequence(const QKeySequence &sequence,
                          const QString &excludeId,
                          QString *reason = nullptr) const;

    /// 快捷键是否可用于全局注册（必须含修饰键，且键位可映射到 Win32 虚拟键）
    static bool isSupportedSequence(const QKeySequence &sequence);

    // ---------------- 持久化 ----------------

    /// 从 QSettings 恢复绑定（键名即 hotkeyId）
    void loadFromSettings();
    /// 写回 QSettings
    void saveToSettings() const;

    // ---------------- 文本互转 ----------------

    static QString sequenceToText(const QKeySequence &sequence);
    static QKeySequence textToSequence(const QString &text);
    /// 归一化显示，例如 "Ctrl+Alt+P"
    static QString normalizeText(const QString &text);

    // ---------------- QAbstractNativeEventFilter ----------------
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

Q_SIGNALS:
    /// 快捷键被按下
    void hotkeyTriggered(const QString &hotkeyId);
    /// 成功注册
    void hotkeyRegistered(const QString &hotkeyId, const QKeySequence &sequence);
    /// 注册失败（被占用 / 组合不受支持）
    void hotkeyFailed(const QString &hotkeyId, const QString &reason);
    /// 绑定集合发生变化
    void bindingsChanged();

private:
    int allocateSystemId();
    void releaseSystemId(int systemId);
    bool applyRegistration(HotkeyBinding &binding, QString *failureReason = nullptr);
    void removeRegistration(HotkeyBinding &binding);

    QHash<QString, HotkeyBinding> m_bindings;  ///< hotkeyId -> 绑定
    QHash<int, QString> m_idBySystemId;        ///< 原子 id -> hotkeyId
    int m_nextSystemId = 0xB000;               ///< 与应用级 id 区间隔离，避免碰撞
};

} // namespace WinEase

Q_DECLARE_METATYPE(WinEase::HotkeyBinding)
