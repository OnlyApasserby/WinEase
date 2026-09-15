# ============================================================================
#  hook_smoke —— HookService（P0-2）验收测试
#
#  说明：HookServiceImpl 位于主程序内（src/app/core/），不是独立库，
#        因此这里直接把它的源文件编入测试目标。
# ============================================================================
qt_add_executable(hook_smoke
    main.cpp

    "${CMAKE_SOURCE_DIR}/src/app/core/HookServiceImpl.h"
    "${CMAKE_SOURCE_DIR}/src/app/core/HookServiceImpl.cpp"
)

target_include_directories(hook_smoke PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/app"
)

target_link_libraries(hook_smoke PRIVATE
    WinEaseSdk
    WinEaseWin32
    Qt6::Core
    Qt6::Gui
)

set_target_properties(hook_smoke PROPERTIES
    OUTPUT_NAME "hook_smoke"
    WIN32_EXECUTABLE FALSE
)
