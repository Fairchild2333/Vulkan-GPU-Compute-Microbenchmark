cmake_minimum_required(VERSION 3.16)
project(gpubench)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_definitions(-DVK_USE_PLATFORM_OHOS=1)

add_library(gpubench SHARED
    napi_init.cpp
    vulkan_renderer.cpp
)

target_include_directories(gpubench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(gpubench PRIVATE
    libace_ndk.z.so
    libnative_window.so
    libvulkan.so
    libhilog_ndk.z.so
)
