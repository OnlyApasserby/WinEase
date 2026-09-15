# ============================================================================
#  elevation_smoke —— WinEaseHelper 提权助手验收测试（P0-4）
#
#  与 theme_smoke 同一思路：把**真实的 ElevationClient**（主程序源文件）编入
#  测试目标，测的是主程序真正使用的代码路径，而不是在测试里另抄一份客户端。
#
#  运行方式：
#      .\build\bin\elevation_smoke.exe              # helper 已在运行时做全量验收
#      .\build\bin\elevation_smoke.exe --with-uac   # 允许弹出 UAC 拉起 helper
#  退出码 0 表示全部通过。
# ============================================================================

qt_add_executable(elevation_smoke
    main.cpp

    "${CMAKE_SOURCE_DIR}/src/app/core/ElevationClient.h"
    "${CMAKE_SOURCE_DIR}/src/app/core/ElevationClient.cpp"
)

target_include_directories(elevation_smoke PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/app"
)

target_link_libraries(elevation_smoke PRIVATE
    WinEaseWin32
    Qt6::Core
    Qt6::Network      # QLocalSocket（helper 通信 + 伪造调用方）
)

if (WIN32)
    target_link_libraries(elevation_smoke PRIVATE
        shell32       # 对照项：直接读注册表
        advapi32
    )
endif ()

set_target_properties(elevation_smoke PROPERTIES
    OUTPUT_NAME "elevation_smoke"
    WIN32_EXECUTABLE FALSE
)
