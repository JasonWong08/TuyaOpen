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

set(APP_INC ${APP_PATH}/include)

########################################
# Generate language config header
########################################


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

if("${CONFIG_ENABLE_CHAT_BOT_ROBOT_SECOND_UART}" STREQUAL "y")
    target_compile_definitions(${EXAMPLE_LIB}
        PRIVATE
            ENABLE_CHAT_BOT_ROBOT_SECOND_UART=1)
    target_sources(${EXAMPLE_LIB}
        PRIVATE
            ${APP_PATH}/src/second_uart.c
            ${APP_PATH}/src/robot_uart_voice.c
            ${APP_PATH}/src/ai_mcp_robot_tools.c)
endif()

########################################
# Add subdirectory
########################################

if (CONFIG_ENABLE_BATTERY STREQUAL "y")
    add_subdirectory(${APP_PATH}/src/battery)
endif()

add_subdirectory(${APP_PATH}/../ai_components)
