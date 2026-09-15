# ============================================================================
#  P2-09 放大镜增强（display.magnifier）
#
#  没有额外链接需求：Magnification.lib 由平台层 WinEaseWin32 统一带上
#  （插件只使用 WinEase::Win32::Magnifier 这个封装，不直接碰 Magnification API）。
# ============================================================================

winease_add_plugin(display_magnifier
    CLASS_NAME MagnifierPlugin
    SOURCES
        magnifier_plugin.h
        magnifier_plugin.cpp
        magnifier_plugin.json
)
