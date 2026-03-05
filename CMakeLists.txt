cmake_minimum_required(VERSION 3.20)
project(VulkanGpuComputeRenderer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Vulkan SDK provides headers, libraries, and the glslc shader compiler.
# GLFW is used for cross-platform window creation and Vulkan surface setup.
find_package(Vulkan REQUIRED)
find_package(glfw3 REQUIRED)

add_executable(vulkan_gpu_pipeline
    src/main.cpp
)

target_link_libraries(vulkan_gpu_pipeline PRIVATE Vulkan::Vulkan glfw)

# GLFW_INCLUDE_VULKAN makes glfw3.h pull in vulkan.h automatically.
target_compile_definitions(vulkan_gpu_pipeline PRIVATE GLFW_INCLUDE_VULKAN)

if (MSVC)
    target_compile_options(vulkan_gpu_pipeline PRIVATE /W4 /permissive-)
else()
    target_compile_options(vulkan_gpu_pipeline PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------- Shader compilation (GLSL -> SPIR-V) ----------
# Each .glsl source gets its own custom command with a DEPENDS clause so
# CMake only recompiles a shader when its source file changes.  A post-build
# step copies the resulting .spv files next to the executable so the app can
# locate them via argv[0] regardless of the working directory.
find_program(GLSLC_EXECUTABLE glslc)
if (GLSLC_EXECUTABLE)
    set(SHADER_SOURCES
        ${CMAKE_SOURCE_DIR}/shaders/particle.vert
        ${CMAKE_SOURCE_DIR}/shaders/particle.frag
        ${CMAKE_SOURCE_DIR}/shaders/compute.comp
    )

    set(SPV_OUTPUTS "")
    foreach(SHADER ${SHADER_SOURCES})
        get_filename_component(SHADER_NAME ${SHADER} NAME)
        set(SPV_FILE ${CMAKE_BINARY_DIR}/${SHADER_NAME}.spv)
        add_custom_command(
            OUTPUT ${SPV_FILE}
            COMMAND ${GLSLC_EXECUTABLE} ${SHADER} -o ${SPV_FILE}
            DEPENDS ${SHADER}
            COMMENT "Compiling ${SHADER_NAME} -> SPIR-V"
        )
        list(APPEND SPV_OUTPUTS ${SPV_FILE})
    endforeach()

    add_custom_target(compile_shaders ALL DEPENDS ${SPV_OUTPUTS})
    add_dependencies(vulkan_gpu_pipeline compile_shaders)

    # Copy compiled SPIR-V files next to the executable for runtime loading.
    add_custom_command(TARGET vulkan_gpu_pipeline POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SPV_OUTPUTS} $<TARGET_FILE_DIR:vulkan_gpu_pipeline>
        COMMENT "Copying SPIR-V files next to executable"
    )
else()
    message(WARNING "glslc not found. Shader compilation target is disabled.")
endif()
