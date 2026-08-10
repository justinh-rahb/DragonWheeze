# DragonWheeze — open firmware for the Sovol SH01 Filament Dryer (ESP32-C3 Super Mini)
cmake_minimum_required(VERSION 3.16)

# Shared board-agnostic services come from justinh-rahb/dragon-core through the
# ESP-IDF component manager. Product-specific dw_* components remain local.
execute_process(
    COMMAND git describe --tags --exact-match
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE DW_TAG OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET RESULT_VARIABLE DW_TAG_RC)
if(DW_TAG_RC EQUAL 0 AND DW_TAG)
    set(PROJECT_VER "${DW_TAG}")
else()
    execute_process(
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE DW_HASH OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE DW_HASH_RC)
    execute_process(
        COMMAND git status --porcelain --untracked-files=no
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE DW_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT DW_HASH_RC EQUAL 0)
        set(PROJECT_VER "unknown")
    elseif(DW_DIRTY)
        set(PROJECT_VER "${DW_HASH}-dirty")
    else()
        set(PROJECT_VER "${DW_HASH}")
    endif()
endif()
message(STATUS "DragonWheeze version: ${PROJECT_VER}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dragonwheeze)

# Apply warnings-as-errors to first-party dw_* components
file(GLOB DW_OWN_COMPONENT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/components/dw_*)
set(DW_WERROR_APPLIED 0)
foreach(dw_dir ${DW_OWN_COMPONENT_DIRS})
    if(IS_DIRECTORY ${dw_dir})
        get_filename_component(dw_comp ${dw_dir} NAME)
        if(TARGET __idf_${dw_comp})
            target_compile_options(__idf_${dw_comp} PRIVATE -Wall -Wextra -Werror)
            math(EXPR DW_WERROR_APPLIED "${DW_WERROR_APPLIED} + 1")
        endif()
    endif()
endforeach()
if(NOT DW_WERROR_APPLIED EQUAL 0)
    message(STATUS "-Wall -Wextra -Werror applied to ${DW_WERROR_APPLIED} dw_* components")
endif()
