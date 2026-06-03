##
# @file CMakeLists.txt
# @brief 
#/

# MODULE_PATH
if (CONFIG_ENABLE_BLUETOOTH STREQUAL "y")

set(MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR})

# MODULE_NAME
get_filename_component(MODULE_NAME ${MODULE_PATH} NAME)

# LIB_SRCS
file(GLOB_RECURSE  LIB_SRCS  "${MODULE_PATH}/src/*.c")

# LIB_PUBLIC_INC
set(LIB_PUBLIC_INC ${MODULE_PATH}/include)


if (CONFIG_ENABLE_NIMBLE STREQUAL "y")

set(NIMBLE nimble)
file(GLOB_RECURSE  NIMBLE_SRCS  "${MODULE_PATH}/${NIMBLE}/*.c")
list(APPEND LIB_SRCS ${NIMBLE_SRCS})

list(APPEND  LIB_PUBLIC_INC ${MODULE_PATH}/${NIMBLE}/include)
set(LIB_PRIVATE_INC ${MODULE_PATH}/${NIMBLE}/host)

endif()

########################################
# Target Configure
########################################
add_library(${MODULE_NAME})

target_sources(${MODULE_NAME}
    PRIVATE
        ${LIB_SRCS}
    )

target_include_directories(${MODULE_NAME}
    PRIVATE
        ${LIB_PRIVATE_INC}

    INTERFACE
        ${LIB_INTERFACE_INC}

    PUBLIC
        ${LIB_PUBLIC_INC}
    )

if(CONFIG_ENABLE_QUADDLE_BLE_HID_CENTRAL STREQUAL "y")
    target_compile_definitions(${MODULE_NAME}
        PRIVATE
            TY_HS_BLE_ROLE_CENTRAL=1
            TY_HS_BLE_SM_LEGACY=1
            TY_HS_BLE_SM_BONDING=1
            TY_HS_BLE_SM_IO_CAP=3
            TY_HS_BLE_SM_OUR_KEY_DIST=3
            TY_HS_BLE_SM_THEIR_KEY_DIST=3)
endif()


########################################
# Layer Configure
########################################
list(APPEND COMPONENT_LIBS ${MODULE_NAME})
set(COMPONENT_LIBS "${COMPONENT_LIBS}" PARENT_SCOPE)
list(APPEND COMPONENT_PUBINC ${LIB_PUBLIC_INC})
set(COMPONENT_PUBINC "${COMPONENT_PUBINC}" PARENT_SCOPE)

endif()
