# GPU Compute Benchmark — HarmonyOS (OHOS) Vulkan Build

# GPU Compute Benchmark — HarmonyOS (OHOS) Vulkan Build

# GPU Compute Benchmark — HarmonyOS (OHOS) Vulkan Build

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
The ArkTS layer is minimal — it provides a full-screen XComponent that handsan OHNativeWindow to the C++ layer. All GPU work happens in native code.PrerequisitesDependencyNotesCodeArts IDEFor HarmonyOS PC (VSCode-based).Node.js & npmBuilt-in on HarmonyOS PC (located in /data/app/bin/).HarmonyOS SDKIncludes Vulkan headers and libvulkan.so.SPIR-V shadersPre-compiled from the project's GLSL sources.Prepare ShadersBefore building, compile the GLSL shaders to SPIR-V and place them in therawfile directory. On a machine with Vulkan SDK installed:Bashglslc shaders/compute.comp  -o ohos/entry/src/main/resources/rawfile/shaders/compute.comp.spv
glslc shaders/particle.vert -o ohos/entry/src/main/resources/rawfile/shaders/particle.vert.spv
glslc shaders/particle.frag -o ohos/entry/src/main/resources/rawfile/shaders/particle.frag.spv
Note for HarmonyOS PC Users: If you don't have glslc installed locally, you must copy the pre-built .spv files from the Windows build/Release/ directory into ohos/entry/src/main/resources/rawfile/shaders/ before compiling.Build on HarmonyOS PC (CodeArts IDE & CLI)Unlike DevEco Studio, CodeArts IDE on HarmonyOS PC is a lightweight editor. The project must be built via the native system terminal using the hvigor toolchain.1. Configure System EnvironmentEnsure the native terminal can locate the built-in node and npm tools. Add the system app bin path to your ~/.zshrc:Bashecho 'export PATH=/data/app/bin:$PATH' >> ~/.zshrc
source ~/.zshrc
2. Install Hvigor Build ToolsConfigure the official HarmonyOS npm registry and globally install the required build tools:Bashnpm config set @ohos:registry=[https://repo.harmonyos.com/npm/](https://repo.harmonyos.com/npm/)
npm install -g @ohos/hvigor @ohos/hvigor-ohos-plugin
Make sure the global npm bin path is also added to your environment variables so the hvigor command can be recognized:Bashecho 'export PATH="$(npm prefix -g)/bin:$PATH"' >> ~/.zshrc
source ~/.zshrc
3. Compile the ProjectNavigate to the ohos/ directory and execute the build command:Bashcd ohos
hvigor assembleHap --mode module -p module=entry
(Troubleshooting: If hvigor complains about project structure or configuration upgrades, ensure your globally installed @ohos/hvigor version matches the version specified in the project's oh-package.json5 or hvigor/hvigor-config.json5)Install and RunThe output .hap file will be generated in:entry/build/default/outputs/default/entry-default-signed.hapYou can push it to the device via hdc:Bashhdc install entry/build/default/outputs/default/entry-default-signed.hap
GPU Profiling OutputTiming information is logged via HiLog. View it with:Bashhdc shell hilog | grep GpuBench
Sample output:PlaintextCompute: 0.031 ms | Render: 0.054 ms | Total: 0.112 ms | FPS: 3200
Project Structure(See original structure mapping)
***

这份文档现在已经是一份极其标准的“原生鸿蒙 PC 极客开发指南”了。

我们刚才已经帮你把 `node` 和 `npm` 的环境跑通了。要在那个独立的终端里一鼓作气把最后这几行编译命令

This directory contains the HarmonyOS application that runs the same Vulkan
particle simulation as the Windows/Linux build, using HarmonyOS native APIs.

## Architecture

```
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
|---|---|
| **DevEco Studio 5.0+** | Or command-line build tools |
| **HarmonyOS SDK (API 12+)** | Includes Vulkan headers and `libvulkan.so` |
| **SPIR-V shaders** | Pre-compiled from the project's GLSL sources |

## Prepare Shaders

Before building, compile the GLSL shaders to SPIR-V and place them in the
rawfile directory. On a machine with Vulkan SDK installed:

```bash
glslc shaders/compute.comp  -o ohos/entry/src/main/resources/rawfile/shaders/compute.comp.spv
glslc shaders/particle.vert -o ohos/entry/src/main/resources/rawfile/shaders/particle.vert.spv
glslc shaders/particle.frag -o ohos/entry/src/main/resources/rawfile/shaders/particle.frag.spv
```

Or copy the pre-built `.spv` files from the Windows `build/Release/` directory.

## Build with DevEco Studio

1. Open DevEco Studio
2. **File → Open Project** → select the `ohos/` directory
3. Connect your HarmonyOS PC or device
4. Click **Build → Build Hap(s)/APP(s)** or press **Ctrl+F9**
5. Run on device with **Run → Run 'entry'**

## Build from Command Line (on HarmonyOS PC)

If building natively on the HarmonyOS PC using Termony + DevBox:

```bash
# Ensure hvigorw is available (from DevEco or SDK)
cd ohos
hvigorw assembleHap --mode module -p module=entry

# The output .hap file will be in:
# entry/build/default/outputs/default/entry-default-signed.hap
```

## Install and Run

```bash
# Push to device via hdc
hdc install entry/build/default/outputs/default/entry-default-signed.hap

# Or run directly from DevEco Studio
```

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

```
ohos/
├── AppScope/
│   ├── app.json5                          # Application metadata
│   └── resources/base/element/string.json
├── entry/
│   ├── build-profile.json5               # Native build config (CMake path, ABI)
│   ├── hvigorfile.ts
│   └── src/main/
│       ├── ets/
│       │   ├── entryability/EntryAbility.ets  # UIAbility lifecycle
│       │   └── pages/Index.ets                # Full-screen XComponent
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


This directory contains the HarmonyOS application that runs the same Vulkan
particle simulation as the Windows/Linux build, using HarmonyOS native APIs.

## Architecture

```
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
|---|---|
| **DevEco Studio 5.0+** | Or command-line build tools |
| **HarmonyOS SDK (API 12+)** | Includes Vulkan headers and `libvulkan.so` |
| **SPIR-V shaders** | Pre-compiled from the project's GLSL sources |

## Prepare Shaders

Before building, compile the GLSL shaders to SPIR-V and place them in the
rawfile directory. On a machine with Vulkan SDK installed:

```bash
glslc shaders/compute.comp  -o ohos/entry/src/main/resources/rawfile/shaders/compute.comp.spv
glslc shaders/particle.vert -o ohos/entry/src/main/resources/rawfile/shaders/particle.vert.spv
glslc shaders/particle.frag -o ohos/entry/src/main/resources/rawfile/shaders/particle.frag.spv
```

Or copy the pre-built `.spv` files from the Windows `build/Release/` directory.

## Build with DevEco Studio

1. Open DevEco Studio
2. **File → Open Project** → select the `ohos/` directory
3. Connect your HarmonyOS PC or device
4. Click **Build → Build Hap(s)/APP(s)** or press **Ctrl+F9**
5. Run on device with **Run → Run 'entry'**

## Build from Command Line (on HarmonyOS PC)

If building natively on the HarmonyOS PC using Termony + DevBox:

```bash
# Ensure hvigorw is available (from DevEco or SDK)
cd ohos
hvigorw assembleHap --mode module -p module=entry

# The output .hap file will be in:
# entry/build/default/outputs/default/entry-default-signed.hap
```

## Install and Run

```bash
# Push to device via hdc
hdc install entry/build/default/outputs/default/entry-default-signed.hap

# Or run directly from DevEco Studio
```

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

```
ohos/
├── AppScope/
│   ├── app.json5                          # Application metadata
│   └── resources/base/element/string.json
├── entry/
│   ├── build-profile.json5               # Native build config (CMake path, ABI)
│   ├── hvigorfile.ts
│   └── src/main/
│       ├── ets/
│       │   ├── entryability/EntryAbility.ets  # UIAbility lifecycle
│       │   └── pages/Index.ets                # Full-screen XComponent
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
