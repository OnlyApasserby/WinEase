# ============================================================================
#  WinEaseHelper —— 提权助手（P0-4）
#
#  独立进程，requireAdministrator 清单使其以管理员权限运行（UAC 弹一次并常驻）。
#  与主程序之间用命名管道 JSON 行协议通信（见 HelperService.h）。
# ============================================================================

qt_add_executable(WinEaseHelper
    main.cpp

    HelperLog.h
    HelperService.h   HelperService.cpp
    HelperOps.h       HelperOps.cpp
    HelperSecurity.h  HelperSecurity.cpp
)

target_include_directories(WinEaseHelper PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src"
)

# WinEaseWin32 复用其注册表/进程原语（纯函数层，无全局状态，静态链接安全）
# 注意：helper 的管道服务端是 Win32 原生实现（CreateNamedPipeW，需要自管
# DACL/完整性标签），不依赖 Qt6::Network；客户端（QLocalSocket）在主程序侧。
target_link_libraries(WinEaseHelper PRIVATE
    WinEaseWin32
    Qt6::Core
)

if (WIN32)
    target_link_libraries(WinEaseHelper PRIVATE
        wintrust      # WinVerifyTrust 签名校验
        advapi32      # 令牌/特权（电源操作）
        user32        # LockWorkStation
        powrprof      # SetSuspendState 睡眠/休眠
    )
endif ()

# 版本号注入（ping 应答里带回，供自检核对）
target_compile_definitions(WinEaseHelper PRIVATE
    WINEASE_APP_VERSION="${PROJECT_VERSION}"
)

set_target_properties(WinEaseHelper PROPERTIES
    OUTPUT_NAME "WinEaseHelper"
    WIN32_EXECUTABLE TRUE   # GUI 子系统：UAC 提权拉起时不闪黑框
)

# ---------------------------------------------------------------------------
# requireAdministrator 清单：提权助手的根本。
# 用链接器 /MANIFESTUAC 而不是独立 .manifest 文件，避免与默认清单合并时冲突。
# ---------------------------------------------------------------------------
if (MSVC)
    target_link_options(WinEaseHelper PRIVATE
        "/MANIFEST:EMBED"
        "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'"
    )
endif ()
