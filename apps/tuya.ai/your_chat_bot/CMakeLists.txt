##
# @file CMakeLists.txt
# @brief 
#/

# APP_PATH
set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})

# APP_NAME
get_filename_component(APP_NAME ${APP_PATH} NAME)

# APP_SRCS (optional robot UART sources are added only when Kconfig enables them)
aux_source_directory(${APP_PATH}/src APP_SRCS)
list(FILTER APP_SRCS EXCLUDE REGEX ".*/ai_mcp_robot_tools\\.c$")
list(FILTER APP_SRCS EXCLUDE REGEX ".*/second_uart\\.c$")
list(FILTER APP_SRCS EXCLUDE REGEX ".*/robot_uart_voice\\.c$")
list(FILTER APP_SRCS EXCLUDE REGEX ".*/quaddle_robot_bridge\\.c$")
list(FILTER APP_SRCS EXCLUDE REGEX ".*/quaddle_ble_hid_central\\.c$")

set(APP_INC ${APP_PATH}/include)

########################################
# Generate language config header
########################################


########################################
# Target Configure
########################################
add_library(${EXAMPLE_LIB})

# This product always runs in wake-word free-chat mode.  Keep the policy
# application-specific even though the AI components are compiled into the
# same target.
target_compile_definitions(${EXAMPLE_LIB}
    PRIVATE
        YOUR_CHAT_BOT_FIXED_FREE_MODE=1)

target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRCS}
    )

target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${APP_INC}
    )

if("${CONFIG_ENABLE_CHAT_BOT_ROBOT_SECOND_UART}" STREQUAL "y")
    target_compile_definitions(${EXAMPLE_LIB}
        PRIVATE
            ENABLE_CHAT_BOT_ROBOT_SECOND_UART=1)
    target_sources(${EXAMPLE_LIB}
        PRIVATE
            ${APP_PATH}/src/second_uart.c
            ${APP_PATH}/src/robot_uart_voice.c
            ${APP_PATH}/src/ai_mcp_robot_tools.c
            ${APP_PATH}/src/quaddle_robot_bridge.c)
endif()

if(("${CONFIG_ENABLE_QUADDLE_GAMEPAD}" STREQUAL "y") OR ("${CONFIG_ENABLE_QUADDLE_BLE_HID_CENTRAL}" STREQUAL "y"))
    target_compile_definitions(${EXAMPLE_LIB}
        PRIVATE
            ENABLE_QUADDLE_GAMEPAD=1)
endif()

if("${CONFIG_ENABLE_QUADDLE_BLE_HID_CENTRAL}" STREQUAL "y")
    target_compile_definitions(${EXAMPLE_LIB}
        PRIVATE
            ENABLE_QUADDLE_BLE_HID_CENTRAL=1
            QUADDLE_BLE_GAMEPAD_NAME_FILTER="${CONFIG_QUADDLE_BLE_GAMEPAD_NAME_FILTER}")
    target_sources(${EXAMPLE_LIB}
        PRIVATE
            ${APP_PATH}/src/quaddle_ble_hid_central.c)
endif()

########################################
# Add subdirectory
########################################

if (CONFIG_ENABLE_BATTERY STREQUAL "y")
    add_subdirectory(${APP_PATH}/src/battery)
endif()

add_subdirectory(${APP_PATH}/../ai_components)
