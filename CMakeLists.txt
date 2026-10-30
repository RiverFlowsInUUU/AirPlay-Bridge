# SPDX-License-Identifier: Apache-2.0
#
# AirPlay Bridge -- Windows 系统声音 -> AirPlay 2 (HomePod)
#
# 把上游引擎（third_party/airplay2-sender-cpp）作为子目录引入，
# 再加上 WASAPI 采集、设备发现、托盘界面。
#
# 首次构建：
#   git submodule update --init --recursive
#   cd third_party/airplay2-sender-cpp && git apply ../../patches/upstream-airplay2-sender-cpp.patch && cd ../..
#   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
#   cmake --build build --config Release

cmake_minimum_required(VERSION 3.16)
# 静态链接 CRT：产物不依赖 MSVCP140.dll / VCRUNTIME140.dll，
# 用户下载 exe 双击即用，不用先装 VC++ 运行库。
# 必须在 project() 之前设置才生效。
#
# ★ AddressSanitizer 要求动态 CRT（/MD），和静态链接互斥 —— 所以留一个开关：
#     cmake -S . -B build-asan -DALMA_ASAN=ON
#   用途：把内存错误（越界读写 / use-after-free）整程序扫一遍。
#   这个项目真踩过三次越界写（MultiByteToWideChar 的 need 含 NUL），
#   所以这一类检查有实际价值，不是仪式。
#   **这个开关编出来的产物不可分发**（依赖 ASan 运行库）。
option(ALMA_ASAN "用 AddressSanitizer 构建（动态 CRT，产物不可分发）" OFF)
if(ALMA_ASAN)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "" FORCE)
else()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING "" FORCE)
endif()

# 版本号唯一来源 —— 发版只改这一行（PROJECT_VERSION）。
# 它通过下面的 target_compile_definitions(APP_VERSION=...) 注入到代码里，
# 托盘菜单「关于」自动跟着走，不会再出现"忘了改源码里的版本号"。
#
# 2026-10-24：同一个 PROJECT_VERSION 现在还喂给 exe 的 VERSIONINFO 资源
# （见 src/icons/bridge.rc），资源管理器「属性 → 详细信息」才不空白。
# 仍是单一来源 —— 资源文件里不另存一份版本号。
project(airplay_bridge VERSION 1.5.48 LANGUAGES CXX)

if(ALMA_ASAN)
    add_compile_options(/fsanitize=address /Zi)
    add_link_options(/fsanitize=address)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "AirPlay Bridge 目前只支持 Windows (WASAPI)")
endif()

find_package(Threads REQUIRED)

# /utf-8 必须对所有目标一致：源码含中文注释，MSVC 默认按系统代码页
# (936/GBK) 解码会吞掉换行，把注释和下一行声明并成一行。
if(MSVC)
    add_compile_options(/utf-8)
endif()

# ---- 上游引擎（git submodule）----
set(UPSTREAM ${CMAKE_CURRENT_SOURCE_DIR}/third_party/airplay2-sender-cpp)
if(NOT EXISTS ${UPSTREAM}/CMakeLists.txt)
    message(FATAL_ERROR
        "找不到上游引擎。请先执行:\n"
        "  git submodule update --init --recursive\n"
        "  cd third_party/airplay2-sender-cpp && git apply ../../patches/upstream-airplay2-sender-cpp.patch")
endif()

