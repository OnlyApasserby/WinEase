# ============================================================================
#  win32_smoke —— WinEaseWin32 平台能力层自检程序（控制台）
# ============================================================================
qt_add_executable(win32_smoke
    main.cpp
)

target_link_libraries(win32_smoke PRIVATE
    WinEaseWin32
    Qt6::Core
    Qt6::Gui
)

# 控制台子系统，便于直接看输出（默认为 GUI 时看不到 printf）
set_target_properties(win32_smoke PROPERTIES
    OUTPUT_NAME "win32_smoke"
    WIN32_EXECUTABLE FALSE
)
