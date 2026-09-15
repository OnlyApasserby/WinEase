#pragma once

// ============================================================================
//  FeatureCategory.h —— 功能分类定义
//
//  分类与主界面左侧导航栏一一对应；插件通过 IFeaturePlugin::category()
//  声明自己的归属，主程序据此分组展示。
// ============================================================================

#include <QIcon>
#include <QList>
#include <QMetaType>
#include <QString>

namespace WinEase {

/// 功能分类（枚举顺序即导航栏展示顺序）
enum class FeatureCategory {
    WindowManagement = 0, ///< 窗口管理
    FileEnhancement,      ///< 文件增强
    InputEfficiency,      ///< 输入效率
    SystemMonitor,        ///< 系统监控
    DisplayAssist,        ///< 显示辅助
    Media,                ///< 媒体
    Launcher,             ///< 启动器
    Security,             ///< 安全
    Development,          ///< 开发
    Personalization,      ///< 个性化
    Unknown               ///< 未知 / 未分类（内部使用，也代表"全部"）
};

namespace Category {

/// 全部分类（不含 Unknown），顺序即导航顺序
QList<FeatureCategory> all();

/// 中文显示名，例如"窗口管理"
QString displayName(FeatureCategory category);

/// 一句话说明，用于导航栏提示
QString description(FeatureCategory category);

/// 资源路径，例如 ":/winease/icons/window.svg"
QString iconPath(FeatureCategory category);

/// 分类图标
QIcon icon(FeatureCategory category);

/// 稳定键名（用于配置文件、插件元数据），例如 "window"
QString key(FeatureCategory category);

/// 由键名反查分类，未知返回 FeatureCategory::Unknown
FeatureCategory fromKey(const QString &key);

} // namespace Category
} // namespace WinEase

Q_DECLARE_METATYPE(WinEase::FeatureCategory)
