# Copyright 2026 Yuan Tong. All rights reserved.
# SPDX-License-Identifier: BSD-2-Clause

option(AVIF_BUILD_CUSTOM_CODEC_NVENC "Build NVENC Custom Encoder" OFF)
option(AVIF_BUILD_CUSTOM_CODEC_NVENC_GTESTS "Build NVENC custom codec gtests" OFF)
option(AVIF_BUILD_CUSTOM_CODEC_NVENC_BENCHMARK "Build NVENC custom codec benchmark utility" OFF)

if(AVIF_BUILD_CUSTOM_CODEC_NVENC OR AVIF_BUILD_CUSTOM_CODEC_NVDEC)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Modules")
endif()

if(AVIF_BUILD_CUSTOM_CODEC_NVENC)
    if(WIN32 AND NOT BUILD_SHARED_LIBS)
        message(FATAL_ERROR "AVIF_BUILD_CUSTOM_CODEC_NVENC requires BUILD_SHARED_LIBS=ON.")
    endif()

    enable_language(CXX)
    find_package(nvenc)

    set(CUSTOM_CODEC_NVENC_SRC src/exception.cpp src/dyn_loader.cpp src/cuda_driver.cpp src/nvenc.cpp src/codec_options.cpp
                               src/scheduler.cpp src/runtime.cpp src/session.cpp src/codec_wrapper.cpp src/avif_nvenc_codec.cpp)

    add_library(custom-codec-nvenc_obj OBJECT ${CUSTOM_CODEC_NVENC_SRC})
    target_compile_features(custom-codec-nvenc_obj PRIVATE cxx_std_20)
    target_compile_definitions(custom-codec-nvenc_obj PRIVATE AVIF_NVENC_DLL AVIF_NVENC_BUILDING_SHARED_LIBS)
    target_include_directories(custom-codec-nvenc_obj PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(custom-codec-nvenc_obj PRIVATE nvenc avif yuv::yuv)

    add_library(custom-codec-nvenc SHARED)
    target_link_libraries(custom-codec-nvenc PRIVATE custom-codec-nvenc_obj nvenc avif yuv::yuv)
    target_compile_definitions(custom-codec-nvenc PUBLIC AVIF_NVENC_DLL)
    target_include_directories(
        custom-codec-nvenc PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    set_target_properties(
        custom-codec-nvenc
        PROPERTIES CXX_VISIBILITY_PRESET hidden
                   VISIBILITY_INLINES_HIDDEN YES
                   RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
                   OUTPUT_NAME "avif_nvenc"
    )

    add_library(custom-codec-nvenc_internal STATIC)
    target_link_libraries(custom-codec-nvenc_internal PRIVATE custom-codec-nvenc_obj yuv::yuv)
    target_link_libraries(custom-codec-nvenc_internal PUBLIC nvenc avif)
    target_include_directories(custom-codec-nvenc_internal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(custom-codec-nvenc_internal PUBLIC AVIF_NVENC_USING_STATIC_LIBS)

    if(NOT SKIP_INSTALL_LIBRARIES AND NOT SKIP_INSTALL_ALL)
        install(
            TARGETS custom-codec-nvenc
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
            ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        )
    endif()
    if(NOT SKIP_INSTALL_HEADERS AND NOT SKIP_INSTALL_ALL)
        install(FILES include/avif_nvenc_codec.h DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/avif")
    endif()

    if(AVIF_BUILD_CUSTOM_CODEC_NVENC_GTESTS OR AVIF_BUILD_CUSTOM_CODEC_NVENC_BENCHMARK)
        add_library(custom-codec-nvenc-test-helpers OBJECT tests/gtest_helpers.cpp)
        target_compile_features(custom-codec-nvenc-test-helpers PRIVATE cxx_std_20)
        target_include_directories(custom-codec-nvenc-test-helpers PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_link_libraries(custom-codec-nvenc-test-helpers PRIVATE custom-codec-nvenc_internal)
    endif()

    if(AVIF_BUILD_CUSTOM_CODEC_NVENC_BENCHMARK)
        include(LocalLibargparse)
        if(NOT TARGET avif_apps)
            message(FATAL_ERROR "AVIF_BUILD_CUSTOM_CODEC_NVENC_BENCHMARK requires AVIF_BUILD_APPS=ON so avif_apps is available.")
        endif()

        add_executable(custom-codec-nvenc-benchmark tests/nvenc_benchmark.cc)
        target_compile_features(custom-codec-nvenc-benchmark PRIVATE cxx_std_20)
        target_include_directories(
            custom-codec-nvenc-benchmark
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/tests
        )
        target_link_libraries(custom-codec-nvenc-benchmark PRIVATE custom-codec-nvenc_internal libargparse avif_apps)
        target_sources(custom-codec-nvenc-benchmark PRIVATE $<TARGET_OBJECTS:custom-codec-nvenc-test-helpers>)
        set_target_properties(custom-codec-nvenc-benchmark PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    endif()

    if(AVIF_BUILD_CUSTOM_CODEC_NVENC_GTESTS)
        if(NOT AVIF_BUILD_TESTS)
            message(FATAL_ERROR "AVIF_BUILD_CUSTOM_CODEC_NVENC_GTESTS requires AVIF_BUILD_TESTS=ON.")
        endif()
        if(NOT AVIF_GTEST)
            message(FATAL_ERROR "AVIF_BUILD_CUSTOM_CODEC_NVENC_GTESTS requires AVIF_GTEST=ON.")
        endif()

        check_avif_option(AVIF_GTEST TARGET GTest::GTest PKG_NAME GTest)

        function(add_custom_codec_nvenc_gtest TARGET_NAME)
            add_executable(${TARGET_NAME} ${ARGN})
            target_compile_features(${TARGET_NAME} PRIVATE cxx_std_20)
            target_include_directories(
                ${TARGET_NAME}
                PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests
            )
            target_link_libraries(${TARGET_NAME} PRIVATE custom-codec-nvenc_internal GTest::GTest GTest::Main)
            set_target_properties(${TARGET_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
            add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})
        endfunction()

        add_custom_codec_nvenc_gtest(custom-codec-nvenc-schedulertest tests/nvenc_schedulertest.cc)
        add_custom_codec_nvenc_gtest(custom-codec-nvenc-integrationtest tests/nvenc_integrationtest.cc)
        target_sources(custom-codec-nvenc-integrationtest PRIVATE $<TARGET_OBJECTS:custom-codec-nvenc-test-helpers>)
    endif()
endif ()
