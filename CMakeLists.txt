cmake_minimum_required(VERSION 3.20)
project(GpuComputeBenchmark LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Options ----
option(ENABLE_VULKAN "Build with Vulkan backend" ON)
if(WIN32)
    option(ENABLE_DX12  "Build with DirectX 12 backend (Windows only)" ON)
    option(ENABLE_DX11  "Build with DirectX 11 backend (Windows only)" ON)
else()
    option(ENABLE_DX12  "Build with DirectX 12 backend (Windows only)" OFF)
    option(ENABLE_DX11  "Build with DirectX 11 backend (Windows only)" OFF)
endif()
if(APPLE)
    option(ENABLE_METAL "Build with Metal backend (macOS only)" ON)
else()
    option(ENABLE_METAL "Build with Metal backend (macOS only)" OFF)
endif()
option(ENABLE_OPENGL "Build with OpenGL 4.3 backend" ON)

# ---- GLFW (required by all backends for windowing) ----
find_package(glfw3 REQUIRED)

# ---- Common source files ----
set(COMMON_SOURCES
    src/main.cpp
    src/app_base.cpp
    src/benchmark_results.cpp
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

# ---- Metal backend (macOS only) ----
if(ENABLE_METAL AND APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

    find_library(METAL_FRAMEWORK Metal)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore)
    find_library(COCOA_FRAMEWORK Cocoa)

    if(METAL_FRAMEWORK)
        list(APPEND BACKEND_SOURCES src/metal_backend.mm src/metal_probe.mm)
        list(APPEND BACKEND_LIBS
            ${METAL_FRAMEWORK} ${QUARTZCORE_FRAMEWORK} ${COCOA_FRAMEWORK})
        list(APPEND BACKEND_DEFS HAVE_METAL)

        set_source_files_properties(src/metal_backend.mm src/metal_probe.mm
            PROPERTIES COMPILE_FLAGS "-fobjc-arc"
        )

        message(STATUS "Metal backend:  ENABLED")
    else()
        message(STATUS "Metal backend:  DISABLED (Metal framework not found)")
    endif()
elseif(ENABLE_METAL AND NOT APPLE)
    message(STATUS "Metal backend:  DISABLED (not macOS)")
else()
    message(STATUS "Metal backend:  DISABLED (by option)")
endif()

# ---- OpenGL backend (cross-platform, lowest API priority) ----
if(ENABLE_OPENGL)
    find_package(OpenGL QUIET)
    if(OpenGL_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            glad
            GIT_REPOSITORY https://github.com/Dav1dde/glad.git
            GIT_TAG        v2.0.8
            SOURCE_SUBDIR  cmake
        )
        FetchContent_MakeAvailable(glad)
        glad_add_library(glad_gl43 STATIC API gl:core=4.3)

        list(APPEND BACKEND_SOURCES src/opengl_backend.cpp)
        list(APPEND BACKEND_LIBS    OpenGL::GL glad_gl43)
        list(APPEND BACKEND_DEFS    HAVE_OPENGL)
        message(STATUS "OpenGL backend: ENABLED")
    else()
        message(STATUS "OpenGL backend: DISABLED (OpenGL not found)")
    endif()
else()
    message(STATUS "OpenGL backend: DISABLED (by option)")
endif()

# ---- Executable ----
add_executable(gpu_benchmark ${COMMON_SOURCES} ${BACKEND_SOURCES})

target_link_libraries(gpu_benchmark PRIVATE glfw ${BACKEND_LIBS})
target_compile_definitions(gpu_benchmark PRIVATE ${BACKEND_DEFS})

if(MSVC)
    target_compile_options(gpu_benchmark PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(gpu_benchmark PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
        $<$<COMPILE_LANGUAGE:OBJCXX>:-Wall -Wextra>
    )
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

# ---- Copy Metal shader source file next to executable (macOS) ----
if(ENABLE_METAL AND APPLE AND METAL_FRAMEWORK)
    add_custom_command(TARGET gpu_benchmark POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/shaders/particle.metal
            $<TARGET_FILE_DIR:gpu_benchmark>
        COMMENT "Copying Metal shader file next to executable"
    )
endif()

# ---- Copy OpenGL GLSL shader files next to executable ----
if(ENABLE_OPENGL AND OpenGL_FOUND)
    set(GL_SHADERS
        ${CMAKE_SOURCE_DIR}/shaders/compute_gl.comp
        ${CMAKE_SOURCE_DIR}/shaders/particle_gl.vert
        ${CMAKE_SOURCE_DIR}/shaders/particle_gl.frag
    )
    add_custom_command(TARGET gpu_benchmark POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GL_SHADERS} $<TARGET_FILE_DIR:gpu_benchmark>
        COMMENT "Copying OpenGL GLSL shader files next to executable"
    )
endif()
