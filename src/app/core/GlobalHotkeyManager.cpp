#include "core/GlobalHotkeyManager.h"

#include "core/Logging.h"
#include "core/SettingsManager.h"

#include <QCoreApplication>
#include <QStringList>

#include <algorithm>
#include <utility>

#include <windows.h>

namespace WinEase {

namespace {

/// 应用自定义快捷键时不希望收到自动重复（长按连发）
constexpr UINT kHotkeyExtraModifiers = MOD_NOREPEAT;

/// Qt 修饰键 -> Win32 修饰键
UINT toNativeModifiers(Qt::KeyboardModifiers modifiers)
{
    UINT result = 0;
    if (modifiers & Qt::ControlModifier) {
        result |= MOD_CONTROL;
    }
    if (modifiers & Qt::AltModifier) {
        result |= MOD_ALT;
    }
    if (modifiers & Qt::ShiftModifier) {
        result |= MOD_SHIFT;
    }
    if (modifiers & Qt::MetaModifier) {
        result |= MOD_WIN;
    }
    return result;
}

/// Qt 键值 -> Win32 虚拟键码
bool toNativeKey(int key, UINT *nativeKey)
{
    if (!nativeKey) {
        return false;
    }

    // A-Z / 0-9（Qt 的键值恰好与 ASCII 一致）
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        *nativeKey = static_cast<UINT>('A' + (key - Qt::Key_A));
        return true;
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        *nativeKey = static_cast<UINT>('0' + (key - Qt::Key_0));
        return true;
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
        *nativeKey = static_cast<UINT>(VK_F1 + (key - Qt::Key_F1));
        return true;
    }

    switch (key) {
    case Qt::Key_Escape:    *nativeKey = VK_ESCAPE;     return true;
    case Qt::Key_Tab:       *nativeKey = VK_TAB;        return true;
    case Qt::Key_Backtab:   *nativeKey = VK_TAB;        return true;
    case Qt::Key_Backspace: *nativeKey = VK_BACK;       return true;
    case Qt::Key_Return:    *nativeKey = VK_RETURN;     return true;
    case Qt::Key_Enter:     *nativeKey = VK_RETURN;     return true;
    case Qt::Key_Insert:    *nativeKey = VK_INSERT;     return true;
    case Qt::Key_Delete:    *nativeKey = VK_DELETE;     return true;
    case Qt::Key_Pause:     *nativeKey = VK_PAUSE;      return true;
    case Qt::Key_Print:     *nativeKey = VK_SNAPSHOT;   return true;
    case Qt::Key_Home:      *nativeKey = VK_HOME;       return true;
    case Qt::Key_End:       *nativeKey = VK_END;        return true;
    case Qt::Key_Left:      *nativeKey = VK_LEFT;       return true;
    case Qt::Key_Up:        *nativeKey = VK_UP;         return true;
    case Qt::Key_Right:     *nativeKey = VK_RIGHT;      return true;
    case Qt::Key_Down:      *nativeKey = VK_DOWN;       return true;
    case Qt::Key_PageUp:    *nativeKey = VK_PRIOR;      return true;
    case Qt::Key_PageDown:  *nativeKey = VK_NEXT;       return true;
    case Qt::Key_NumLock:   *nativeKey = VK_NUMLOCK;    return true;
    case Qt::Key_ScrollLock:*nativeKey = VK_SCROLL;     return true;
    case Qt::Key_Menu:      *nativeKey = VK_APPS;       return true;
    case Qt::Key_Space:     *nativeKey = VK_SPACE;      return true;
    case Qt::Key_Plus:      *nativeKey = VK_OEM_PLUS;   return true;
    case Qt::Key_Minus:     *nativeKey = VK_OEM_MINUS;  return true;
    case Qt::Key_Comma:     *nativeKey = VK_OEM_COMMA;  return true;
    case Qt::Key_Period:    *nativeKey = VK_OEM_PERIOD; return true;
    case Qt::Key_Slash:     *nativeKey = VK_OEM_2;      return true;
    case Qt::Key_Semicolon: *nativeKey = VK_OEM_1;      return true;
    case Qt::Key_QuoteLeft: *nativeKey = VK_OEM_3;      return true;
    case Qt::Key_BracketLeft:  *nativeKey = VK_OEM_4;   return true;
    case Qt::Key_Backslash:    *nativeKey = VK_OEM_5;   return true;
    case Qt::Key_BracketRight: *nativeKey = VK_OEM_6;   return true;
    case Qt::Key_Apostrophe:   *nativeKey = VK_OEM_7;   return true;
    default:
        break;
    }

