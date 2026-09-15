#pragma once

// ============================================================================
//  WinEaseVersion.h —— 版本与全局常量
//
//  插件应使用 WINEASE_SDK_VERSION 做兼容性判断：
//      if (WinEase::SdkVersion >= QVersionNumber(0, 1, 0)) { ... }
// ============================================================================

#include <QString>
#include <QVersionNumber>

#ifndef WINEASE_SDK_VERSION
#    define WINEASE_SDK_VERSION "0.1.0"
#endif

namespace WinEase {

/// 插件 SDK 版本（与主程序版本同步发布）
inline QVersionNumber sdkVersion()
{
    static const QVersionNumber version = QVersionNumber::fromString(QStringLiteral(WINEASE_SDK_VERSION));
    return version;
}

/// 进程间通信 / 单实例 / 托盘相关的固定标识
namespace Identifiers {
inline constexpr auto kAppName        = "WinEase";
inline constexpr auto kOrganization   = "WinEase";
inline constexpr auto kDataDirName    = "WinEase";
inline constexpr auto kSingleInstance = "WinEase.SingleInstance.Instance";
} // namespace Identifiers

/// 插件元数据 IID —— 必须与插件源文件中 Q_PLUGIN_METADATA 的 IID 字符串完全一致
#define WinEase_IFeaturePlugin_iid "com.winease.WinEase.IFeaturePlugin/1.0"

} // namespace WinEase
