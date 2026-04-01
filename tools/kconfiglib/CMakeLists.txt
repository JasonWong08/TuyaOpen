##
# @file kconfiglib/CMakeLists.txt
# @brief 
# @author Tuya
# @version 1.0.0
# @date 2023-04-08
#/

########################################
# First Run
########################################
# check dot-config
if(NOT EXISTS "${DOT_CONFIG_DIR}/${DOT_CONFIG}")  # copy pre-config to dot-config
    message(FATAL_ERROR "
    [KCONFIG] Don't found
    [${DOT_CONFIG_DIR}/${DOT_CONFIG}]
    Please use command: [tos config_choice or tos menuconfig].
    ")
endif()

# use dot-config to generate cmake-config
# Refresh when using.config is newer than generated using.cmake.
set(KCONFIG_NEED_REGEN_CMAKE FALSE)
if(NOT EXISTS "${DOT_CONFIG_DIR}/${CMAKE_CONFIG}")
    set(KCONFIG_NEED_REGEN_CMAKE TRUE)
else()
    file(TIMESTAMP "${DOT_CONFIG_DIR}/${DOT_CONFIG}" DOT_CONFIG_TS "%s")
    file(TIMESTAMP "${DOT_CONFIG_DIR}/${CMAKE_CONFIG}" CMAKE_CONFIG_TS "%s")
    if(DOT_CONFIG_TS GREATER CMAKE_CONFIG_TS)
        set(KCONFIG_NEED_REGEN_CMAKE TRUE)
    endif()
endif()
if(KCONFIG_NEED_REGEN_CMAKE)
    message(STATUS "[KCONFIG] Use dot-config to generate cmake-config.")
    execute_process(
        COMMAND
        python ${KCONFIG_TOOLS}/conf2cmake.py -c "${DOT_CONFIG_DIR}/${DOT_CONFIG}" -o "${DOT_CONFIG_DIR}/${CMAKE_CONFIG}"
        )
endif()

# use dot-config to generate header-file
set(KCONFIG_NEED_REGEN_HEADER FALSE)
if(NOT EXISTS "${HEADER_DIR}/${HEADER_FILE}")
    set(KCONFIG_NEED_REGEN_HEADER TRUE)
else()
    file(TIMESTAMP "${DOT_CONFIG_DIR}/${DOT_CONFIG}" DOT_CONFIG_TS "%s")
    file(TIMESTAMP "${HEADER_DIR}/${HEADER_FILE}" HEADER_FILE_TS "%s")
    if(DOT_CONFIG_TS GREATER HEADER_FILE_TS)
        set(KCONFIG_NEED_REGEN_HEADER TRUE)
    endif()
endif()
if(KCONFIG_NEED_REGEN_HEADER)
    message(STATUS "[KCONFIG] Use dot-config to generate header-file.")
    execute_process(
        COMMAND
        ${CMAKE_COMMAND} -E make_directory ${HEADER_DIR}

        COMMAND
        python ${KCONFIG_TOOLS}/conf2h.py -c "${DOT_CONFIG_DIR}/${DOT_CONFIG}" -i "${HEADER_IN_PATH}" -p "${HEADER_PARAM_LIST}" -o "${HEADER_DIR}/${HEADER_FILE}"
        )
endif()
