# Define source files
set(SOURCES "audio/audio_codec.cc"
            "audio/audio_service.cc"
            "audio/codecs/no_audio_codec.cc"
            "audio/codecs/box_audio_codec.cc"
            "audio/codecs/es8311_audio_codec.cc"
            "audio/codecs/es8374_audio_codec.cc"
            "audio/codecs/es8388_audio_codec.cc"
            "audio/codecs/es8389_audio_codec.cc"
            "audio/codecs/dummy_audio_codec.cc"
            "audio/processors/audio_debugger.cc"
            "led/single_led.cc"
            "led/circular_strip.cc"
            "led/gpio_led.cc"
            "display/display.cc"
            "display/lcd_display.cc"
            "display/oled_display.cc"
            "display/lv_adapter_display.cc"
            "display/display_orientation.cc"
            "display/touch_feed.cc"
            "display/font/font_puhui_number_50_4.c"
            "display/font/font_puhui_number_120_4.c"
            "i18n/i18n.cc"
            "display/screen/screen_util.cc"
            "display/screen/wifi_required_dialog.cc"
            "display/screen/round_keyboard.cc"
            "display/screen/battery_alert.cc"
            "display/screen/idle_power_policy.cc"
            "display/screen/pwr_key_handler.cc"
            "display/screen/boot_screen/boot_screen.cc"
            "display/screen/ota_screen/ota_screen.cc"
            "display/screen/wifi_config_tip_screen/wifi_config_tip_screen.cc"
            "display/screen/home_screen/home_screen.cc"
            "display/screen/info_screen/info_screen.cc"
            "display/screen/standby_screen/standby_screen.cc"
            "display/screen/standby_screen/standby_gallery.cc"
            "display/screen/game_2048_screen/game_2048_screen.cc"
            "display/screen/clock_screen/clock_screen.cc"
            "display/screen/album_screen/album_screen.cc"
            "display/screen/music_screen/music_screen.cc"
            "display/screen/music_screen/music_screen_sd.cc"
            # 暂时不编电台：主页入口已隐藏；恢复时取消下行注释，并恢复 home_screen 的 LaunchRadio。
            # "display/screen/radio_screen/radio_screen.cc"
            "display/screen/recording_screen/recording_screen.cc"
            "display/screen/weather_screen/weather_screen.cc"
            "display/screen/weather_screen/weather_icon_map.cc"
            "display/screen/weather_screen/weather_city_list.cc"
            "display/screen/gps_screen/gps_screen.cc"
            "display/screen/vibrate_screen/vibrate_screen.cc"
            "display/screen/backlight_screen/backlight_screen.cc"
            "display/screen/settings_screen/settings_screen.cc"
            "display/screen/call_screen/call_screen.cc"
            "display/screen/bluetooth_screen/bluetooth_screen.cc"
            "display/screen/chat_screen/chat_screen.cc"
            "display/screen/network_screen/network_screen.cc"
            "display/screen/digital_people_screen/digital_people_screen.cc"
            "display/screen/digital_people_screen/avatar_compositor.cc"
            "display/screen/level_screen/level_screen.cc"
            "display/screen/magnet_screen/magnet_screen.cc"
            "display/screen/sd_card_screen/sd_card_screen.cc"
            "display/screen/pin_test_screen/pin_test_screen.cc"
            "display/screen/test_screen/test_screen.cc"
            "display/screen/test_screen/auto_test_screen.cc"
            "display/screen/test_screen/stress_test_screen.cc"
            "display/screen/test_screen/stress_demo.c"
            "display/screen/test_screen/test_ui_common.cc"
            "display/screen/test_screen/vibrate_motor_test.cc"
            "display/screen/test_screen/audio_test.cc"
            "display/screen/test_screen/sc7a20h_test.cc"
            "display/screen/test_screen/qmc6309_test.cc"
            "display/screen/test_screen/sd_card_test.cc"
            "display/screen/test_screen/cell_4g_test.cc"
            "display/screen/test_screen/wifi_test.cc"
            "display/screen/test_screen/gps_test.cc"
            "display/screen/test_screen/bq27220_test.cc"
            "display/screen/test_screen/nu1680_test.cc"
            "display/screen/test_screen/pin_gpio_test.cc"
            "display/screen/test_screen/screen_color_test.cc"
            "display/screen/test_screen/touch_panel_test.cc"
            "display/screen/badge_screen/badge_screen.cc"
            "display/screen/bagclip_screen/bagclip_screen.cc"
            "display/lvgl_display/lvgl_display.cc"
            "display/lvgl_display/emoji_collection.cc"
            "display/lvgl_display/lvgl_theme.cc"
            "display/lvgl_display/lvgl_font.cc"
            "display/lvgl_display/lvgl_image.cc"
            "display/lvgl_display/gif/lvgl_gif.cc"
            "display/lvgl_display/gif/gifdec.c"
            "display/lvgl_display/jpg/image_to_jpeg.cpp"
            "display/lvgl_display/jpg/jpeg_to_image.c"
            "protocols/protocol.cc"
            "protocols/mqtt_protocol.cc"
            "protocols/websocket_protocol.cc"
            "mcp_server.cc"
            "system_info.cc"
            "application.cc"
            "ota.cc"
            "api_endpoints.cc"
            "settings.cc"
            "device_state_event.cc"
            "assets.cc"
            "main.cc"
            )

