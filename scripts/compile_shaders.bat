@echo off
setlocal

where glslc >nul 2>&1
if errorlevel 1 (
    echo [error] glslc not found. Install Vulkan SDK and ensure glslc is in PATH.
    exit /b 1
)

glslc shaders\particle.vert -o build\particle.vert.spv
if errorlevel 1 exit /b 1

glslc shaders\particle.frag -o build\particle.frag.spv
if errorlevel 1 exit /b 1

glslc shaders\compute.comp -o build\compute.comp.spv
if errorlevel 1 exit /b 1

echo Shader compilation complete.
