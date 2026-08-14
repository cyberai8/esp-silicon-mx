# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "2.0.51")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# espressif/libjpeg-turbo 3.1.1 的 CMake 写死了 idf::libjpeg-turbo，IDF 5.5
# 里组件 target 是 ${COMPONENT_LIB}，不改 configure 会直接失败。
# managed_components 是拉下来的，每次解压后都可能被冲掉，所以这里重复打补丁。
set(_LJT_CMAKE "${CMAKE_SOURCE_DIR}/managed_components/espressif__libjpeg-turbo/CMakeLists.txt")
if(EXISTS "${_LJT_CMAKE}")
    file(READ "${_LJT_CMAKE}" _ljt_cmake)
    if(_ljt_cmake MATCHES "idf::libjpeg-turbo")
        string(REPLACE "idf::libjpeg-turbo" "\${COMPONENT_LIB}" _ljt_cmake "${_ljt_cmake}")
        file(WRITE "${_LJT_CMAKE}" "${_ljt_cmake}")
        message(STATUS "Patched libjpeg-turbo CMakeLists: idf::libjpeg-turbo -> COMPONENT_LIB")
    endif()
endif()

# Append before project() AND rely on CFLAGS for end-of-command-line override.
# IDF injects -Werror=all during project(); env CFLAGS are typically appended last.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=stringop-truncation" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=overflow" APPEND)

project(xiaozhi)

idf_build_set_property(COMPILE_OPTIONS "-Wno-error=stringop-truncation" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=overflow" APPEND)

# managed_components/ is gitignored and refreshed by the component manager.
# Keep udp_receive stack >= 8192: MqttProtocol AES decrypt runs on that task and
# overflows the upstream 2048B default during speaking (see crash logs).
set(_ESP_UDP_CC "${CMAKE_SOURCE_DIR}/managed_components/78__esp-ml307/src/esp/esp_udp.cc")
if(EXISTS "${_ESP_UDP_CC}")
    file(READ "${_ESP_UDP_CC}" _esp_udp_src)
    if(_esp_udp_src MATCHES "\"udp_receive\", 2048")
        string(REPLACE "\"udp_receive\", 2048" "\"udp_receive\", 8192" _esp_udp_src "${_esp_udp_src}")
        file(WRITE "${_ESP_UDP_CC}" "${_esp_udp_src}")
        message(STATUS "Patched 78/esp-ml307 udp_receive stack: 2048 -> 8192")
    endif()
endif()

# Override esp-sr packed model with custom wakeword when present
if(EXISTS "${CMAKE_SOURCE_DIR}/wakeword/srmodels.bin")
    if(TARGET srmodels_bin)
        add_custom_target(custom_wakeword_srmodels ALL
            COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_SOURCE_DIR}/wakeword/srmodels.bin"
                "${CMAKE_BINARY_DIR}/srmodels/srmodels.bin"
            COMMENT "Override srmodels.bin with custom wakeword"
        )
        add_dependencies(custom_wakeword_srmodels srmodels_bin)
        if(TARGET flash)
            add_dependencies(flash custom_wakeword_srmodels)
        endif()
        message(STATUS "Custom wakeword enabled: wakeword/srmodels.bin -> build/srmodels/srmodels.bin")
    else()
        message(WARNING "wakeword/srmodels.bin exists but srmodels_bin target not found; custom wakeword override skipped")
    endif()
endif()

