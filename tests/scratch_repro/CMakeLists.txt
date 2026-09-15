# ============================================================================
#  scratch_repro —— **临时**排障目标（复现 display.focus_highlight 在 shutdown()
#  阶段抛 C++ 异常被隔离的问题）。定案后连同 tests/scratch_repro.cpp 一起删除。
# ============================================================================

qt_add_executable(scratch_repro
    "${CMAKE_SOURCE_DIR}/tests/scratch_repro/main.cpp"

    "${CMAKE_SOURCE_DIR}/src/app/core/PluginGuard.h"
    "${CMAKE_SOURCE_DIR}/src/app/core/PluginGuard.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/core/PluginManager.h"
    "${CMAKE_SOURCE_DIR}/src/app/core/PluginManager.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/core/SettingsManager.h"
    "${CMAKE_SOURCE_DIR}/src/app/core/SettingsManager.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/core/AdminHelper.h"
    "${CMAKE_SOURCE_DIR}/src/app/core/AdminHelper.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/core/Logging.h"
    "${CMAKE_SOURCE_DIR}/src/app/core/Logging.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/core/GlobalHotkeyManager.h"
    "${CMAKE_SOURCE_DIR}/src/app/core/GlobalHotkeyManager.cpp"
)

target_include_directories(scratch_repro PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/app"
)

target_link_libraries(scratch_repro PRIVATE
    WinEaseSdk
    WinEaseWin32
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

if (WIN32)
    target_link_libraries(scratch_repro PRIVATE shell32 advapi32)
endif ()

# 要加载 build/bin/plugins 里的真实插件，因此必须等插件全部构建完
get_property(_winease_plugin_targets GLOBAL PROPERTY WINEASE_PLUGIN_TARGETS)
if (_winease_plugin_targets)
    add_dependencies(scratch_repro ${_winease_plugin_targets})
endif ()

set_target_properties(scratch_repro PROPERTIES
    OUTPUT_NAME "scratch_repro"
    WIN32_EXECUTABLE FALSE
)
