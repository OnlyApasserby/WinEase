# ============================================================================
#  WinEaseWin32 —— Windows 平台能力层（静态库）
#
#  设计原则（对应 ROADMAP P0-1 与 ENV-SETUP §3.2）：
#    1. 只放**可复用平台原语**，不放任何功能逻辑（功能逻辑属于插件）
#    2. 纯函数式、**无全局状态**：本库会被静态链接进主程序与每一个插件 DLL，
#       任何全局状态都会在每个模块里各持一份，必然出错
#    3. 头文件里不泄漏 WinRT / COM 具体类型（除必要的 HANDLE 等），
#       便于将来替换实现而不影响插件
#    4. 全部坐标一律使用**物理像素 + 屏幕坐标**，与 Qt 逻辑像素的换算显式提供
#
#  链接方：主程序 WinEase、所有功能插件
# ============================================================================

qt_add_library(WinEaseWin32 STATIC
    WinEaseWin32.h

    Win32Error.h        Win32Error.cpp
    ComApartment.h      ComApartment.cpp
    WindowUtils.h       WindowUtils.cpp
    WindowTarget.h      WindowTarget.cpp   # "操作哪个窗口"的统一解析（窗口类插件共用）
    RegistryUtils.h     RegistryUtils.cpp
    ProcessUtils.h      ProcessUtils.cpp
    InputUtils.h        InputUtils.cpp     # 合成按键（取选中文本 / 就地替换，P1-09/10/11）
    ClipboardPrivacy.h  ClipboardPrivacy.cpp # 剪贴板来源标记识别（"不记录"闸门，P2-04）
    NetUtils.h          NetUtils.cpp       # 网络端点枚举（端口占用查看，P1-12）
    DesktopWallpaper.h  DesktopWallpaper.cpp # 壁纸读写（P1-14；IDesktopWallpaper + SPI 兜底）
    ScreenCapture.h     ScreenCapture.cpp
    ShellUtils.h        ShellUtils.cpp
    SystemInfo.h        SystemInfo.cpp
    CoreAudio.h         CoreAudio.cpp
    AudioSessions.h     AudioSessions.cpp # 音频会话（每应用音量/静音，P3-09；与端点主音量并列）
    MediaSessions.h     MediaSessions.cpp # 系统媒体会话（P3-11；读曲目/进度/封面 + 精确控制 + 媒体键）
    DisplayControl.h    DisplayControl.cpp
    PowerUtils.h        PowerUtils.cpp  # 锁定会话 / 取消挂起关机（P2-06：撤销通道不能依赖助手）
    StorageDevices.h    StorageDevices.cpp # 存储设备枚举与安全弹出（P3-14；**判据不用 DRIVE_ 类型**）
    RestartManager.h    RestartManager.cpp # 谁占用了这个文件（P3-02；免提权只读，只查不动）
    VirtualDesktop.h    VirtualDesktop.cpp # 虚拟桌面查询/搬迁（P3-01；公开接口 IVirtualDesktopManager）
    Magnifier.h         Magnifier.cpp   # 原生放大镜窗口（Magnification API，P2-09）
    WinRtSupport.h      WinRtSupport.cpp
)

target_include_directories(WinEaseWin32
    PUBLIC "${CMAKE_SOURCE_DIR}/src"
)

# Qt 只用于 QRect/QPoint/QImage/QColor 等值类型，不做 GUI 依赖扩张
target_link_libraries(WinEaseWin32
    PUBLIC
        Qt6::Core
        Qt6::Gui
        windowsapp.lib   # C++/WinRT（随 Windows SDK 提供，非第三方）
    PRIVATE
        user32      # 窗口、显示器、消息、电源状态
        gdi32       # BitBlt 抓屏、gamma ramp
        advapi32    # 注册表、令牌
        shell32     # Shell 缩略图、文件操作
        dwmapi      # DWMWA_CLOAKED 等 DWM 属性
        ole32       # COM
        oleaut32    # COM 变体
        setupapi    # 枚举物理磁盘（温度）
        cfgmgr32    # CM_Request_Device_Eject（P3-14 安全弹出）
        rstrtmgr    # Restart Manager（P3-02 查询"谁占用了这个文件"，系统自带）
        iphlpapi    # GetIfTable2 网络吞吐
        pdh         # GPU 利用率计数器
        dxva2       # DDC/CI 显示器亮度色温
        wbemuuid    # WMI：笔记本内置屏亮度（root\WMI，DDC/CI 到不了的面板）
        magnification # 原生放大镜窗口（P2-09：把放大交给系统合成器，CPU 基本不涨）
        version     # 文件版本信息（后续用）
)

set_target_properties(WinEaseWin32 PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_compile_definitions(WinEaseWin32 PUBLIC
    WINEASE_WIN32_LAYER=1
    WINEASE_HAS_WINRT=1
    WINRT_LEAN_AND_MEAN   # 精简 C++/WinRT 头文件体积
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)
