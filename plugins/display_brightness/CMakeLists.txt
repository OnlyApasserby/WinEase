# ============================================================================
#  P2-08 亮度色温护眼（display.brightness）
#
#  没有额外链接需求：
#    * 亮度通路（DDC/CI 的 dxva2、内置屏的 wbemuuid）都由平台层 WinEaseWin32 统一带上
#    * 色温走 gdi32 的 SetDeviceGammaRamp，同样在平台层
#  插件只使用 WinEase::Win32::DisplayControl 里的封装，不直接碰这些 API。
# ============================================================================

winease_add_plugin(display_brightness
    CLASS_NAME BrightnessPlugin
    SOURCES
        brightness_plugin.h
        brightness_plugin.cpp
        brightness_plugin.json
)
