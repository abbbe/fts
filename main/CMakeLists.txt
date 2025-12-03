# Conditional source file selection based on BUILD_TARGET environment variable
# Usage:
#   BUILD_TARGET=ftms_master idf.py build    # FTS master (AP)
#   BUILD_TARGET=ftms idf.py build           # FTS slave (STA)

# Generate build_info.h with git hash, dirty status, and timestamp
set(BUILD_INFO_H "${CMAKE_CURRENT_BINARY_DIR}/build_info.h")
execute_process(
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT_FILE=${BUILD_INFO_H}
        -DCMAKE_SOURCE_DIR=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/build_info.cmake
)

set(MAIN_SRCS "fts_main.c")

# Unified requirements for fts_main.c (both master and slave)
set(COMPONENT_REQUIRES "esp_driver_mcpwm" "ftm" "dtr" "dtc")

idf_component_register(SRCS ${MAIN_SRCS}
                       INCLUDE_DIRS "." "${CMAKE_CURRENT_BINARY_DIR}"
                       PRIV_REQUIRES ${COMPONENT_REQUIRES})