    return false;
}

bool toNativeSequence(const QKeySequence &sequence, UINT *modifiers, UINT *virtualKey)
{
    if (sequence.isEmpty() || sequence.count() < 1) {
        return false;
    }

    const QKeyCombination combination = sequence[0];
    const UINT mods = toNativeModifiers(combination.keyboardModifiers());
    UINT key = 0;
    if (!toNativeKey(static_cast<int>(combination.key()), &key)) {
        return false;
    }
    // 全局快捷键必须带修饰键，否则会吞掉正常输入
    if (mods == 0) {
        return false;
    }

    if (modifiers) {
        *modifiers = mods;
    }
    if (virtualKey) {
        *virtualKey = key;
    }
    return true;
}

} // namespace

// ============================================================================
//  构造 / 析构
// ============================================================================

GlobalHotkeyManager::GlobalHotkeyManager(QObject *parent)
    : QObject(parent)
{
    if (QCoreApplication::instance()) {
        QCoreApplication::instance()->installNativeEventFilter(this);
    }
    qCInfo(lcHotkey) << "全局快捷键管理器已创建";
}

GlobalHotkeyManager::~GlobalHotkeyManager()
{
    unregisterAll();
    if (QCoreApplication::instance()) {
        QCoreApplication::instance()->removeNativeEventFilter(this);
    }
}

// ============================================================================
//  注册 / 注销
// ============================================================================

bool GlobalHotkeyManager::registerHotkey(const QString &hotkeyId,
                                         const QKeySequence &sequence,
                                         const QString &description,
                                         const QString &ownerId)
{
    if (hotkeyId.isEmpty()) {
        return false;
    }

    HotkeyBinding &binding = m_bindings[hotkeyId];
    const bool existed = binding.registered;
    if (existed) {
        removeRegistration(binding);
    }

    binding.hotkeyId = hotkeyId;
    binding.description = description;
    binding.ownerId = ownerId.isEmpty() ? hotkeyId.section(QStringLiteral("::"), 0, 0) : ownerId;
    binding.sequence = sequence;

    if (sequence.isEmpty()) {
        // 仅登记、不注册（用户可后续在设置界面指定按键）
        Q_EMIT bindingsChanged();
        return true;
    }

    // 与已有绑定冲突时直接拒绝，避免拿系统错误码做二次猜测
    QString reason;
    if (!validateSequence(sequence, hotkeyId, &reason)) {
        Q_EMIT hotkeyFailed(hotkeyId, reason);
        Q_EMIT bindingsChanged();
        return false;
    }

    QString failure;
    if (!applyRegistration(binding, &failure)) {
        Q_EMIT hotkeyFailed(hotkeyId, failure);
        Q_EMIT bindingsChanged();
        return false;
    }

    // 描述留空时回退为按键文本，保证托盘菜单有可读文字
    if (binding.description.isEmpty()) {
        binding.description = sequenceToText(sequence);
    }

    Q_EMIT hotkeyRegistered(hotkeyId, sequence);
    Q_EMIT bindingsChanged();
    return true;
}

bool GlobalHotkeyManager::updateHotkey(const QString &hotkeyId, const QKeySequence &sequence)
{
    if (!m_bindings.contains(hotkeyId)) {
        return false;
    }

    if (!sequence.isEmpty()) {
        QString reason;
        if (!validateSequence(sequence, hotkeyId, &reason)) {
            Q_EMIT hotkeyFailed(hotkeyId, reason);
            return false;
        }
    }

    HotkeyBinding &binding = m_bindings[hotkeyId];
    removeRegistration(binding);
    binding.sequence = sequence;

    if (!sequence.isEmpty()) {
        QString failure;
        if (!applyRegistration(binding, &failure)) {
            Q_EMIT hotkeyFailed(hotkeyId, failure);
            return false;
        }
    }

    Q_EMIT bindingsChanged();
    return true;
}

bool GlobalHotkeyManager::unregisterHotkey(const QString &hotkeyId)
{
    auto it = m_bindings.find(hotkeyId);
    if (it == m_bindings.end()) {
        return false;
    }
    removeRegistration(it.value());
    m_bindings.erase(it);
    Q_EMIT bindingsChanged();
    return true;
}

void GlobalHotkeyManager::unregisterByOwner(const QString &ownerId)
{
    QStringList targets;
    for (auto it = m_bindings.cbegin(); it != m_bindings.cend(); ++it) {
        if (it.value().ownerId == ownerId) {
            targets.append(it.key());
        }
    }
    for (const QString &hotkeyId : std::as_const(targets)) {
        unregisterHotkey(hotkeyId);
    }
}

