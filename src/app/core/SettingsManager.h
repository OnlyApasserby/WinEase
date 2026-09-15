#pragma once

// ============================================================================
//  SettingsManager.h —— 统一配置持久化（QSettings 封装）
//
//  存储位置：%APPDATA%/WinEase/config.ini
//  格式    ：INI（QSettings::IniFormat，Qt6 固定 UTF-8 编码，中文无需特殊处理）
//
//  配置结构约定：
//      [General]             全局设置（开机自启、日志级别、关闭行为…）
//      [Plugins/<pluginId>]  每个插件的独立 section，键名由插件自定义
//      [Hotkeys]             hotkeyId -> "Ctrl+Alt+P"
//
//  两套读写接口：
//    * 模板方法（推荐，类型安全）：
//          int  level = settings.value("General/logLevel", 0);
//          settings.setValue("General/logLevel", 3);
//          bool dark = settings.pluginValue<bool>("dev.template", "darkMode", false);
//    * 原始接口（QVariant，用于插件通过 PluginServices 转发）：
//          rawValue() / setRawValue() / rawPluginValue() / setRawPluginValue()
//
//  线程安全：内部用 QMutex 串行化；但仍建议只在主线程读写配置。
// ============================================================================

#include <QMutex>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <memory>
#include <type_traits>

namespace WinEase {

class SettingsManager : public QObject
{
    Q_OBJECT

public:
    /// 全局单例（进程内唯一；插件通过 PluginServices 间接访问）
    static SettingsManager &instance();

    SettingsManager(const SettingsManager &) = delete;
    SettingsManager &operator=(const SettingsManager &) = delete;

    /// 创建数据目录并打开配置文件；失败返回 false（调用方仍可按默认值运行）
    bool initialize();

    // ------------------------------------------------------------------------
    //  路径
    // ------------------------------------------------------------------------
    /// %APPDATA%/WinEase
    QString dataDirectory() const;
    /// %APPDATA%/WinEase/config.ini
    QString configFilePath() const;
    /// %APPDATA%/WinEase/logs
    QString logDirectory() const;
    /// %APPDATA%/WinEase/plugins（用户级插件目录）
    QString userPluginDirectory() const;

    // ------------------------------------------------------------------------
    //  全局配置 —— 模板接口
    // ------------------------------------------------------------------------
    /// 读取配置项：键不存在或类型不可转换时返回 defaultValue
    template <typename T>
    T value(const QString &key, const T &defaultValue = T()) const
    {
        const QVariant stored = rawValue(key);
        if (!stored.isValid() || stored.isNull()) {
            return defaultValue;
        }
        if (stored.metaType() == QMetaType::fromType<T>()) {
            return stored.value<T>();
        }
        if (stored.canConvert<T>()) {
            return stored.value<T>();
        }
        return defaultValue;
    }

    /// 写入配置项并立即落盘
    template <typename T>
    void setValue(const QString &key, const T &value)
    {
        setRawValue(key, QVariant::fromValue(value));
    }

    /// 字符串字面量便捷重载（避免把 const char* 存成指针类型）
    void setValue(const QString &key, const char *value)
    {
        setRawValue(key, QString::fromUtf8(value));
    }

    // ------------------------------------------------------------------------
    //  插件配置 —— 模板接口（自动落到 [Plugins/<pluginId>] 段）
    // ------------------------------------------------------------------------
    template <typename T>
    T pluginValue(const QString &pluginId, const QString &key, const T &defaultValue = T()) const
    {
        return value(pluginSection(pluginId) + QLatin1Char('/') + key, defaultValue);
    }

    template <typename T>
    void setPluginValue(const QString &pluginId, const QString &key, const T &value)
    {
        setValue(pluginSection(pluginId) + QLatin1Char('/') + key, value);
    }

    // ------------------------------------------------------------------------
    //  原始 QVariant 接口（插件服务转发 / 泛型场景）
    // ------------------------------------------------------------------------
    QVariant rawValue(const QString &key, const QVariant &defaultValue = QVariant()) const;
    void setRawValue(const QString &key, const QVariant &value);
    QVariant rawPluginValue(const QString &pluginId,
                            const QString &key,
                            const QVariant &defaultValue = QVariant()) const;
    void setRawPluginValue(const QString &pluginId, const QString &key, const QVariant &value);

    bool contains(const QString &key) const;
    void remove(const QString &key);
    /// 清空某个插件的全部配置
    void clearPluginSection(const QString &pluginId);

    // ------------------------------------------------------------------------
    //  插件启用状态
    // ------------------------------------------------------------------------
    bool isPluginEnabled(const QString &pluginId, bool defaultValue = true) const;
    void setPluginEnabled(const QString &pluginId, bool enabled);
    QStringList knownPluginIds() const;

    // ------------------------------------------------------------------------
    //  全局快捷键
    // ------------------------------------------------------------------------
    QString hotkeyText(const QString &hotkeyId) const;
    void setHotkeyText(const QString &hotkeyId, const QString &sequenceText);
    QStringList knownHotkeyIds() const;
    void clearHotkeys();

    /// 提交落盘（QSettings 已即时 sync，本方法用于显式兜底）
    void sync();

Q_SIGNALS:
    /// 任意配置项发生变化（key 为完整键名）
    void valueChanged(const QString &key);
    /// 插件启用状态变化
    void pluginEnabledChanged(const QString &pluginId, bool enabled);

private:
    SettingsManager();
    ~SettingsManager() override;

    static QString pluginSection(const QString &pluginId);
    static QString enabledKey(const QString &pluginId);
    void ensureDirectories() const;

    std::unique_ptr<QSettings> m_settings;
    QString m_dataDirectory;
    mutable QMutex m_mutex;
};

} // namespace WinEase
