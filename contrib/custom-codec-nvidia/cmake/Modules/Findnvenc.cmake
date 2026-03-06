# Copyright 2026 Yuan Tong. All rights reserved.
# SPDX-License-Identifier: BSD-2-Clause

# - Try to find nvenc Header
# Once done this will define
#
#  NVENC_FOUND - system has nvenc header
#  NVENC_INCLUDE_DIR - the nvenc header include directory

find_path(NVENC_INCLUDE_DIR
          NAMES nvEncodeAPI.h
          PATH_SUFFIXES ffnvcodec
)

function(_nvenc_get_version)
    unset(NVENC_VERSION_STRING PARENT_SCOPE)
    set(_hdr_file "${NVENC_INCLUDE_DIR}/nvEncodeAPI.h")

    if(NOT EXISTS "${_hdr_file}")
        return()
    endif()

    file(STRINGS "${_hdr_file}" VERSION_STRINGS REGEX "#define NVENCAPI_.+_VERSION [0-9]+")

    foreach(TYPE MAJOR MINOR)
        string(REGEX MATCH "NVENCAPI_${TYPE}_VERSION [0-9]+" NVENC_TYPE_STRING ${VERSION_STRINGS})
        string(REGEX MATCH "[0-9]+" NVENC_VERSION_${TYPE} ${NVENC_TYPE_STRING})
    endforeach(TYPE)

    set(NVENC_VERSION_MAJOR ${NVENC_VERSION_MAJOR} PARENT_SCOPE)
    set(NVENC_VERSION_MINOR ${NVENC_VERSION_MINOR} PARENT_SCOPE)

    set(NVENC_VERSION_STRING "${NVENC_VERSION_MAJOR}.${NVENC_VERSION_MINOR}" PARENT_SCOPE)
endfunction(_nvenc_get_version)

_nvenc_get_version()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(nvenc REQUIRED_VARS NVENC_INCLUDE_DIR VERSION_VAR NVENC_VERSION_STRING)

# show the NVENC_INCLUDE_DIR variable only in the advanced view
mark_as_advanced(NVENC_INCLUDE_DIR)

if(NVENC_INCLUDE_DIR)
    add_library(nvenc INTERFACE IMPORTED GLOBAL)
    target_include_directories(nvenc INTERFACE "${NVENC_INCLUDE_DIR}")
endif()