# 只建库，不建它的示例与测试
set(AIRPLAY_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(AIRPLAY_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
add_subdirectory(${UPSTREAM} ${CMAKE_BINARY_DIR}/sender)

# ---- 主程序（托盘）----
add_executable(airplay_bridge WIN32
    # 主程序与配置
    src/app/airplay_bridge.cpp
    src/app/config_store.cpp
    src/app/i18n.cpp
    # 音频：系统声音采集、音量与设备切换
    src/audio/wasapi_capture.cpp
    src/audio/sysvolume.cpp
    # 设备：局域网发现
    src/device/discovery.cpp
    # 界面：托盘、滑块窗口
    src/ui/tray.cpp
    src/ui/latency_dialog.cpp
    # 图标与资源
    src/icons/bridge.rc
)

target_include_directories(airplay_bridge PRIVATE
    src/app
    src/audio
    src/device
    src/ui
    ${UPSTREAM}/src
)

# 版本号注入：源码用 APP_VERSION 拿，值就是 project(... VERSION x.y.z)。
# 引号在这里带上 —— APP_VERSION 在代码里当字符串用。
target_compile_definitions(airplay_bridge PRIVATE
    APP_VERSION="${PROJECT_VERSION}")

# VERSIONINFO 资源要的是**分开的数字段**（FILEVERSION x,y,z,0），
# 不能直接用 "${PROJECT_VERSION}" 这个字符串。
# 所以再传三个整数 + 一个显示用字符串。
#
# ★ 必须挂在 source-file 属性上，不能进 target_compile_definitions ——
#   后者只传给 C/C++ 编译器（cl.exe），而 .rc 由 rc.exe 单独处理。
#   不传的话 bridge.rc 里那几个 #ifndef 兜底生效，资源里会是 0.0.0 ——
#   编译能过、属性页显示 0.0.0，很难发现。
set_source_files_properties(src/icons/bridge.rc PROPERTIES
    COMPILE_DEFINITIONS
        "APP_VER_MAJOR=${PROJECT_VERSION_MAJOR};APP_VER_MINOR=${PROJECT_VERSION_MINOR};APP_VER_PATCH=${PROJECT_VERSION_PATCH};APP_VER_STRING=\"${PROJECT_VERSION}\"")

target_link_libraries(airplay_bridge PRIVATE
    raop_sender
    Threads::Threads
    ole32       # COM
    uuid        # __uuidof / IID
    avrt        # 音频线程优先级
    winmm
    winhttp     # 检查更新（WinHTTP 拉 GitHub release）
    ws2_32      # socket
    bcrypt      # 加密
    shell32     # 托盘
    user32
    gdi32
)

if(MSVC)
    target_compile_options(airplay_bridge PRIVATE /W3)
endif()




# 重采样器自测（不参与发布，但必须能一键跑）
#
# ★ 接入 CTest：`ctest -C Release` 就会执行它。
#   之前只有 add_executable，既没有 enable_testing() 也没有 add_test()，
#   等于"有测试但永远不会被跑到"—— 分块塌缩那个 bug 就是这样活下来的。
#
# 关键用例是「分块不变性」：同一条输入，喂不同的块大小，输出必须逐样点相同。
# 线上 WASAPI 回调的帧数本来就不固定，所以这不只是理论要求。
add_executable(resampler_test src/audio/resampler_test.cpp)
target_include_directories(resampler_test PRIVATE src/audio)
if(MSVC)
    target_compile_options(resampler_test PRIVATE /W3)
endif()

enable_testing()
add_test(NAME resampler COMMAND resampler_test)

# 配置读写自测：纯文件 I/O，不需要声卡/网络。
# 为什么值这个：配置那条链坏过两次（v1.5.4 延迟被纠偏、v1.5.10 音量两份副本），
# 而它的核心断言是「界内的值一个都不许被改」—— 出界的拉回来是保护，
# 界内的动一下就是篡改。
add_executable(config_store_test src/app/config_store_test.cpp src/app/config_store.cpp)
target_include_directories(config_store_test PRIVATE src/app)
if(MSVC)
    target_compile_options(config_store_test PRIVATE /W3)
endif()
add_test(NAME config_store COMMAND config_store_test)

# 版本比较 + JSON 取值：同样是纯函数，几毫秒跑完。
# 这两类函数最容易错、也最容易被漏测（P3-3）。
add_executable(version_test src/app/version_test.cpp src/app/config_store.cpp)
target_include_directories(version_test PRIVATE src/app)
if(MSVC)
    target_compile_options(version_test PRIVATE /W3)
endif()
add_test(NAME version COMMAND version_test)

# 采集失败分类：纯函数，几毫秒跑完。
# 它把「运行中失效优先于启动失败」这个优先级钉死 —— 那正是用户提示
# 会不会说错的关键（2026-10-19 第六轮复核 P2-A）。
add_executable(capture_health_test src/audio/capture_health_test.cpp)
target_include_directories(capture_health_test PRIVATE src/audio)
if(MSVC)
    target_compile_options(capture_health_test PRIVATE /W3)
endif()
add_test(NAME capture_health COMMAND capture_health_test)

# 采集看门狗：判定链 + 「只触发一次」（2026-10-19 第七轮复核要求补的回归）。
add_executable(capture_watchdog_test src/audio/capture_watchdog_test.cpp)
target_include_directories(capture_watchdog_test PRIVATE src/audio)
if(MSVC)
    target_compile_options(capture_watchdog_test PRIVATE /W3)
endif()
add_test(NAME capture_watchdog COMMAND capture_watchdog_test)

# 托盘提示文案：纯函数拼装，钉住「长设备名不能把 IP 挤掉」
# 与「名字为空时 IP 不重复出现」这两条（2026-10-20）。
add_executable(tooltip_text_test src/app/tooltip_text_test.cpp)
target_include_directories(tooltip_text_test PRIVATE src/app)
if(MSVC)
    target_compile_options(tooltip_text_test PRIVATE /W3 /utf-8)
endif()
add_test(NAME tooltip_text COMMAND tooltip_text_test)
