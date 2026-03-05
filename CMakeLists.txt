cmake_minimum_required(VERSION 3.20)
project(VulkanGpuComputeRenderer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Vulkan REQUIRED)
find_package(glfw3 REQUIRED)

add_executable(vulkan_gpu_pipeline
    src/main.cpp
)

target_link_libraries(vulkan_gpu_pipeline PRIVATE Vulkan::Vulkan glfw)

target_compile_definitions(vulkan_gpu_pipeline PRIVATE GLFW_INCLUDE_VULKAN)

if (MSVC)
    target_compile_options(vulkan_gpu_pipeline PRIVATE /W4 /permissive-)
else()
    target_compile_options(vulkan_gpu_pipeline PRIVATE -Wall -Wextra -Wpedantic)
endif()

find_program(GLSLC_EXECUTABLE glslc)
if (GLSLC_EXECUTABLE)
    add_custom_target(compile_shaders ALL
        COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_SOURCE_DIR}/shaders/particle.vert -o ${CMAKE_BINARY_DIR}/particle.vert.spv
        COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_SOURCE_DIR}/shaders/particle.frag -o ${CMAKE_BINARY_DIR}/particle.frag.spv
        COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_SOURCE_DIR}/shaders/compute.comp -o ${CMAKE_BINARY_DIR}/compute.comp.spv
        COMMENT "Compiling GLSL shaders to SPIR-V"
    )
    add_dependencies(vulkan_gpu_pipeline compile_shaders)
else()
    message(WARNING "glslc not found. Shader compilation target is disabled.")
endif()
