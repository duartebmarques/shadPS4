# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

option(ENABLE_GR2_FSR3 "Build the Gravity Rush 2 FSR 3.1 and FSR 4 upscaler module" ON)

if (ENABLE_GR2_FSR3 AND NOT APPLE)
    set(GR2_FSR3_DIR ${CMAKE_CURRENT_LIST_DIR})
    set(GR2_FFX_ROOT ${PROJECT_SOURCE_DIR}/third_party/fsr-vulkan)
    set(GR2_FFX_SDK ${GR2_FFX_ROOT}/upstream/ffx-1.1.4/sdk)

    # The vendored FidelityFX SDK 1.1.4 with the FSR 3.1 upscaler as its only effect, its Vulkan
    # backend and the portable C API.
    add_library(gr2_ffx_fsr3_host STATIC
        ${GR2_FFX_SDK}/src/shared/ffx_assert.cpp
        ${GR2_FFX_SDK}/src/shared/ffx_message.cpp
        ${GR2_FFX_SDK}/src/shared/ffx_object_management.cpp
        ${GR2_FFX_SDK}/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp)
    target_include_directories(gr2_ffx_fsr3_host
        PUBLIC ${GR2_FFX_SDK}/include
        PRIVATE ${GR2_FFX_SDK}/src/shared ${GR2_FFX_SDK}/src/components)
    target_compile_definitions(gr2_ffx_fsr3_host PRIVATE DISABLE_FFX_DEBUG_BREAK)

    add_library(gr2_ffx_fsr3_vk STATIC
        ${GR2_FFX_SDK}/src/backends/vk/ffx_vk.cpp
        ${GR2_FFX_SDK}/src/backends/shared/ffx_shader_blobs.cpp
        ${GR2_FFX_SDK}/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp
        ${GR2_FFX_SDK}/src/shared/ffx_breadcrumbs_list.cpp)
    target_include_directories(gr2_ffx_fsr3_vk PRIVATE
        ${GR2_FFX_SDK}/src/shared
        ${GR2_FFX_SDK}/src/components
        ${GR2_FFX_SDK}/src/backends/shared
        ${GR2_FFX_SDK}/src/backends/shared/blob_accessors
        ${GR2_FFX_ROOT}/generated/ffx-1.1.4/vk/fsr3upscaler)
    target_compile_definitions(gr2_ffx_fsr3_vk PRIVATE FFX_FSR3UPSCALER=1 DISABLE_FFX_DEBUG_BREAK)
    target_link_libraries(gr2_ffx_fsr3_vk PUBLIC gr2_ffx_fsr3_host Vulkan::Headers)

    add_library(gr2_ffx_portable STATIC
        ${GR2_FFX_ROOT}/src/ffx_vk_portable.c
        ${GR2_FFX_ROOT}/src/ffx_vk_fsr3_portable.cpp)
    set_target_properties(gr2_ffx_portable PROPERTIES C_STANDARD 11)
    target_include_directories(gr2_ffx_portable
        PUBLIC ${GR2_FFX_ROOT}/include
        PRIVATE ${GR2_FFX_SDK}/include)
    target_compile_definitions(gr2_ffx_portable PRIVATE
        FFX_VK_PORTABLE_HAS_FSR3_UPSCALER=1
        FFX_VK_PORTABLE_NO_FRAME_GENERATION=1
        DISABLE_FFX_DEBUG_BREAK)
    target_link_libraries(gr2_ffx_portable PUBLIC Vulkan::Headers PRIVATE gr2_ffx_fsr3_vk)

    if (MSVC)
        set(GR2_FFX_NO_WARNINGS /w)
    else()
        set(GR2_FFX_NO_WARNINGS -w)
    endif()
    foreach(GR2_FFX_TARGET gr2_ffx_fsr3_host gr2_ffx_fsr3_vk gr2_ffx_portable)
        set_target_properties(${GR2_FFX_TARGET} PROPERTIES
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
        target_compile_options(${GR2_FFX_TARGET} PRIVATE ${GR2_FFX_NO_WARNINGS})
    endforeach()

    # The SDK sources call MSVC CRT functions, which this header supplies on other platforms.
    if (NOT WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        foreach(GR2_FFX_TARGET gr2_ffx_fsr3_host gr2_ffx_fsr3_vk)
            target_compile_options(${GR2_FFX_TARGET} PRIVATE
                "SHELL:-include \"${GR2_FFX_ROOT}/src/ffx_1_1_4_portability.hpp\"")
        endforeach()
    endif()

    # The source-v07 FSR 4 INT8 provider and the module's C bridge to it. Only C sources include
    # the provider headers: their FFX type names collide with the SDK headers of FSR 3.1.
    add_library(gr2_ffx_fsr4_v07 STATIC
        ${GR2_FFX_ROOT}/src/ffx_vk_fsr4_v07.c
        ${GR2_FFX_ROOT}/src/ffx_vk_fsr4_v07_assets.c
        ${GR2_FFX_ROOT}/src/ffx_vk_fsr4_v07_provider.c
        ${GR2_FFX_ROOT}/src/ffx_vk_fsr4_v07_schedule.c)
    target_include_directories(gr2_ffx_fsr4_v07 PUBLIC ${GR2_FFX_ROOT}/include)
    # Other threads own queues of the device, and the module destroys a context only after the
    # GPU has finished its work.
    target_compile_definitions(gr2_ffx_fsr4_v07 PRIVATE FFX_FSR4_VK_HOST_RETIRES_WORK=1)
    target_compile_options(gr2_ffx_fsr4_v07 PRIVATE ${GR2_FFX_NO_WARNINGS})
    target_link_libraries(gr2_ffx_fsr4_v07 PUBLIC Vulkan::Headers)
    if (UNIX)
        target_link_libraries(gr2_ffx_fsr4_v07 PRIVATE m)
    endif()

    add_library(gr2_fsr4_bridge STATIC ${GR2_FSR3_DIR}/fsr4_v07_bridge.c)
    target_link_libraries(gr2_fsr4_bridge PUBLIC Vulkan::Headers PRIVATE gr2_ffx_fsr4_v07)
    set_target_properties(gr2_ffx_fsr4_v07 gr2_fsr4_bridge PROPERTIES C_STANDARD 11)

    # Helper shaders, compiled like the host shaders.
    if (TARGET glslang-standalone)
        set(GR2_FSR3_GLSLANG glslang-standalone)
    elseif (TARGET glslang::glslang-standalone)
        set(GR2_FSR3_GLSLANG glslang::glslang-standalone)
    endif()
    set(GR2_FSR3_HEADER_SCRIPT
        ${PROJECT_SOURCE_DIR}/src/video_core/host_shaders/AddShaderHeaderInclude.cmake)
    set(GR2_FSR3_SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/gr2_fsr3_shaders/include)
    set(GR2_FSR3_SHADER_DIR ${GR2_FSR3_SHADER_INCLUDE}/video_core/gr2_fsr3/shaders)
    set(GR2_FSR3_SHADER_HEADERS "")
    function(gr2_fsr3_add_shader FILENAME SHADER_NAME)
        set(HEADER_FILE ${GR2_FSR3_SHADER_DIR}/${SHADER_NAME}.h)
        set(DEPFILE ${HEADER_FILE}.d)
        string(TOUPPER ${SHADER_NAME} SYMBOL)
        add_custom_command(
            OUTPUT ${HEADER_FILE}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${GR2_FSR3_SHADER_DIR}
            COMMAND ${GR2_FSR3_GLSLANG} --target-env vulkan1.3
                    -o ${HEADER_FILE} --vn ${SYMBOL} --depfile ${DEPFILE}
                    ${GR2_FSR3_DIR}/shaders/${FILENAME}
            COMMAND ${CMAKE_COMMAND} "-DHEADER_FILE=${HEADER_FILE}" -P ${GR2_FSR3_HEADER_SCRIPT}
            DEPENDS ${GR2_FSR3_GLSLANG} ${GR2_FSR3_DIR}/shaders/${FILENAME}
                    ${GR2_FSR3_HEADER_SCRIPT}
            DEPFILE ${DEPFILE}
            BYPRODUCTS ${DEPFILE}
            COMMENT "Building GR2 FSR3 shader ${FILENAME}"
            VERBATIM)
        set(GR2_FSR3_SHADER_HEADERS ${GR2_FSR3_SHADER_HEADERS} ${HEADER_FILE} PARENT_SCOPE)
    endfunction()
    gr2_fsr3_add_shader(fsr3_mask.comp fsr3_mask_comp)
    gr2_fsr3_add_shader(fsr3_mvcopy.comp fsr3_mvcopy_comp)
    gr2_fsr3_add_shader(fsr3_store.comp fsr3_store_comp)
    gr2_fsr3_add_shader(fsr3_fallback.comp fsr3_fallback_comp)
    gr2_fsr3_add_shader(fsr3_debug.comp fsr3_debug_comp)
    gr2_fsr3_add_shader(fsr4_inputs.comp fsr4_inputs_comp)
    gr2_fsr3_add_shader(grr_camera_motion.comp grr_camera_motion_comp)
    add_custom_target(gr2_fsr3_shaders
        DEPENDS ${GR2_FSR3_SHADER_HEADERS}
        SOURCES
            ${GR2_FSR3_DIR}/shaders/fsr3_color.glsl
            ${GR2_FSR3_DIR}/shaders/fsr3_debug.comp
            ${GR2_FSR3_DIR}/shaders/fsr3_fallback.comp
            ${GR2_FSR3_DIR}/shaders/fsr3_mask.comp
            ${GR2_FSR3_DIR}/shaders/fsr3_mvcopy.comp
            ${GR2_FSR3_DIR}/shaders/fsr3_store.comp
            ${GR2_FSR3_DIR}/shaders/fsr4_inputs.comp
            ${GR2_FSR3_DIR}/shaders/grr_camera_motion.comp)

    # vk_trampolines.cpp defines the Vulkan commands that the FidelityFX libraries and the FSR 4 v07
    # provider call.
    target_sources(shadps4 PRIVATE
        ${GR2_FSR3_DIR}/context_images.cpp
        ${GR2_FSR3_DIR}/context_images.h
        ${GR2_FSR3_DIR}/dlss_context.cpp
        ${GR2_FSR3_DIR}/dlss_context.h
        ${GR2_FSR3_DIR}/dlss_streamline.h
        ${GR2_FSR3_DIR}/fsr3_context.cpp
        ${GR2_FSR3_DIR}/fsr3_context.h
        ${GR2_FSR3_DIR}/fsr3_inputs.cpp
        ${GR2_FSR3_DIR}/fsr3_inputs.h
        ${GR2_FSR3_DIR}/fsr3_jitter.cpp
        ${GR2_FSR3_DIR}/fsr3_jitter.h
        ${GR2_FSR3_DIR}/fsr3_passes.cpp
        ${GR2_FSR3_DIR}/fsr3_passes.h
        ${GR2_FSR3_DIR}/fsr3_settings.cpp
        ${GR2_FSR3_DIR}/fsr3_settings.h
        ${GR2_FSR3_DIR}/fsr3_types.h
        ${GR2_FSR3_DIR}/fsr4_assets.cpp
        ${GR2_FSR3_DIR}/fsr4_assets.h
        ${GR2_FSR3_DIR}/fsr4_context.cpp
        ${GR2_FSR3_DIR}/fsr4_context.h
        ${GR2_FSR3_DIR}/fsr4_device.cpp
        ${GR2_FSR3_DIR}/fsr4_device.h
        ${GR2_FSR3_DIR}/fsr4_v07_bridge.h
        ${GR2_FSR3_DIR}/fsr_worker.cpp
        ${GR2_FSR3_DIR}/fsr_worker.h
        ${GR2_FSR3_DIR}/gr2_fsr3.cpp
        ${GR2_FSR3_DIR}/gr2_fsr3.h
        ${GR2_FSR3_DIR}/grr_capture.cpp
        ${GR2_FSR3_DIR}/grr_capture.h
        ${GR2_FSR3_DIR}/grr_fsr3.cpp
        ${GR2_FSR3_DIR}/grr_survey.cpp
        ${GR2_FSR3_DIR}/grr_survey.h
        ${GR2_FSR3_DIR}/vk_trampolines.cpp)
    # Streamline's MIT headers. The Streamline and DLSS libraries load at run time from the dlss
    # folder of the user folder.
    if (WIN32)
        target_sources(shadps4 PRIVATE ${GR2_FSR3_DIR}/dlss_streamline.cpp)
        target_include_directories(shadps4 PRIVATE
            ${PROJECT_SOURCE_DIR}/third_party/streamline/include)
    endif()
    target_include_directories(shadps4 PRIVATE ${GR2_FSR3_SHADER_INCLUDE})
    add_dependencies(shadps4 gr2_fsr3_shaders)
    target_link_libraries(shadps4 PRIVATE gr2_ffx_portable gr2_fsr4_bridge)
    target_compile_definitions(shadps4 PRIVATE ENABLE_GR2_FSR3=1)
endif()