void GlobalHotkeyManager::unregisterAll()
{
    for (auto it = m_bindings.begin(); it != m_bindings.end(); ++it) {
        removeRegistration(it.value());
    }
    m_bindings.clear();
    m_idBySystemId.clear();
    Q_EMIT bindingsChanged();
}

// ============================================================================
//  查询
// ============================================================================

bool GlobalHotkeyManager::contains(const QString &hotkeyId) const
{
    return m_bindings.contains(hotkeyId);
}

HotkeyBinding GlobalHotkeyManager::binding(const QString &hotkeyId) const
{
    return m_bindings.value(hotkeyId);
}

QVector<HotkeyBinding> GlobalHotkeyManager::bindings() const
{
    QVector<HotkeyBinding> result;
    result.reserve(m_bindings.size());
    for (auto it = m_bindings.cbegin(); it != m_bindings.cend(); ++it) {
        result.append(it.value());
    }
    std::sort(result.begin(), result.end(), [](const HotkeyBinding &lhs, const HotkeyBinding &rhs) {
        return lhs.hotkeyId < rhs.hotkeyId;
    });
    return result;
}

QVector<HotkeyBinding> GlobalHotkeyManager::bindingsOfOwner(const QString &ownerId) const
{
    QVector<HotkeyBinding> result;
    for (auto it = m_bindings.cbegin(); it != m_bindings.cend(); ++it) {
        if (it.value().ownerId == ownerId) {
            result.append(it.value());
        }
    }
    return result;
}

bool GlobalHotkeyManager::isRegistered(const QString &hotkeyId) const
{
    const auto it = m_bindings.constFind(hotkeyId);
    return it != m_bindings.cend() && it.value().registered;
}

QString GlobalHotkeyManager::findConflict(const QKeySequence &sequence, const QString &excludeId) const
{
    if (sequence.isEmpty()) {
        return {};
    }
    for (auto it = m_bindings.cbegin(); it != m_bindings.cend(); ++it) {
        if (it.key() == excludeId) {
            continue;
        }
        if (!it.value().sequence.isEmpty() && it.value().sequence == sequence) {
            return it.key();
        }
    }
    return {};
}

QString GlobalHotkeyManager::conflictDescription(const QKeySequence &sequence,
                                                 const QString &excludeId) const
{
    const QString conflictId = findConflict(sequence, excludeId);
    if (conflictId.isEmpty()) {
        return QString();
    }

    const HotkeyBinding conflicting = binding(conflictId);
    const QString owner = conflicting.description.isEmpty() ? conflictId : conflicting.description;
    return tr("快捷键 %1 已被「%2」占用").arg(sequenceToText(sequence), owner);
}

bool GlobalHotkeyManager::validateSequence(const QKeySequence &sequence,
                                           const QString &excludeId,
                                           QString *reason) const
{
    // 空序列表示"不启用"，属于合法状态
    if (sequence.isEmpty()) {
        return true;
    }

    if (sequence.count() > 1) {
        if (reason) {
            *reason = tr("不支持组合键序列（如 Ctrl+K, Ctrl+C），请只录入单个按键组合");
        }
        return false;
    }

    if (!isSupportedSequence(sequence)) {
        if (reason) {
            *reason = tr("不支持的快捷键 %1：必须包含 Ctrl / Alt / Shift / Win 中的至少一个修饰键")
                          .arg(sequenceToText(sequence));
        }
        return false;
    }

    const QString conflict = conflictDescription(sequence, excludeId);
    if (!conflict.isEmpty()) {
        if (reason) {
            *reason = conflict;
        }
        return false;
    }

    return true;
}

bool GlobalHotkeyManager::isSupportedSequence(const QKeySequence &sequence)
{
    UINT modifiers = 0;
    UINT virtualKey = 0;
    return toNativeSequence(sequence, &modifiers, &virtualKey);
}

// ============================================================================
//  内部：注册到系统
// ============================================================================

int GlobalHotkeyManager::allocateSystemId()
{
    // 在私有区间内循环查找未被占用的 id
    for (int attempt = 0; attempt < 0x1000; ++attempt) {
        const int candidate = m_nextSystemId++;
        if (m_nextSystemId > 0xBFFF) {
            m_nextSystemId = 0xB000;
        }
        if (!m_idBySystemId.contains(candidate)) {
            return candidate;
        }
    }
    return 0;
}

