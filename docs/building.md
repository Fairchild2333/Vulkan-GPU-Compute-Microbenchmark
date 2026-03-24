# Building from Source

## Prerequisites

| Dependency | Install |
|---|---|
| **CMake 3.20+** | https://cmake.org/download/ or system package manager |
| **C++17 compiler** | MSVC (Visual Studio 2019+), GCC 8+, Clang, or Apple Clang |
| **Python 3** | Required by [GLAD](https://github.com/Dav1dde/glad) (OpenGL loader generator) at build time. [python.org](https://www.python.org/downloads/) / `sudo apt install python3` / `brew install python` |
| **GLFW** | `vcpkg install glfw3` / `brew install glfw` / `sudo apt install libglfw3-dev` |
| **Vulkan SDK** (optional) | [LunarG](https://vulkan.lunarg.com/sdk/home) or `sudo apt install libvulkan-dev` |
| **Windows SDK** (for DX) | Included with Visual Studio |
| **Xcode CLT** (for Metal) | `xcode-select --install` (macOS) |

---

## Linux

> **Tested on Ubuntu.** Fedora and Arch commands are provided for
> convenience but have not been verified by the author.

**Ubuntu / Debian** (`apt`):

```bash
sudo apt install build-essential cmake libglfw3-dev libgl-dev
sudo apt install libvulkan-dev vulkan-tools glslc   # optional, for Vulkan backend
```

**Fedora / RHEL** (`dnf`):

```bash
sudo dnf install gcc-c++ cmake glfw-devel mesa-libGL-devel
sudo dnf install vulkan-loader-devel vulkan-tools glslc   # optional, for Vulkan backend
```

**Arch / Manjaro** (`pacman`):

```bash
sudo pacman -S base-devel cmake glfw-x11 mesa
sudo pacman -S vulkan-icd-loader vulkan-tools shaderc   # optional, for Vulkan backend
```

At least one of the OpenGL (`libgl-dev` / `mesa-libGL-devel` / `mesa`) or
Vulkan development packages must be installed — otherwise no backend will be
available. DirectX and Metal backends are automatically disabled on Linux.

| Backend | Available on Linux | Driver Requirement |
|---------|-------------------|--------------------|
| Vulkan  | Yes (with `libvulkan-dev`) | Mesa or NVIDIA proprietary driver |
| OpenGL 4.3 | Yes (with `libgl-dev`) | Mesa or NVIDIA proprietary driver |
| DirectX 11/12 | No | Windows only |
| Metal | No | macOS only |

**GPU selection for OpenGL:** On Linux, the application uses the `DRI_PRIME`
environment variable to route OpenGL to the user's chosen GPU. This is set
automatically when a GPU is selected via the interactive menu or `--gpu`.
You can also set it manually:

```bash
DRI_PRIME=1 ./build/gpu_benchmark --backend opengl   # use secondary GPU
```

For NVIDIA proprietary drivers, use:

```bash
__NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia ./build/gpu_benchmark --backend opengl
```

---

## Windows (x64 / ARM64)

### 1. Install Visual Studio C++ Build Tools

Install [**Visual Studio 2026**](https://visualstudio.microsoft.com/)
(Community edition is free) with the following workloads selected in the
Visual Studio Installer:

- **Desktop development with C++** — provides MSVC compiler (`cl`), Windows
  SDK, CMake, and the linker.
- **C++ CMake tools for Windows** — bundled CMake integration.

If you only need command-line builds (no IDE), install
[Build Tools for Visual Studio](https://visualstudio.microsoft.com/visual-cpp-build-tools/)
instead — select the same workloads above.

> **Note:** Visual Studio does **not** add `cmake` or `cl` to the system
> PATH by default. They are only available inside the **Developer PowerShell
> for VS** (or **Native Tools Command Prompt**). If you run `cmake` or `cl`
> in a regular PowerShell window, you will get:
>
> ```
> cmake : The term 'cmake' is not recognized as the name of a cmdlet, function, script file, or operable program. Check
> the spelling of the name, or if a path was included, verify that the path is correct and try again.
> At line:1 char:1
> + cmake --version
> + ~~~~~
>     + CategoryInfo          : ObjectNotFound: (cmake:String) [], CommandNotFoundException
>     + FullyQualifiedErrorId : CommandNotFoundException
> ```
>
> To make them available globally, add their directories to your User PATH
> (adjust the VS year/edition and MSVC version to match your installation):

```powershell
# cmake
$cmakeDir = "C:\Program Files\Microsoft Visual Studio\<year>\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"

# cl (x64 — for ARM64, replace Hostx64\x64 with Hostarm64\arm64)
$clDir = "C:\Program Files\Microsoft Visual Studio\<year>\Community\VC\Tools\MSVC\<version>\bin\Hostx64\x64"

$currentPath = [Environment]::GetEnvironmentVariable("Path", "User")
foreach ($dir in @($cmakeDir, $clDir)) {
    if ($currentPath -notlike "*$dir*") {
        $currentPath = "$currentPath;$dir"
    }
}
[Environment]::SetEnvironmentVariable("Path", $currentPath, "User")
```

To find the exact MSVC version installed on your system:

```powershell
ls "C:\Program Files\Microsoft Visual Studio\<year>\Community\VC\Tools\MSVC"
# Example output: 14.50.35717
```

Reopen your terminal, then verify:

```powershell
cmake --version   # Should be 3.20+
cl                # Should print MSVC version information
```

### 2. Install Standalone vcpkg

> **Why not the Visual Studio bundled vcpkg?**
> Visual Studio 2022 17.6+ ships with a bundled vcpkg, but it only supports
> **manifest mode** (requires a `vcpkg.json` in the project). Running
> `vcpkg install <package>` with the bundled version will fail with:
>
> ```
> error: Could not locate a manifest (vcpkg.json) above the current working directory.
> This vcpkg distribution does not have a classic mode instance.
> ```
>
> To use **classic mode** (`vcpkg install glfw3`), you need a standalone
> vcpkg installation.

Clone and bootstrap the standalone vcpkg:

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
```

Then add `C:\vcpkg` to your User PATH so it is available globally:

```powershell
$vcpkgDir = "C:\vcpkg"

$currentPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ($currentPath -notlike "*$vcpkgDir*") {
    [Environment]::SetEnvironmentVariable("Path", "$currentPath;$vcpkgDir", "User")
    Write-Host "vcpkg added to User PATH. Reopen your terminal for the change to take effect."
} else {
    Write-Host "vcpkg is already in User PATH."
}
```

Reopen your terminal, then verify:

```powershell
vcpkg --version
# Expected: vcpkg package management program version ...
```

### 3. Install Vulkan SDK (optional — required for Vulkan API)

Download and install the [LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home)
for Windows. The installer will set the `VULKAN_SDK` environment variable and
add the SDK `Bin` directory (containing `glslc`, `vulkaninfo`, etc.) to PATH.

After installation, verify that your GPU supports Vulkan:

```powershell
vulkaninfo --summary
```

Expected output (example):

```
==========
VULKANINFO
==========
Vulkan Instance Version: 1.x.xxx

Devices:
========
GPU0:
    apiVersion         = 1.3.xxx
    driverVersion      = xxx.xx
    vendorID           = 0x10de
    deviceID           = 0x2684
    deviceType         = PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
    deviceName         = NVIDIA GeForce RTX 5090
    driverName         = NVIDIA
    driverInfo         = xxx.xx
```

If `vulkaninfo` reports no physical devices, your GPU driver may not support
Vulkan — the application will still work with DirectX or OpenGL backends.

### 4. Install Python 3 (required for OpenGL backend)

The OpenGL backend uses [GLAD](https://github.com/Dav1dde/glad) as its
loader generator. GLAD's CMake build invokes Python at configure time to
generate the OpenGL function loader source code. Without Python, CMake will
fail with:

```
Could NOT find Python (missing: Python_EXECUTABLE Interpreter)
```

Download and install Python from [python.org](https://www.python.org/downloads/).
During installation, make sure to check **"Add python.exe to PATH"**.

GLAD also depends on the **jinja2** Python package. Install it after Python
is set up:

```powershell
pip install jinja2
```

Without `jinja2`, the build will fail with:

```
ModuleNotFoundError: No module named 'jinja2'
```

After installation, reopen your terminal and verify:

```powershell
python --version
# Expected: Python 3.x.x
```

> **Note:** If you do not need the OpenGL backend, you can skip this step
> and disable it with `-DENABLE_OPENGL=OFF` during CMake configuration.

### 5. Install GLFW via vcpkg

```powershell
# x64
vcpkg install glfw3

# ARM64
vcpkg install glfw3:arm64-windows
```

The DX12 and DX11 backends only need the Windows SDK (bundled with Visual
Studio). No additional driver installation is needed — D3D12/D3D11 work
through the built-in Windows graphics stack.

---

## macOS (Apple Silicon / Intel)

```bash
brew install glfw cmake
```

The Metal backend uses the system Metal framework — no additional SDK or
driver installation is needed.

---

## Verify Environment

### Linux

```bash
cmake --version    # Should be 3.20+
g++ --version      # GCC 8+ or clang++ 7+
pkg-config --modversion glfw3   # Should print 3.x
glslc --version    # Optional — only required for the Vulkan backend
```

If `glslc` is not found and you need the Vulkan backend, install the LunarG
Vulkan SDK or `sudo apt install glslc`.

### Windows

Before building on Windows, ensure that `cmake`, `cl` (MSVC compiler), and
`glslc` (Vulkan shader compiler — optional) are available in your PATH.
If they are not found, see [Step 1](#1-install-visual-studio-c-build-tools)
for how to add them. Verify in PowerShell:

```powershell
cmake --version   # Should be 3.20+
cl                # Should print MSVC version information
glslc --version   # Optional — only required for the Vulkan backend
```

Typical default paths (Visual Studio 2026 Community on ARM64 as an example):

| Tool | Default Path |
|------|-------------|
| cmake | `C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin` |
| cl | `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\<version>\bin\Hostarm64\arm64` |
| glslc | `C:\VulkanSDK\<version>\Bin` |

Add the relevant directories to **User environment variables → Path**, then
reopen your terminal for the changes to take effect.

### macOS

```bash
cmake --version   # Should be 3.20+
clang --version   # Apple Clang (comes with Xcode Command Line Tools)
```

If `cmake` is not found, install it via Homebrew: `brew install cmake`.

---

## Build Steps

### Linux

```bash
# Configure (backends auto-detected based on installed packages)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build

# Run
./build/gpu_benchmark
```

CMake will print which backends are enabled during configuration:

```
-- Vulkan backend: ENABLED
-- DX12 backend:   DISABLED (not Windows)
-- DX11 backend:   DISABLED (not Windows)
-- Metal backend:  DISABLED (not macOS)
-- OpenGL backend: ENABLED
```

### Windows

```powershell
# Configure (vcpkg toolchain, all backends auto-detected)
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake

# For ARM64 native builds:
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=arm64-windows -A ARM64

# Build
cmake --build build --config Release
```

### macOS

```bash
# Configure (Metal backend auto-detected on macOS)
cmake -S . -B build

# Build
cmake --build build --config Release
```

### Backend Toggles

```bash
cmake -S . -B build -DENABLE_VULKAN=OFF -DENABLE_DX12=ON -DENABLE_DX11=ON -DENABLE_METAL=OFF ...
```
