#pragma once

// ============================================================================
//  Logging.h —— 日志系统
//
//  * 使用 QLoggingCategory 分类，便于按模块过滤
//  * 输出到 %APPDATA%/WinEase/logs/winease-YYYY-MM-DD.log
//  * 通过 install() 安装自定义消息处理器；插件通过 PluginServices::log()
//    间接写入同一份日志，无需重复安装
//
//  用法：
//      qCInfo(lcPlugin) << "窗口置顶已启用";
//      qCWarning(lcHotkey) << "快捷键冲突:" << text;
// ============================================================================

#include <QLoggingCategory>
#include <QString>

namespace WinEase::Logging {

// 日志分类
Q_DECLARE_LOGGING_CATEGORY(lcApp)    ///< 主程序
Q_DECLARE_LOGGING_CATEGORY(lcConfig) ///< 配置
Q_DECLARE_LOGGING_CATEGORY(lcPlugin) ///< 插件加载与生命周期
Q_DECLARE_LOGGING_CATEGORY(lcHotkey) ///< 全局快捷键
Q_DECLARE_LOGGING_CATEGORY(lcTray)   ///< 托盘
Q_DECLARE_LOGGING_CATEGORY(lcUi)     ///< 界面

/// 安装消息处理器并开始写日志文件
/// @param logDirectory 日志目录，不存在会自动创建
/// @param minLevel     写入文件的最低级别，默认 Debug
bool install(const QString &logDirectory, QtMsgType minLevel = QtDebugMsg);

/// 关闭日志文件并还原默认处理器
void uninstall();

/// 当前日志文件绝对路径（未安装时为空）
QString currentLogFilePath();

/// 当前日志目录
QString logDirectory();

/// 动态调整最低日志级别
void setMinimumLevel(QtMsgType level);
QtMsgType minimumLevel();

/// 清理超过 keepCount 天的历史日志
void pruneOldLogs(int keepDays = 7);

/// 把插件日志级别映射为 Qt 消息类型（供 AppContext 使用）
QtMsgType toQtMsgType(int pluginLogLevel);

} // namespace WinEase::Logging

// ---------------------------------------------------------------------------
//  便捷别名：使 WinEase 命名空间内的代码可直接写 qCInfo(lcPlugin) << ...
//  （否则每次都要写全限定名 WinEase::Logging::lcPlugin）
// ---------------------------------------------------------------------------
namespace WinEase {

using Logging::lcApp;
using Logging::lcConfig;
using Logging::lcHotkey;
using Logging::lcPlugin;
using Logging::lcTray;
using Logging::lcUi;

} // namespace WinEase
