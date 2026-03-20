##
# @file CMakeLists.txt
# @brief Build configuration for petoi_esp32c3_cube application.
#/

# APP_PATH
set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})

# APP_NAME
get_filename_component(APP_NAME ${APP_PATH} NAME)

# APP_SRCS
aux_source_directory(${APP_PATH}/src APP_SRCS)

set(APP_INC ${APP_PATH}/include)

########################################
# Target Configure
########################################
add_library(${EXAMPLE_LIB})

target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRCS}
    )

target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${APP_INC}
    )

########################################
# Add subdirectory
########################################

if (CONFIG_ENABLE_BATTERY STREQUAL "y")
    add_subdirectory(${APP_PATH}/src/battery)
endif()

add_subdirectory(${APP_PATH}/../ai_components)

# Kconfig merge may not expose AI_AUDIO_VAD_ACTIVE_MS to the example target; force
# 0 here to shrink the recording ringbuf on ESP32-C3 (no PSRAM).
target_compile_definitions(${EXAMPLE_LIB} PRIVATE CONFIG_AI_AUDIO_VAD_ACTIVE_MS=0)
