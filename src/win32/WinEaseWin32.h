#pragma once

// ============================================================================
//  WinEaseWin32.h —— 平台能力层汇总头
//
//  插件一般不需要全量包含，按需包含具体模块头即可：
//      #include "win32/WindowUtils.h"
//      #include "win32/RegistryUtils.h"
//
//  坐标约定（务必遵守，否则高 DPI 下必错）：
//      本层所有坐标/尺寸均为 **物理像素** 且使用 **屏幕坐标**。
//      Qt 的 QWidget/QScreen 坐标是 **逻辑像素**，
//      两者换算使用 WindowUtils 中的 scaleFactorForWindow() / toLogical() / toPhysical()。
// ============================================================================

#include "win32/AudioSessions.h"
#include "win32/ComApartment.h"
#include "win32/CoreAudio.h"
#include "win32/DesktopWallpaper.h"
#include "win32/DisplayControl.h"
#include "win32/InputUtils.h"
#include "win32/NetUtils.h"
#include "win32/ProcessUtils.h"
#include "win32/RegistryUtils.h"
#include "win32/ScreenCapture.h"
#include "win32/ShellUtils.h"
#include "win32/SystemInfo.h"
#include "win32/Win32Error.h"
#include "win32/WindowUtils.h"
#include "win32/WindowTarget.h"
#include "win32/WinRtSupport.h"
