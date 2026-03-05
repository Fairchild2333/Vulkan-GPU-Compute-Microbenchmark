cmake_minimum_required(VERSION 3.20)
project(GpuComputeBenchmark LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Options ----
option(ENABLE_VULKAN "Build with Vulkan backend" ON)
option(ENABLE_DX12   "Build with DirectX 12 backend (Windows only)" WIN32)
option(ENABLE_DX11   "Build with DirectX 11 backend (Windows only)" WIN32)

# ---- GLFW (required by all backends for windowing) ----
find_package(glfw3 REQUIRED)

# ---- Common source files ----
set(COMMON_SOURCES
    src/main.cpp
    src/app_base.cpp
)

set(BACKEND_SOURCES "")
set(BACKEND_LIBS "")
set(BACKEND_DEFS "")

# ---- Vulkan backend (optional) ----
if(ENABLE_VULKAN)
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        list(APPEND BACKEND_SOURCES src/vulkan_backend.cpp)
        list(APPEND BACKEND_LIBS    Vulkan::Vulkan)
        list(APPEND BACKEND_DEFS    HAVE_VULKAN GLFW_INCLUDE_VULKAN)
        message(STATUS "Vulkan backend: ENABLED")
    else()
        message(STATUS "Vulkan backend: DISABLED (Vulkan SDK not found)")
    endif()
else()
    message(STATUS "Vulkan backend: DISABLED (by option)")
endif()

# ---- DirectX 12 backend (Windows only) ----
if(ENABLE_DX12 AND WIN32)
    list(APPEND BACKEND_SOURCES src/dx12_backend.cpp)
    list(APPEND BACKEND_LIBS    d3d12 dxgi d3dcompiler)
    list(APPEND BACKEND_DEFS    HAVE_DX12)
    message(STATUS "DX12 backend:   ENABLED")
elseif(ENABLE_DX12 AND NOT WIN32)
    message(STATUS "DX12 backend:   DISABLED (not Windows)")
else()
    message(STATUS "DX12 backend:   DISABLED (by option)")
endif()

# ---- DirectX 11 backend (Windows only) ----
if(ENABLE_DX11 AND WIN32)
    list(APPEND BACKEND_SOURCES src/dx11_backend.cpp)
    list(APPEND BACKEND_LIBS    d3d11 dxgi d3dcompiler)
    list(APPEND BACKEND_DEFS    HAVE_DX11)
    message(STATUS "DX11 backend:   ENABLED")
elseif(ENABLE_DX11 AND NOT WIN32)
    message(STATUS "DX11 backend:   DISABLED (not Windows)")
else()
    message(STATUS "DX11 backend:   DISABLED (by option)")
endif()

# ---- Executable ----
add_executable(gpu_benchmark ${COMMON_SOURCES} ${BACKEND_SOURCES})

target_link_libraries(gpu_benchmark PRIVATE glfw ${BACKEND_LIBS})
target_compile_definitions(gpu_benchmark PRIVATE ${BACKEND_DEFS})

if(MSVC)
    target_compile_options(gpu_benchmark PRIVATE /W4 /permissive-)
else()
    target_compile_options(gpu_benchmark PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---- Shader compilation: GLSL -> SPIR-V (Vulkan) ----
if(ENABLE_VULKAN AND Vulkan_FOUND)
    find_program(GLSLC_EXECUTABLE glslc)
    if(GLSLC_EXECUTABLE)
        set(GLSL_SOURCES
            ${CMAKE_SOURCE_DIR}/shaders/particle.vert
            ${CMAKE_SOURCE_DIR}/shaders/particle.frag
            ${CMAKE_SOURCE_DIR}/shaders/compute.comp
        )
        set(SPV_OUTPUTS "")
        foreach(SHADER ${GLSL_SOURCES})
            get_filename_component(SHADER_NAME ${SHADER} NAME)
            set(SPV_FILE ${CMAKE_BINARY_DIR}/${SHADER_NAME}.spv)
            add_custom_command(
                OUTPUT  ${SPV_FILE}
                COMMAND ${GLSLC_EXECUTABLE} ${SHADER} -o ${SPV_FILE}
                DEPENDS ${SHADER}
                COMMENT "Compiling ${SHADER_NAME} -> SPIR-V"
            )
            list(APPEND SPV_OUTPUTS ${SPV_FILE})
        endforeach()

        add_custom_target(compile_glsl ALL DEPENDS ${SPV_OUTPUTS})
        add_dependencies(gpu_benchmark compile_glsl)

        add_custom_command(TARGET gpu_benchmark POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SPV_OUTPUTS} $<TARGET_FILE_DIR:gpu_benchmark>
            COMMENT "Copying SPIR-V files next to executable"
        )
    else()
        message(WARNING "glslc not found – Vulkan shader compilation disabled.")
    endif()
endif()

# ---- Copy HLSL shader source files next to executable (DX12/DX11) ----
if((ENABLE_DX12 AND WIN32) OR (ENABLE_DX11 AND WIN32))
    set(HLSL_SOURCES
        ${CMAKE_SOURCE_DIR}/shaders/compute.hlsl
        ${CMAKE_SOURCE_DIR}/shaders/particle_vs.hlsl
        ${CMAKE_SOURCE_DIR}/shaders/particle_ps.hlsl
    )
    add_custom_command(TARGET gpu_benchmark POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${HLSL_SOURCES} $<TARGET_FILE_DIR:gpu_benchmark>
        COMMENT "Copying HLSL shader files next to executable"
    )
endif()
