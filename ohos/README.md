# GPU Compute Benchmark — HarmonyOS (OHOS) Vulkan Build

[中文](README.zh-CN.md)

This directory contains the HarmonyOS application that runs the same Vulkan
particle simulation as the Windows/Linux build, using HarmonyOS native APIs.

## Architecture

```text
ArkTS Entry (Index.ets)
  └── XComponent (type: SURFACE)
        └── NAPI Module (libgpubench.so)
              ├── napi_init.cpp       — XComponent lifecycle callbacks
              └── vulkan_renderer.cpp — Vulkan compute + graphics pipeline
                    └── vkCreateSurfaceOHOS() from OHNativeWindow
```

The ArkTS layer is minimal — it provides a full-screen XComponent that hands
an `OHNativeWindow` to the C++ layer. All GPU work happens in native code.

## Prerequisites

| Dependency | Notes |
|------------|-------|
| **CodeArts IDE** | For HarmonyOS PC (VSCode-based), or DevEco Studio 5.0+ |
| **Node.js & npm** | Built-in on HarmonyOS PC (`/data/app/bin/`). For DevEco, use built-in tools |
| **HarmonyOS SDK (API 12+)** | Includes Vulkan headers and `libvulkan.so` |
| **SPIR-V shaders** | Pre-compiled from the project's GLSL sources |

## Prepare Shaders

Compile the GLSL shaders to SPIR-V and place them in the rawfile directory.
On a machine with Vulkan SDK installed:

```bash
glslc shaders/compute.comp  -o ohos/entry/src/main/resources/rawfile/shaders/compute.comp.spv
glslc shaders/particle.vert -o ohos/entry/src/main/resources/rawfile/shaders/particle.vert.spv
glslc shaders/particle.frag -o ohos/entry/src/main/resources/rawfile/shaders/particle.frag.spv
```

If `glslc` is not available locally, copy the pre-built `.spv` files from the
Windows `build/Release/` directory into `ohos/entry/src/main/resources/rawfile/shaders/`.

## Build with DevEco Studio

1. Open DevEco Studio
2. **File → Open Project** → select the `ohos/` directory
3. Connect your HarmonyOS PC or device
4. Click **Build → Build Hap(s)/APP(s)** or press **Ctrl+F9**
5. Run on device with **Run → Run 'entry'**

## Build from Command Line (CodeArts IDE on HarmonyOS PC)

If building natively on the HarmonyOS PC using the hvigor toolchain:

**1. Configure system environment**

Add the system app bin path and npm global bin to your `~/.zshrc`:

```bash
echo 'export PATH=/data/app/bin:$PATH' >> ~/.zshrc
echo 'export PATH="$(npm prefix -g)/bin:$PATH"' >> ~/.zshrc
source ~/.zshrc
```

**2. Install hvigor build tools**

```bash
npm config set @ohos:registry=https://repo.harmonyos.com/npm/
npm install -g @ohos/hvigor @ohos/hvigor-ohos-plugin
```

**3. Compile the project**

```bash
cd ohos
hvigor assembleHap --mode module -p module=entry
```

> If hvigor complains about project structure or version mismatches, ensure your
> global `@ohos/hvigor` version matches the project's `oh-package.json5` or
> `hvigor/hvigor-config.json5`.

## Install and Run

The output `.hap` file will be in:

```
entry/build/default/outputs/default/entry-default-signed.hap
```

Push to device via hdc:

```bash
hdc install entry/build/default/outputs/default/entry-default-signed.hap
```

Or run directly from DevEco Studio.

## GPU Profiling Output

Timing information is logged via HiLog. View it with:

```bash
hdc shell hilog | grep GpuBench
```

Sample output:

```
Compute: 0.031 ms | Render: 0.054 ms | Total: 0.112 ms | FPS: 3200
```

## Project Structure

```text
ohos/
├── AppScope/
│   ├── app.json5                          # Application metadata
│   └── resources/base/element/string.json
├── entry/
│   ├── build-profile.json5                # Native build config (CMake path, ABI)
│   ├── hvigorfile.ts
│   └── src/main/
│       ├── ets/
│       │   ├── entryability/EntryAbility.ets  # UIAbility lifecycle
│       │   └── pages/Index.ets               # Full-screen XComponent
│       ├── cpp/
│       │   ├── CMakeLists.txt                 # Links libvulkan.so etc.
│       │   ├── napi_init.cpp                  # NAPI + XComponent callbacks
│       │   ├── vulkan_renderer.h              # Renderer interface
│       │   └── vulkan_renderer.cpp            # Full Vulkan pipeline
│       ├── module.json5                       # Module config
│       └── resources/
│           ├── base/
│           │   ├── element/string.json
│           │   └── profile/main_pages.json
│           └── rawfile/shaders/               # SPIR-V files go here
├── build-profile.json5                    # Project-level build config
└── hvigorfile.ts
```
