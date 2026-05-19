##
# @file CMakeLists.txt
# @brief UART1 (second serial) hello test for ESP32-S3 DNESP32S3_BOX
#/

if (CONFIG_ENABLE_UART STREQUAL "y")

set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})
get_filename_component(APP_NAME ${APP_PATH} NAME)
aux_source_directory(${APP_PATH}/src APP_SRCS)

add_library(${EXAMPLE_LIB})

target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRCS}
    )
else()
    message(FATAL_ERROR "uart1_second_hello requires CONFIG_ENABLE_UART=y")
endif()
