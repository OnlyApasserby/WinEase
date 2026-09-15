#include "core/SettingsManager.h"

#include "core/Logging.h"
#include "sdk/WinEaseVersion.h"

#include <QDateTime>
#include <QDir>
#include <QMutexLocker>
#include <QProcessEnvironment>
#include <QStandardPaths>

namespace WinEase {

namespace {

/// 插件启用状态在 [Plugins/<id>] 段中的键名
constexpr auto kEnabledKey = "enabled";
constexpr auto kHotkeysSection = "Hotkeys";
constexpr auto kPluginsSectionRoot = "Plugins";

/// 解析数据目录：%APPDATA%/WinEase
/// 优先使用环境变量 %APPDATA%（Roaming），保证与用户预期路径一致；
/// 异常情况下回退到 Qt 标准路径。
QString resolveDataDirectory()
{
    QString base = QProcessEnvironment::systemEnvironment().value(QStringLiteral("APPDATA"));
    if (base.isEmpty()) {
        base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    }
    if (base.isEmpty()) {
        base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    }
    if (base.isEmpty()) {
        base = QDir::homePath() + QStringLiteral("/.winease");
    }
    return QDir::cleanPath(base + QLatin1Char('/') + QString::fromLatin1(Identifiers::kDataDirName));
}

} // namespace

// ---------------------------------------------------------------------------
//  单例与初始化
// ---------------------------------------------------------------------------

SettingsManager &SettingsManager::instance()
{
    static SettingsManager manager;
    return manager;
}

SettingsManager::SettingsManager()
    : QObject(nullptr)
    , m_dataDirectory(resolveDataDirectory())
{
    ensureDirectories();
}

SettingsManager::~SettingsManager()
{
    if (m_settings) {
        m_settings->sync();
    }
}

void SettingsManager::ensureDirectories() const
{
    QDir dir;
    dir.mkpath(m_dataDirectory);
    dir.mkpath(logDirectory());
}

bool SettingsManager::initialize()
{
    QMutexLocker locker(&m_mutex);

    ensureDirectories();

    m_settings = std::make_unique<QSettings>(configFilePath(), QSettings::IniFormat);
    if (m_settings->status() != QSettings::NoError) {
        qCWarning(lcConfig) << "配置文件打开失败, status =" << m_settings->status()
                            << "path =" << configFilePath();
        return false;
    }

    if (!m_settings->contains(QStringLiteral("General/version"))) {
        m_settings->setValue(QStringLiteral("General/version"), QStringLiteral(WINEASE_SDK_VERSION));
        m_settings->setValue(QStringLiteral("General/firstRunAt"), QDateTime::currentDateTime());
    }
    m_settings->sync();

    qCInfo(lcConfig) << "配置文件就绪:" << configFilePath();
    return true;
}

// ---------------------------------------------------------------------------
//  路径
// ---------------------------------------------------------------------------

QString SettingsManager::dataDirectory() const
{
    return m_dataDirectory;
}

QString SettingsManager::configFilePath() const
{
    return QDir(m_dataDirectory).filePath(QStringLiteral("config.ini"));
}

QString SettingsManager::logDirectory() const
{
    return QDir(m_dataDirectory).filePath(QStringLiteral("logs"));
}

QString SettingsManager::userPluginDirectory() const
{
    return QDir(m_dataDirectory).filePath(QStringLiteral("plugins"));
}

// ---------------------------------------------------------------------------
//  原始 QVariant 接口
// ---------------------------------------------------------------------------

QVariant SettingsManager::rawValue(const QString &key, const QVariant &defaultValue) const
{
    QMutexLocker locker(&m_mutex);
    if (!m_settings) {
        return defaultValue;
    }
    return m_settings->value(key, defaultValue);
}

void SettingsManager::setRawValue(const QString &key, const QVariant &value)
{
    {
        QMutexLocker locker(&m_mutex);
        if (!m_settings) {
            return;
        }
        m_settings->setValue(key, value);
        m_settings->sync();
    }
    Q_EMIT valueChanged(key);
}

QVariant SettingsManager::rawPluginValue(const QString &pluginId,
                                         const QString &key,
                                         const QVariant &defaultValue) const
{
    return rawValue(pluginSection(pluginId) + QLatin1Char('/') + key, defaultValue);
}

void SettingsManager::setRawPluginValue(const QString &pluginId,
                                        const QString &key,
                                        const QVariant &value)
{
    setRawValue(pluginSection(pluginId) + QLatin1Char('/') + key, value);
}

bool SettingsManager::contains(const QString &key) const
{
    QMutexLocker locker(&m_mutex);
    return m_settings && m_settings->contains(key);
}

void SettingsManager::remove(const QString &key)
{
    {
        QMutexLocker locker(&m_mutex);
        if (!m_settings) {
            return;
        }
        m_settings->remove(key);
        m_settings->sync();
    }
    Q_EMIT valueChanged(key);
}

// ---------------------------------------------------------------------------
//  插件配置
// ---------------------------------------------------------------------------

QString SettingsManager::pluginSection(const QString &pluginId)
{
    return QStringLiteral("%1/%2").arg(QString::fromLatin1(kPluginsSectionRoot), pluginId);
}

void SettingsManager::clearPluginSection(const QString &pluginId)
{
    {
        QMutexLocker locker(&m_mutex);
        if (!m_settings) {
            return;
        }
        m_settings->beginGroup(pluginSection(pluginId));
        m_settings->remove(QString()); // 移除该 group 下的全部键
        m_settings->endGroup();
        m_settings->sync();
    }
    Q_EMIT valueChanged(pluginSection(pluginId));
}

// ---------------------------------------------------------------------------
//  插件启用状态
// ---------------------------------------------------------------------------

QString SettingsManager::enabledKey(const QString &pluginId)
{
    return pluginSection(pluginId) + QLatin1Char('/') + QString::fromLatin1(kEnabledKey);
}

bool SettingsManager::isPluginEnabled(const QString &pluginId, bool defaultValue) const
{
    // 模板接口：T 由 defaultValue 推导为 bool，QVariant 会自动完成类型转换
    return value(enabledKey(pluginId), defaultValue);
}

void SettingsManager::setPluginEnabled(const QString &pluginId, bool enabled)
{
    setValue(enabledKey(pluginId), enabled);
    Q_EMIT pluginEnabledChanged(pluginId, enabled);
}

QStringList SettingsManager::knownPluginIds() const
{
    QMutexLocker locker(&m_mutex);
    if (!m_settings) {
        return {};
    }

    m_settings->beginGroup(QString::fromLatin1(kPluginsSectionRoot));
    const QStringList groups = m_settings->childGroups();
    m_settings->endGroup();
    return groups;
}

// ---------------------------------------------------------------------------
//  全局快捷键
// ---------------------------------------------------------------------------

QString SettingsManager::hotkeyText(const QString &hotkeyId) const
{
    return rawValue(QStringLiteral("%1/%2").arg(QString::fromLatin1(kHotkeysSection), hotkeyId))
        .toString();
}

void SettingsManager::setHotkeyText(const QString &hotkeyId, const QString &sequenceText)
{
    setValue(QStringLiteral("%1/%2").arg(QString::fromLatin1(kHotkeysSection), hotkeyId), sequenceText);
}

QStringList SettingsManager::knownHotkeyIds() const
{
    QMutexLocker locker(&m_mutex);
    if (!m_settings) {
        return {};
    }
    m_settings->beginGroup(QString::fromLatin1(kHotkeysSection));
    const QStringList keys = m_settings->childKeys();
    m_settings->endGroup();
    return keys;
}

void SettingsManager::clearHotkeys()
{
    {
        QMutexLocker locker(&m_mutex);
        if (!m_settings) {
            return;
        }
        m_settings->beginGroup(QString::fromLatin1(kHotkeysSection));
        m_settings->remove(QString());
        m_settings->endGroup();
        m_settings->sync();
    }
    Q_EMIT valueChanged(QString::fromLatin1(kHotkeysSection));
}

void SettingsManager::sync()
{
    QMutexLocker locker(&m_mutex);
    if (m_settings) {
        m_settings->sync();
    }
}

} // namespace WinEase
