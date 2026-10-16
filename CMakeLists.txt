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
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING "" FORCE)

# 版本号唯一来源 —— 发版只改这一行（PROJECT_VERSION）。
# 它通过下面的 target_compile_definitions(APP_VERSION=...) 注入到代码里，
# 托盘菜单「关于」自动跟着走，不会再出现"忘了改源码里的版本号"。
project(airplay_bridge VERSION 1.2.60 LANGUAGES CXX)

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




# 重采样器自测（不参与发布）
add_executable(resampler_test src/audio/resampler_test.cpp)
target_include_directories(resampler_test PRIVATE src/audio)
if(MSVC)
    target_compile_options(resampler_test PRIVATE /W3)
endif()
