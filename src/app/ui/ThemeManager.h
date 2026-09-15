#pragma once

// ============================================================================
//  ThemeManager.h —— 强制深色主题（黑底白字）
//
//  设计目标（按重要性排序）：
//    1. **配色固定不变**：不读取、不跟随操作系统的浅色/深色偏好。
//       系统切成浅色模式时，WinEase 依然是黑底白字，且不含任何浅色残留
//       （包括由 DWM 绘制的窗口标题栏）。
//    2. 黑底白字：底色纯黑 #000000，主文字纯白 #FFFFFF。
//    3. 只改这一处：所有配色集中在本文件与 src/resources/style.qss，
//       界面代码里不出现任何硬编码颜色。
//
//  "固定不变"由四道保证共同成立（缺一都会在系统浅色模式下漏出浅色）：
//    ① 控件样式固定为 Fusion
//       Windows 11 下 Qt 默认使用 windows11 样式，它会跟随系统的"应用模式"
//       偏好自动切换明暗；Fusion 完全由调色板驱动，与系统主题无关。
//       ——只改样式表和调色板而不动样式，仍会有一部分控件由 windows11 样式
//         画出浅色外观，这是最容易漏掉的一道。
//    ② 显式设置 QStyleHints::colorScheme(Dark)
//       让平台插件（原生对话框、非样式表覆盖的部分）也走深色。
//    ③ 一套完整的深色 QPalette（含 Light/Midlight/Mid/Dark/Shadow 与禁用态）
//       样式表只覆盖它写到的控件；调色板负责**兜底**所有未覆盖的部分。
//    ④ 标题栏走 Win32 → DWM 属性
//       标题栏由 DWM 绘制，样式表与调色板都影响不到，必须单独处理。
//
//  用法（main.cpp 中，创建任何窗口之前调用一次）：
//      QApplication app(argc, argv);
//      WinEase::Ui::applyForcedDarkTheme(app);
// ============================================================================

#include <QPalette>
#include <QString>

class QApplication;

namespace WinEase::Ui {

/// 应用强制深色主题（须在创建任何窗口之前调用）
void applyForcedDarkTheme(QApplication &app);

/// 强制深色调色板（导出供自检比对，界面代码无需直接使用）
QPalette forcedDarkPalette();

/// 全局样式表在 Qt 资源中的路径
QString themeStyleSheetPath();

// ============================================================================
//  主题色板（唯一定义处）
//
//  样式表里用的是同样的字面量；这里导出是为了让自检能断言"主题确实是深色"，
//  避免自检里再抄一份颜色值、改配色时两边不一致。
// ============================================================================

namespace ThemeColor {

constexpr const char *kBase = "#000000";       ///< 纯黑：主背景
constexpr const char *kSurface = "#0E0E0E";    ///< 表面一级：卡片 / 工具栏 / 导航栏
constexpr const char *kSurfaceHover = "#1A1A1A";
constexpr const char *kSurfaceActive = "#242424";
constexpr const char *kBorder = "#2A2A2A";
constexpr const char *kBorderStrong = "#3A3A3A";
constexpr const char *kText = "#FFFFFF";            ///< 纯白：主文字
constexpr const char *kTextSecondary = "#B4B4B4";
constexpr const char *kTextTertiary = "#8A8A8A";
constexpr const char *kTextDisabled = "#565656";
constexpr const char *kAccent = "#4D8DFF";          ///< 亮蓝：纯黑底上比 #2F6BFF 清晰
constexpr const char *kAccentHover = "#6BA4FF";
constexpr const char *kSuccess = "#3DD68C";
constexpr const char *kError = "#FF6B6B";
constexpr const char *kWarning = "#FFB020";
constexpr const char *kSelection = "#2A5FD9";

} // namespace ThemeColor

} // namespace WinEase::Ui