set(INCLUDE_DIRS "." "i18n" "display" "display/font" "display/lvgl_display" "display/lvgl_display/jpg" "display/screen" "display/screen/boot_screen" "display/screen/ota_screen" "display/screen/wifi_config_tip_screen" "display/screen/home_screen" "display/screen/game_2048_screen" "display/screen/clock_screen" "display/screen/album_screen" "display/screen/music_screen" "display/screen/radio_screen" "display/screen/recording_screen" "display/screen/weather_screen" "display/screen/gps_screen" "display/screen/vibrate_screen" "display/screen/backlight_screen" "display/screen/settings_screen" "display/screen/call_screen" "display/screen/bluetooth_screen" "display/screen/chat_screen" "display/screen/network_screen" "display/screen/digital_people_screen" "display/screen/level_screen" "display/screen/magnet_screen" "display/screen/sd_card_screen" "display/screen/pin_test_screen" "display/screen/test_screen" "display/screen/info_screen" "display/screen/standby_screen" "display/screen/badge_screen" "display/screen/bagclip_screen" "audio" "protocols")

# Add board common files (ESP-Show: WiFi only, no 4G / USB disk / legacy chargers)
file(GLOB BOARD_COMMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/boards/common/*.cc)
list(APPEND SOURCES ${BOARD_COMMON_SOURCES})
list(APPEND INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/boards/common)

idf_build_get_property(build_components BUILD_COMPONENTS)
# Function to find component dynamically by pattern
function(find_component_by_pattern PATTERN COMPONENT_VAR PATH_VAR)
    foreach(COMPONENT ${build_components})
        if(COMPONENT MATCHES "${PATTERN}")
            set(${COMPONENT_VAR} ${COMPONENT} PARENT_SCOPE)
            idf_component_get_property(COMPONENT_PATH ${COMPONENT} COMPONENT_DIR)
            set(${PATH_VAR} "${COMPONENT_PATH}" PARENT_SCOPE)
            break()
        endif()
    endforeach()
endfunction()

# Set default BUILTIN_TEXT_FONT and BUILTIN_ICON_FONT
set(BUILTIN_TEXT_FONT font_puhui_14_1)
set(BUILTIN_ICON_FONT font_awesome_14_1)

# ESP-Show is the only supported board target.
set(BOARD_TYPE "esp-show")
set(BUILTIN_TEXT_FONT font_puhui_20_4)
set(BUILTIN_ICON_FONT font_awesome_20_4)
set(DEFAULT_EMOJI_COLLECTION twemoji_64)

file(GLOB BOARD_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_TYPE}/*.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_TYPE}/*.c
)
list(APPEND SOURCES ${BOARD_SOURCES})
list(APPEND INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_TYPE})

# Select audio processor according to Kconfig
if(CONFIG_USE_AUDIO_PROCESSOR)
    list(APPEND SOURCES "audio/processors/afe_audio_processor.cc")
else()
    list(APPEND SOURCES "audio/processors/no_audio_processor.cc")
endif()
if(CONFIG_USE_AFE_WAKE_WORD)
    list(APPEND SOURCES "audio/wake_words/afe_wake_word.cc")
endif()
if(CONFIG_USE_CUSTOM_WAKE_WORD)
    list(APPEND SOURCES "audio/wake_words/custom_wake_word.cc")
endif()
if(CONFIG_USE_ESP_WAKE_WORD)
    list(APPEND SOURCES "audio/wake_words/esp_wake_word.cc")
endif()

# Select language directory according to Kconfig
if(CONFIG_LANGUAGE_ZH_CN)
    set(LANG_DIR "zh-CN")
elseif(CONFIG_LANGUAGE_ZH_TW)
    set(LANG_DIR "zh-TW")
elseif(CONFIG_LANGUAGE_EN_US)
    set(LANG_DIR "en-US")
elseif(CONFIG_LANGUAGE_JA_JP)
    set(LANG_DIR "ja-JP")
elseif(CONFIG_LANGUAGE_KO_KR)
    set(LANG_DIR "ko-KR")
elseif(CONFIG_LANGUAGE_VI_VN)
    set(LANG_DIR "vi-VN")
elseif(CONFIG_LANGUAGE_TH_TH)
    set(LANG_DIR "th-TH")
elseif(CONFIG_LANGUAGE_DE_DE)
    set(LANG_DIR "de-DE")
elseif(CONFIG_LANGUAGE_FR_FR)
    set(LANG_DIR "fr-FR")
elseif(CONFIG_LANGUAGE_ES_ES)
    set(LANG_DIR "es-ES")
elseif(CONFIG_LANGUAGE_IT_IT)
    set(LANG_DIR "it-IT")
elseif(CONFIG_LANGUAGE_RU_RU)
    set(LANG_DIR "ru-RU")
elseif(CONFIG_LANGUAGE_AR_SA)
    set(LANG_DIR "ar-SA")
elseif(CONFIG_LANGUAGE_HI_IN)
    set(LANG_DIR "hi-IN")
elseif(CONFIG_LANGUAGE_PT_PT)
    set(LANG_DIR "pt-PT")
elseif(CONFIG_LANGUAGE_PL_PL)
    set(LANG_DIR "pl-PL")
elseif(CONFIG_LANGUAGE_CS_CZ)
    set(LANG_DIR "cs-CZ")
elseif(CONFIG_LANGUAGE_FI_FI)
    set(LANG_DIR "fi-FI")
elseif(CONFIG_LANGUAGE_TR_TR)
    set(LANG_DIR "tr-TR")
elseif(CONFIG_LANGUAGE_ID_ID)
    set(LANG_DIR "id-ID")
elseif(CONFIG_LANGUAGE_UK_UA)
    set(LANG_DIR "uk-UA")
elseif(CONFIG_LANGUAGE_RO_RO)
    set(LANG_DIR "ro-RO")
elseif(CONFIG_LANGUAGE_BG_BG)
    set(LANG_DIR "bg-BG")
elseif(CONFIG_LANGUAGE_CA_ES)
    set(LANG_DIR "ca-ES")
elseif(CONFIG_LANGUAGE_DA_DK)
    set(LANG_DIR "da-DK")
elseif(CONFIG_LANGUAGE_EL_GR)
    set(LANG_DIR "el-GR")
elseif(CONFIG_LANGUAGE_FA_IR)
    set(LANG_DIR "fa-IR")
elseif(CONFIG_LANGUAGE_FIL_PH)
    set(LANG_DIR "fil-PH")
elseif(CONFIG_LANGUAGE_HE_IL)
    set(LANG_DIR "he-IL")
elseif(CONFIG_LANGUAGE_HR_HR)
    set(LANG_DIR "hr-HR")
elseif(CONFIG_LANGUAGE_HU_HU)
    set(LANG_DIR "hu-HU")
elseif(CONFIG_LANGUAGE_MS_MY)
    set(LANG_DIR "ms-MY")
elseif(CONFIG_LANGUAGE_NB_NO)
    set(LANG_DIR "nb-NO")
elseif(CONFIG_LANGUAGE_NL_NL)
    set(LANG_DIR "nl-NL")
elseif(CONFIG_LANGUAGE_SK_SK)
    set(LANG_DIR "sk-SK")
elseif(CONFIG_LANGUAGE_SL_SI)
    set(LANG_DIR "sl-SI")
elseif(CONFIG_LANGUAGE_SV_SE)
    set(LANG_DIR "sv-SE")
elseif(CONFIG_LANGUAGE_SR_RS)
    set(LANG_DIR "sr-RS")
endif()

# Define generation path
set(LANG_JSON "${CMAKE_CURRENT_SOURCE_DIR}/assets/locales/${LANG_DIR}/language.json")
set(LANG_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/assets/lang_config.h")

# Collect current language audio files
file(GLOB LANG_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/locales/${LANG_DIR}/*.ogg)

# If not en-US, collect en-US audio files as fallback for missing files
if(NOT LANG_DIR STREQUAL "en-US")
    file(GLOB EN_US_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/locales/en-US/*.ogg)
    
    # Extract filenames (without path) from current language
    set(EXISTING_NAMES "")
    foreach(SOUND_FILE ${LANG_SOUNDS})
        get_filename_component(FILENAME ${SOUND_FILE} NAME)
        list(APPEND EXISTING_NAMES ${FILENAME})
    endforeach()
    
    # Only add en-US audio files that are missing in current language
    foreach(EN_SOUND ${EN_US_SOUNDS})
        get_filename_component(FILENAME ${EN_SOUND} NAME)
        if(NOT ${FILENAME} IN_LIST EXISTING_NAMES)
            list(APPEND LANG_SOUNDS ${EN_SOUND})
            message(STATUS "Using en-US fallback for missing audio: ${FILENAME}")
        endif()
    endforeach()
endif()

file(GLOB COMMON_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/common/*.ogg)

# If target chip is ESP32, exclude specific files to avoid build errors
if(CONFIG_IDF_TARGET_ESP32)
    list(REMOVE_ITEM SOURCES "audio/codecs/box_audio_codec.cc"
                             "audio/codecs/es8388_audio_codec.cc"
                             "audio/codecs/es8389_audio_codec.cc"
                             "led/gpio_led.cc"
                             "display/lvgl_display/jpg/image_to_jpeg.cpp"
                             "display/lvgl_display/jpg/jpeg_to_image.c"
                             )
endif()

idf_component_register(SRCS ${SOURCES}
                    EMBED_FILES ${LANG_SOUNDS} ${COMMON_SOUNDS}
                    INCLUDE_DIRS ${INCLUDE_DIRS}
                    WHOLE_ARCHIVE
                    )

# IDF 6.x / mbedTLS 4.x: legacy AES headers live under tf-psa-crypto private include.
# Do NOT add REQUIRES here — that disables auto dependency inference for main.
idf_component_get_property(_mbedtls_dir mbedtls COMPONENT_DIR)
set(_mbedtls_builtin_inc "${_mbedtls_dir}/mbedtls/tf-psa-crypto/drivers/builtin/include")
if(EXISTS "${_mbedtls_builtin_inc}/mbedtls/private/aes.h")
    target_include_directories(${COMPONENT_LIB} PRIVATE "${_mbedtls_builtin_inc}")
endif()

# 温度传感器驱动仅在目标 SoC 支持时链接
if(CONFIG_SOC_TEMP_SENSOR_SUPPORTED)
    target_link_libraries(${COMPONENT_LIB} PRIVATE idf::esp_driver_tsens)
endif()

# Use target_compile_definitions to define BOARD_TYPE, BOARD_NAME
# If BOARD_NAME is empty, use BOARD_TYPE
if(NOT BOARD_NAME)
    set(BOARD_NAME ${BOARD_TYPE})
endif()
target_compile_definitions(${COMPONENT_LIB}
                    PRIVATE BOARD_TYPE=\"${BOARD_TYPE}\" BOARD_NAME=\"${BOARD_NAME}\"
                    PRIVATE BUILTIN_TEXT_FONT=${BUILTIN_TEXT_FONT} BUILTIN_ICON_FONT=${BUILTIN_ICON_FONT}
                    )
# 360 圆屏 UI 适配 + OTA 上报 esp-show（服务端 viseme 口型轴）
target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_ESP_VOCAT=1 BOARD_ESP_SHOW=1
                    OTA_BOARD_TYPE=\"esp-show\")

# Add generation rules
# Use ${PYTHON} (IDF-resolved interpreter). Bare `python` is often missing from
# cmd.exe PATH on Windows even when the IDF Python env is active in PowerShell.
add_custom_command(
    OUTPUT ${LANG_HEADER}
    COMMAND ${PYTHON} ${PROJECT_DIR}/scripts/gen_lang.py
            --language "${LANG_DIR}"
            --output "${LANG_HEADER}"
    DEPENDS
        ${LANG_JSON}
        ${PROJECT_DIR}/scripts/gen_lang.py
    COMMENT "Generating ${LANG_DIR} language config"
)

# Force build generation dependencies
add_custom_target(lang_header ALL
    DEPENDS ${LANG_HEADER}
)

# Runtime UI i18n (multi-locale tables from catalog.json)
set(I18N_CATALOG "${CMAKE_CURRENT_SOURCE_DIR}/i18n/catalog.json")
set(I18N_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/i18n/i18n_strings_gen.h")
add_custom_command(
    OUTPUT ${I18N_HEADER}
    COMMAND ${PYTHON} ${PROJECT_DIR}/scripts/gen_i18n.py
            --catalog "${I18N_CATALOG}"
            --output "${I18N_HEADER}"
    DEPENDS
        ${I18N_CATALOG}
        ${PROJECT_DIR}/scripts/gen_i18n.py
    COMMENT "Generating runtime i18n string tables"
)
add_custom_target(i18n_header ALL DEPENDS ${I18N_HEADER})
add_dependencies(${COMPONENT_LIB} i18n_header)

# Find ESP-SR component dynamically
find_component_by_pattern("espressif__esp-sr" ESP_SR_COMPONENT ESP_SR_COMPONENT_PATH)
if(ESP_SR_COMPONENT_PATH)
    set(ESP_SR_MODEL_PATH "${ESP_SR_COMPONENT_PATH}/model")
endif()

# Find xiaozhi-fonts component dynamically
find_component_by_pattern("xiaozhi-fonts" XIAOZHI_FONTS_COMPONENT XIAOZHI_FONTS_COMPONENT_PATH)
if(XIAOZHI_FONTS_COMPONENT_PATH)
    set(XIAOZHI_FONTS_PATH "${XIAOZHI_FONTS_COMPONENT_PATH}")
endif()

# Function to build default assets based on configuration
function(build_default_assets_bin)
    # Set output path for generated assets.bin
    set(GENERATED_ASSETS_BIN "${CMAKE_BINARY_DIR}/generated_assets.bin")
    
    # Prepare arguments for build script
    set(BUILD_ARGS
        "--sdkconfig" "${SDKCONFIG}"
        "--output" "${GENERATED_ASSETS_BIN}"
    )
    
    # Add builtin text font if defined
    if(BUILTIN_TEXT_FONT)
        list(APPEND BUILD_ARGS "--builtin_text_font" "${BUILTIN_TEXT_FONT}")
    endif()
    
    # Add default emoji collection if defined
    if(DEFAULT_EMOJI_COLLECTION)
        list(APPEND BUILD_ARGS "--emoji_collection" "${DEFAULT_EMOJI_COLLECTION}")
    endif()
    
    # Add default assets extra files if defined
    if(DEFAULT_ASSETS_EXTRA_FILES)
        list(APPEND BUILD_ARGS "--extra_files" "${DEFAULT_ASSETS_EXTRA_FILES}")
    endif()
    
    list(APPEND BUILD_ARGS "--esp_sr_model_path" "${ESP_SR_MODEL_PATH}")
    list(APPEND BUILD_ARGS "--xiaozhi_fonts_path" "${XIAOZHI_FONTS_PATH}")
    
    # Create custom command to build assets
    add_custom_command(
        OUTPUT ${GENERATED_ASSETS_BIN}
        COMMAND ${PYTHON} ${PROJECT_DIR}/scripts/build_default_assets.py ${BUILD_ARGS}
        DEPENDS
            ${SDKCONFIG}
            ${PROJECT_DIR}/scripts/build_default_assets.py
        COMMENT "Building default assets.bin based on configuration"
        VERBATIM
    )
    
    # Create target for generated assets
    add_custom_target(generated_default_assets ALL
        DEPENDS ${GENERATED_ASSETS_BIN}
    )
    
    # Set the generated file path in parent scope
    set(GENERATED_ASSETS_LOCAL_FILE ${GENERATED_ASSETS_BIN} PARENT_SCOPE)
    
    message(STATUS "Default assets build configured: ${GENERATED_ASSETS_BIN}")
endfunction()


# Function to get local assets file path (handles both URL and local file)
function(get_assets_local_file assets_source assets_local_file_var)
    # Check if it's a URL (starts with http:// or https://)
    if(assets_source MATCHES "^https?://")
        # It's a URL, download it
        get_filename_component(ASSETS_FILENAME "${assets_source}" NAME)
        set(ASSETS_LOCAL_FILE "${CMAKE_BINARY_DIR}/${ASSETS_FILENAME}")
        set(ASSETS_TEMP_FILE "${CMAKE_BINARY_DIR}/${ASSETS_FILENAME}.tmp")
        
        # Check if local file exists
        if(EXISTS ${ASSETS_LOCAL_FILE})
            message(STATUS "Assets file ${ASSETS_FILENAME} already exists, skipping download")
        else()
            message(STATUS "Downloading ${ASSETS_FILENAME}")
            
            # Clean up any existing temp file
            if(EXISTS ${ASSETS_TEMP_FILE})
                file(REMOVE ${ASSETS_TEMP_FILE})
            endif()
            
            # Download to temporary file first
            file(DOWNLOAD ${assets_source} ${ASSETS_TEMP_FILE}
                 STATUS DOWNLOAD_STATUS)
            list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
            if(NOT STATUS_CODE EQUAL 0)
                # Clean up temp file on failure
                if(EXISTS ${ASSETS_TEMP_FILE})
                    file(REMOVE ${ASSETS_TEMP_FILE})
                endif()
                message(FATAL_ERROR "Failed to download ${ASSETS_FILENAME} from ${assets_source}")
            endif()
            
            # Move temp file to final location (atomic operation)
            file(RENAME ${ASSETS_TEMP_FILE} ${ASSETS_LOCAL_FILE})
            message(STATUS "Successfully downloaded ${ASSETS_FILENAME}")
        endif()
    else()
        # It's a local file path
        if(IS_ABSOLUTE "${assets_source}")
            set(ASSETS_LOCAL_FILE "${assets_source}")
        else()
            set(ASSETS_LOCAL_FILE "${CMAKE_CURRENT_SOURCE_DIR}/${assets_source}")
        endif()
        
        # Check if local file exists
        if(NOT EXISTS ${ASSETS_LOCAL_FILE})
            message(FATAL_ERROR "Assets file not found: ${ASSETS_LOCAL_FILE}")
        endif()
        
        message(STATUS "Using assets file: ${ASSETS_LOCAL_FILE}")
    endif()
    
    set(${assets_local_file_var} ${ASSETS_LOCAL_FILE} PARENT_SCOPE)
endfunction()


partition_table_get_partition_info(size "--partition-name assets" "size")
partition_table_get_partition_info(offset "--partition-name assets" "offset")
if ("${size}" AND "${offset}")
    # Flash assets based on configuration
    if(CONFIG_FLASH_DEFAULT_ASSETS)
        # Build default assets based on configuration
        build_default_assets_bin()
        esptool_py_flash_to_partition(flash "assets" "${GENERATED_ASSETS_LOCAL_FILE}")
        message(STATUS "Generated default assets flash configured: ${GENERATED_ASSETS_LOCAL_FILE} -> assets partition")
    elseif(CONFIG_FLASH_CUSTOM_ASSETS)
        # Flash custom assets
        get_assets_local_file("${CONFIG_CUSTOM_ASSETS_FILE}" ASSETS_LOCAL_FILE)
        esptool_py_flash_to_partition(flash "assets" "${ASSETS_LOCAL_FILE}")
        message(STATUS "Custom assets flash configured: ${ASSETS_LOCAL_FILE} -> assets partition")
    elseif(CONFIG_FLASH_NONE_ASSETS)
        message(STATUS "Assets flashing disabled (FLASH_NONE_ASSETS)")
    endif()
else()
    message(STATUS "Assets partition not found, using v1 partition table")
endif()

# 数字人 show11 runtime PNG 打进 mmap 资源分区（A:），预览图不进固件。
# 聊天应用仍读 SD 卡 system/chat/*.eaf，互不影响。
set(AVATAR_RUNTIME_DIR "${CMAKE_SOURCE_DIR}/数字人口型素材/show11/show11/runtime")
if(NOT EXISTS "${AVATAR_RUNTIME_DIR}/base_360.png")
    message(FATAL_ERROR "Digital people avatar runtime missing: ${AVATAR_RUNTIME_DIR}")
endif()
set(ASSETS_PACK_DIR "${CMAKE_BINARY_DIR}/xingzhi-assets-pack")
# CONFIGURE_DEPENDS：xingzhi-assets 里图标一改，下次 build 会重新 cmake 拷贝进 pack，
# 避免只改了 png、固件 resources 分区仍是旧图（播放键圆环就是这个问题）。
file(GLOB_RECURSE XINGZHI_ASSET_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/xingzhi-assets/*")
file(GLOB AVATAR_RUNTIME_PNGS CONFIGURE_DEPENDS "${AVATAR_RUNTIME_DIR}/*.png")
file(REMOVE_RECURSE "${ASSETS_PACK_DIR}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/xingzhi-assets/" DESTINATION "${ASSETS_PACK_DIR}")
file(COPY ${AVATAR_RUNTIME_PNGS} DESTINATION "${ASSETS_PACK_DIR}")
list(LENGTH AVATAR_RUNTIME_PNGS AVATAR_PNG_COUNT)
list(LENGTH XINGZHI_ASSET_FILES XINGZHI_ASSET_COUNT)
message(STATUS "Packing xingzhi-assets: ${XINGZHI_ASSET_COUNT} files")
message(STATUS "Packing show11 avatar runtime PNGs: ${AVATAR_PNG_COUNT} files")

spiffs_create_partition_assets(
    resources
    "${ASSETS_PACK_DIR}"
    FLASH_IN_PROJECT
    MMAP_FILE_SUPPORT_FORMAT ".jpg,.png,.eaf"
    MMAP_SUPPORT_SJPG
    MMAP_SUPPORT_SPNG
    MMAP_SPLIT_HEIGHT 16
) 

spiffs_create_partition_image(factory_test ./factory-test-assets FLASH_IN_PROJECT DEPENDS)
