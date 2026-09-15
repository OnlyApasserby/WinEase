#pragma once

// ============================================================================
//  RegistryUtils.h —— 注册表访问封装
//
//  用途（对应路线图中多个功能）：
//      * 右键菜单扩展（HKCU\Software\Classes\*\shell\...）
//      * 主题切换（HKCU\...\Themes\Personalize）
//      * 开始菜单增强（HKCU\...\Explorer\Advanced）
//      * 环境变量管理（HKCU\Environment / HKLM\...\Environment）
//
//  设计要点：
//      1. RAII 管理 HKEY，异常路径不泄漏句柄
//      2. QVariant 与注册表类型自动互转，读写不用手写 RegQueryValueEx
//      3. **优先使用 HKCU**——免提权即可生效，只有系统级修改才需要 HKLM
//      4. 提供 64/32 位视图选择：环境变量、卸载信息等存在 WOW64 重定向陷阱
// ============================================================================

#include <QString>
#include <QStringList>
#include <QVariant>

#include <windows.h>

namespace WinEase::Win32 {

/// 注册表根键
enum class RegistryRoot {
    CurrentUser,   ///< HKEY_CURRENT_USER（免提权，优先使用）
    LocalMachine   ///< HKEY_LOCAL_MACHINE（多数修改需要管理员权限）
};

/// 64/32 位视图
enum class RegistryView {
    Default,  ///< 跟随当前进程位数（64 位进程即 64 位视图）
    Force64,  ///< 强制 64 位视图
    Force32,  ///< 强制 32 位视图（WOW6432Node）
    Force32On64 = Force32
};

/// 注册表值类型（Auto 时按 QVariant 的真实类型推断）
enum class RegistryValueType {
    Auto,
    String,        ///< REG_SZ
    ExpandString,  ///< REG_EXPAND_SZ（含 %PATH% 这类变量引用时必须用它）
    DWord,         ///< REG_DWORD
    QWord,         ///< REG_QWORD
    Binary,        ///< REG_BINARY
    MultiString    ///< REG_MULTI_SZ
};

/// 注册表键的 RAII 包装（不可拷贝，可移动）
class RegistryKey
{
public:
    RegistryKey() = default;
    ~RegistryKey();

    RegistryKey(const RegistryKey &) = delete;
    RegistryKey &operator=(const RegistryKey &) = delete;
    RegistryKey(RegistryKey &&other) noexcept;
    RegistryKey &operator=(RegistryKey &&other) noexcept;

    /// 打开已存在的键（不存在则失败）
    static RegistryKey open(RegistryRoot root,
                            const QString &subKey,
                            bool readOnly = true,
                            RegistryView view = RegistryView::Default,
                            QString *errorOut = nullptr);

    /// 打开或创建键
    static RegistryKey create(RegistryRoot root,
                              const QString &subKey,
                              RegistryView view = RegistryView::Default,
                              QString *errorOut = nullptr);

    bool isValid() const { return m_key != nullptr; }
    HKEY nativeHandle() const { return m_key; }
    QString errorMessage() const { return m_error; }

    // ---------------- 值读写 ----------------

    /// 读取值；不存在或类型不受支持时返回 defaultValue
    QVariant value(const QString &name, const QVariant &defaultValue = QVariant()) const;

    /// 读取并展开环境变量引用（仅对字符串类值有意义）
    QString expandedValue(const QString &name, const QString &defaultValue = QString()) const;

    bool hasValue(const QString &name) const;

    bool setValue(const QString &name,
                  const QVariant &value,
                  RegistryValueType type = RegistryValueType::Auto,
                  QString *errorOut = nullptr);

    bool removeValue(const QString &name, QString *errorOut = nullptr);

    QStringList valueNames() const;
    QStringList subKeyNames() const;

    /// 递归删除本键及其所有子键（功能卸载时清理用，危险操作需二次确认）
    bool removeRecursively(QString *errorOut = nullptr);

private:
    HKEY m_key = nullptr;
    mutable QString m_error;
};

// ============================================================================
//  便捷函数（单次读写场景，省去构造对象的样板）
// ============================================================================

QVariant readValue(RegistryRoot root,
                   const QString &subKey,
                   const QString &name,
                   const QVariant &defaultValue = QVariant(),
                   RegistryView view = RegistryView::Default);

bool writeValue(RegistryRoot root,
                const QString &subKey,
                const QString &name,
                const QVariant &value,
                RegistryValueType type = RegistryValueType::Auto,
                RegistryView view = RegistryView::Default,
                QString *errorOut = nullptr);

bool deleteValue(RegistryRoot root,
                 const QString &subKey,
                 const QString &name,
                 RegistryView view = RegistryView::Default,
                 QString *errorOut = nullptr);

bool keyExists(RegistryRoot root,
               const QString &subKey,
               RegistryView view = RegistryView::Default);

bool deleteKeyRecursively(RegistryRoot root,
                          const QString &subKey,
                          RegistryView view = RegistryView::Default,
                          QString *errorOut = nullptr);

// ============================================================================
//  变更通知
// ============================================================================

/// 广播 WM_SETTINGCHANGE，通知已运行的程序设置已变更。
/// 修改环境变量、主题、资源管理器相关设置后**必须**调用，否则需要重启才生效。
/// @param area 变更区域名，如 "Environment"、"ImmersiveColorSet"；留空表示通用变更
void broadcastSettingChange(const QString &area = QString());

/// 刷新桌面（壁纸/主题类修改后使用）
void refreshDesktop();

} // namespace WinEase::Win32
