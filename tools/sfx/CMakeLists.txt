# ============================================================================
#  winease-setup —— 自解压安装器 SFX 启动器
#
#  三条与其它目标**故意不同**的编译约定（每一条都有硬理由，别顺手改）：
#
#   1. **/MT 静态链接 CRT**：安装器的职责之一就是把 VC++ 运行库装到目标机器上，
#      它自己要是还依赖 msvcp140.dll，干净机器上双击就没反应（鸡生蛋问题）。
#   2. **不加 /utf-8 之外的任何工程级开关**（/W4 保留）：它要在**最干净的机器**上跑，
#      越少花哨越稳。
#   3. **不需要 Qt**：全程只用 Win32 + cabinet.dll + bcrypt.dll + shell32，
#      这样它才能在没装 Qt 的机器上解压出 Qt 来。
# ============================================================================

add_executable(winease-setup
    main.cpp
)

set_target_properties(winease-setup PROPERTIES
    OUTPUT_NAME "winease-setup"
    # ★ 静态 CRT：覆盖工程默认的 MultiThreadedDLL（见文件头第 1 条）
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
foreach (_cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(winease-setup PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/bin")
endforeach ()

target_compile_definitions(winease-setup PRIVATE
    UNICODE          # fdi.h 在 UNICODE 下用宽字符文件名（路径里可能有中文）
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# cabinet.lib / bcrypt.lib 等在源码里用 #pragma comment 指定，
# 这里只补 Windows SDK 的导入库目录（随工具链自带，非第三方）
target_link_libraries(winease-setup PRIVATE
    setupapi     # SetupIterateCabinetW（系统自带的 CAB 遍历/解压）
    bcrypt       # SHA-256
    shell32      # ShellExecute / SHGetKnownFolderPath / IShellLink
    ole32        # CoCreateInstance（快捷方式）
    advapi32     # 注册表
    user32
)