void GlobalHotkeyManager::releaseSystemId(int systemId)
{
    if (systemId != 0) {
        m_idBySystemId.remove(systemId);
    }
}

bool GlobalHotkeyManager::applyRegistration(HotkeyBinding &binding, QString *failureReason)
{
    UINT modifiers = 0;
    UINT virtualKey = 0;
    if (!toNativeSequence(binding.sequence, &modifiers, &virtualKey)) {
        if (failureReason) {
            *failureReason = tr("不支持的快捷键组合：%1").arg(sequenceToText(binding.sequence));
        }
        return false;
    }

    const int systemId = allocateSystemId();
    if (systemId == 0) {
        if (failureReason) {
            *failureReason = tr("系统快捷键标识耗尽");
        }
        return false;
    }

    // hwnd 传 nullptr：WM_HOTKEY 投递到当前线程消息队列，由 nativeEventFilter 捕获
    if (!::RegisterHotKey(nullptr, systemId, modifiers | kHotkeyExtraModifiers, virtualKey)) {
        const DWORD error = ::GetLastError();
        if (failureReason) {
            *failureReason = (error == ERROR_HOTKEY_ALREADY_REGISTERED)
                                 ? tr("快捷键 %1 已被其它程序占用").arg(sequenceToText(binding.sequence))
                                 : tr("注册快捷键 %1 失败（错误码 %2）")
                                       .arg(sequenceToText(binding.sequence))
                                       .arg(error);
        }
        releaseSystemId(systemId);
        return false;
    }

    binding.systemId = systemId;
    binding.registered = true;
    m_idBySystemId.insert(systemId, binding.hotkeyId);

    qCInfo(lcHotkey) << "已注册快捷键" << sequenceToText(binding.sequence)
                     << "->" << binding.hotkeyId << "(id" << systemId << ")";
    return true;
}

void GlobalHotkeyManager::removeRegistration(HotkeyBinding &binding)
{
    if (binding.registered && binding.systemId != 0) {
        ::UnregisterHotKey(nullptr, binding.systemId);
        releaseSystemId(binding.systemId);
        qCInfo(lcHotkey) << "已注销快捷键" << binding.hotkeyId;
    }
    binding.registered = false;
    binding.systemId = 0;
}

// ============================================================================
//  消息过滤
// ============================================================================

bool GlobalHotkeyManager::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
{
    Q_UNUSED(result)

    if (eventType != QByteArrayLiteral("windows_generic_MSG")
        && eventType != QByteArrayLiteral("windows_dispatcher_MSG")) {
        return false;
    }

    MSG *msg = static_cast<MSG *>(message);
    if (!msg || msg->message != WM_HOTKEY) {
        return false;
    }

    const int systemId = static_cast<int>(msg->wParam);
    const auto it = m_idBySystemId.constFind(systemId);
    if (it == m_idBySystemId.cend()) {
        return false;
    }

    const QString hotkeyId = it.value();
    qCDebug(lcHotkey) << "触发快捷键" << hotkeyId;
    Q_EMIT hotkeyTriggered(hotkeyId);
    return false; // 不拦截消息，继续其它处理
}

// ============================================================================
//  持久化
// ============================================================================

void GlobalHotkeyManager::loadFromSettings()
{
    SettingsManager &settings = SettingsManager::instance();
    for (const HotkeyBinding &existing : bindings()) {
        const QString text = settings.hotkeyText(existing.hotkeyId);
        if (text.isEmpty()) {
            continue;
        }
        updateHotkey(existing.hotkeyId, textToSequence(text));
    }
    qCInfo(lcHotkey) << "快捷键配置已从设置载入";
}

void GlobalHotkeyManager::saveToSettings() const
{
    SettingsManager &settings = SettingsManager::instance();
    for (auto it = m_bindings.cbegin(); it != m_bindings.cend(); ++it) {
        settings.setHotkeyText(it.key(), sequenceToText(it.value().sequence));
    }
    settings.sync();
}

// ============================================================================
//  文本互转
// ============================================================================

QString GlobalHotkeyManager::sequenceToText(const QKeySequence &sequence)
{
    if (sequence.isEmpty()) {
        return QString();
    }
    return sequence.toString(QKeySequence::PortableText);
}

QKeySequence GlobalHotkeyManager::textToSequence(const QString &text)
{
    if (text.trimmed().isEmpty()) {
        return {};
    }
    return QKeySequence::fromString(text.trimmed(), QKeySequence::PortableText);
}

QString GlobalHotkeyManager::normalizeText(const QString &text)
{
    return sequenceToText(textToSequence(text));
}

} // namespace WinEase
