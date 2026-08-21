cmake_minimum_required(VERSION 3.16)
project(esp_yun_sim C CXX)

# PC 上的屏幕仿真器：编译仓库里真实的屏幕代码（main/display/screen/...），
# ESP-IDF 的部分由 sim/stubs 顶替，画面用 LVGL 自带的 lodepng 存成 PNG。
# 除了 cmake + gcc 之外没有别的系统依赖（SDL、libjpeg 都不需要）。

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(LVGL_DIR ${REPO_ROOT}/managed_components/lvgl__lvgl)
set(FONTS_DIR ${REPO_ROOT}/managed_components/78__xiaozhi-fonts)

if(NOT EXISTS ${LVGL_DIR}/lvgl.h)
  message(FATAL_ERROR "找不到 LVGL：${LVGL_DIR}。先在仓库根目录跑一次 idf.py build 把组件拉下来。")
endif()

# 仿真里的「SD 卡」和「资源分区」，都是普通目录
set(SIM_SDCARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sdcard" CACHE PATH "仿真 SD 卡目录")
set(SIM_ASSETS_DIR "${REPO_ROOT}/main/xingzhi-assets" CACHE PATH "图片资源目录（A: 盘）")
set(SIM_NVS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.nvs" CACHE PATH "设置持久化目录（NVS 替身）")

# --- LVGL ------------------------------------------------------------------
set(LV_BUILD_CONF_DIR ${CMAKE_CURRENT_SOURCE_DIR} CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory(${LVGL_DIR} ${CMAKE_BINARY_DIR}/lvgl EXCLUDE_FROM_ALL)

# --- 可选：SDL2 实时窗口 ----------------------------------------------------
# 装了 libsdl2-dev 就自动多一个 --gui 模式（鼠标当手指）。没装照样能用，
# 只是只能出截图。sudo apt install libsdl2-dev 之后重新 cmake 即可。
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(SDL2 QUIET sdl2)
endif()
if(SDL2_FOUND)
  message(STATUS "找到 SDL2，启用 --gui 实时窗口模式")
  target_compile_definitions(lvgl PUBLIC SIM_HAS_SDL=1)
  target_include_directories(lvgl PUBLIC ${SDL2_INCLUDE_DIRS})
  target_link_libraries(lvgl PUBLIC ${SDL2_LIBRARIES})
  target_link_directories(lvgl PUBLIC ${SDL2_LIBRARY_DIRS})
else()
  message(STATUS "没有 SDL2，只用无窗口截图模式（想要实时窗口就装 libsdl2-dev）")
endif()

# --- 真实屏幕代码 ------------------------------------------------------------
# 加新屏就往这里添 .cc（顺带在 sim/src/sim_screens.cc 注册）
set(SCREEN_SOURCES
    ${REPO_ROOT}/main/display/screen/screen_util.cc
    ${REPO_ROOT}/main/display/screen/album_screen/album_screen.cc
    ${REPO_ROOT}/main/display/screen/clock_screen/clock_screen.cc
    ${REPO_ROOT}/main/display/screen/settings_screen/settings_screen.cc
    ${REPO_ROOT}/main/display/screen/standby_screen/standby_screen.cc
    ${REPO_ROOT}/main/display/screen/weather_screen/weather_icon_map.cc
    ${REPO_ROOT}/main/i18n/i18n.cc
)

# 设备上提示音是 EMBED_FILES 塞进固件的，lang_config.h 直接引用
# _binary_xxx_ogg_start/end 这些链接器符号。仿真不放音，但符号得存在，
# 于是从那个头文件里把符号名抓出来自动生成空数据 —— 以后加提示音不用改这里。
set(SOUND_STUB ${CMAKE_BINARY_DIR}/sim_embedded_sounds.cc)
file(STRINGS ${REPO_ROOT}/main/assets/lang_config.h SOUND_LINES
     REGEX "asm\\(\"_binary_[A-Za-z0-9_]+\"\\)")
set(SOUND_BODY "// 由 sim/CMakeLists.txt 从 main/assets/lang_config.h 生成，别手改。\n")
set(SOUND_SYMS "")
foreach(line IN LISTS SOUND_LINES)
  string(REGEX MATCH "_binary_[A-Za-z0-9_]+" sym "${line}")
  if(sym)
    list(APPEND SOUND_SYMS "${sym}")
  endif()
endforeach()
list(REMOVE_DUPLICATES SOUND_SYMS)
foreach(sym IN LISTS SOUND_SYMS)
  string(APPEND SOUND_BODY
         "extern \"C\" const char sim_${sym}[1] asm(\"${sym}\") = {0};\n")
endforeach()
file(WRITE ${SOUND_STUB} "${SOUND_BODY}")
list(LENGTH SOUND_SYMS SOUND_SYM_COUNT)
message(STATUS "提示音符号占位：${SOUND_SYM_COUNT} 个")

set(SIM_SOURCES
    src/main.cc
    src/sim_display.cc
    src/sim_input.cc
    src/sim_assets.cc
    src/sim_screens.cc
    src/sim_ramimg.cc
    src/sim_home_stub.cc
    stubs/sim_esp_shims.cc
    stubs/sim_freertos.cc
    stubs/sim_settings.cc
    stubs/sim_board.cc
    stubs/sim_jpeg.cc
    ${SOUND_STUB}
)

add_executable(sim ${SIM_SOURCES} ${SCREEN_SOURCES})

target_include_directories(sim PRIVATE
    # stubs 必须排在最前面，这样 esp_log.h / settings.h / config.h 这些
    # 用的是仿真版而不是仓库里的设备版。
    stubs
    src
    ${REPO_ROOT}/main
    ${REPO_ROOT}/main/i18n
    ${REPO_ROOT}/main/display
    ${REPO_ROOT}/main/display/screen
    ${REPO_ROOT}/main/display/screen/weather_screen
    ${REPO_ROOT}/managed_components/espressif__esp_new_jpeg/include
    ${FONTS_DIR}/include
    ${LVGL_DIR}
)

target_compile_definitions(sim PRIVATE
    ESP_YUN_SIM=1
    SIM_SDCARD_DIR="${SIM_SDCARD_DIR}"
    SIM_ASSETS_DIR="${SIM_ASSETS_DIR}"
    SIM_NVS_DIR="${SIM_NVS_DIR}"
)

target_compile_options(sim PRIVATE -Wall -Wextra -Wno-unused-parameter -g)

# 字体：设备上来自 78/xiaozhi-fonts 组件，源码就是普通 .c，PC 上直接编
file(GLOB FONT_SOURCES ${FONTS_DIR}/src/font_puhui_20_4.c ${FONTS_DIR}/src/font_puhui_30_4.c
     ${REPO_ROOT}/main/display/font/font_puhui_number_120_4.c
     ${FONTS_DIR}/src/font_awesome_20_4.c ${FONTS_DIR}/src/font_awesome_30_4.c)
add_library(sim_fonts STATIC ${FONT_SOURCES})
target_link_libraries(sim_fonts PRIVATE lvgl)
target_include_directories(sim_fonts PRIVATE ${LVGL_DIR})

target_link_libraries(sim PRIVATE lvgl sim_fonts pthread m)
