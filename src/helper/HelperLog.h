#pragma once

// ============================================================================
//  HelperLog.h —— 提权助手日志分类
//
//  helper 是独立可执行程序（不链接主程序的 Logging），分类自成一体系：
//  日志输出到 %APPDATA%/WinEase/logs/helper.log（见 main.cpp 的消息处理器）。
// ============================================================================

#include <QLoggingCategory>

Q_DECLARE_LOGGING_CATEGORY(lcHelper)   ///< 提权助手全部日志
