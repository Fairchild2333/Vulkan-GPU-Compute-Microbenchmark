# GPU Compute Benchmark — HarmonyOS（鸿蒙）Vulkan 版本

[English](README.md)

本目录包含鸿蒙应用，与 Windows/Linux 版本使用相同的 Vulkan 粒子仿真逻辑，通过鸿蒙原生 API 运行。

## 架构

```text
ArkTS 入口 (Index.ets)
  └── XComponent (type: SURFACE)
        └── NAPI 模块 (libgpubench.so)
              ├── napi_init.cpp       — XComponent 生命周期回调
              └── vulkan_renderer.cpp — Vulkan 计算 + 图形管线
                    └── vkCreateSurfaceOHOS()（来自 OHNativeWindow）
```

ArkTS 层仅做最小封装，提供一个全屏 XComponent 并将 `OHNativeWindow` 交给 C++ 层，所有 GPU 逻辑在原生代码中完成。

## 环境依赖

| 依赖 | 说明 |
|------|------|
| **CodeArts IDE** | 鸿蒙 PC 开发用（基于 VSCode），或 DevEco Studio 5.0+ |
| **Node.js & npm** | 鸿蒙 PC 内置（`/data/app/bin/`），DevEco 使用自带工具 |
| **HarmonyOS SDK（API 12+）** | 含 Vulkan 头文件与 `libvulkan.so` |
| **SPIR-V 着色器** | 由项目 GLSL 源码预编译得到 |

## 准备着色器

将 GLSL 编译为 SPIR-V 并放到 rawfile 目录。已安装 Vulkan SDK 的机器上执行：

```bash
glslc shaders/compute.comp  -o ohos/entry/src/main/resources/rawfile/shaders/compute.comp.spv
glslc shaders/particle.vert -o ohos/entry/src/main/resources/rawfile/shaders/particle.vert.spv
glslc shaders/particle.frag -o ohos/entry/src/main/resources/rawfile/shaders/particle.frag.spv
```

若本机没有 `glslc`，可从 Windows 的 `build/Release/` 目录复制已编译的 `.spv` 文件到
`ohos/entry/src/main/resources/rawfile/shaders/`。

## 使用 DevEco Studio 构建

1. 打开 DevEco Studio
2. **File → Open Project**，选择 `ohos/` 目录
3. 连接鸿蒙 PC 或设备
4. 点击 **Build → Build Hap(s)/APP(s)** 或按 **Ctrl+F9**
5. 使用 **Run → Run 'entry'** 在设备上运行

## 命令行构建（鸿蒙 PC 上使用 CodeArts IDE）

在鸿蒙 PC 上使用 hvigor 工具链构建时：

**1. 配置系统环境**

在 `~/.zshrc` 中加入系统 bin 路径和 npm 全局 bin：

```bash
echo 'export PATH=/data/app/bin:$PATH' >> ~/.zshrc
echo 'export PATH="$(npm prefix -g)/bin:$PATH"' >> ~/.zshrc
source ~/.zshrc
```

**2. 安装 hvigor 构建工具**

```bash
npm config set @ohos:registry=https://repo.harmonyos.com/npm/
npm install -g @ohos/hvigor @ohos/hvigor-ohos-plugin
```

**3. 编译项目**

```bash
cd ohos
hvigor assembleHap --mode module -p module=entry
```

> 若 hvigor 报错项目结构或版本不匹配，检查全局安装的 `@ohos/hvigor` 版本是否与项目中的
> `oh-package.json5` 或 `hvigor/hvigor-config.json5` 一致。

## 安装与运行

生成的 `.hap` 文件路径为：

```
entry/build/default/outputs/default/entry-default-signed.hap
```

通过 hdc 安装到设备：

```bash
hdc install entry/build/default/outputs/default/entry-default-signed.hap
```

也可在 DevEco Studio 中直接运行。

## GPU 性能输出

耗时信息通过 HiLog 输出，可用以下命令查看：

```bash
hdc shell hilog | grep GpuBench
```

示例输出：

```
Compute: 0.031 ms | Render: 0.054 ms | Total: 0.112 ms | FPS: 3200
```

## 项目结构

```text
ohos/
├── AppScope/
│   ├── app.json5                          # 应用元数据
│   └── resources/base/element/string.json
├── entry/
│   ├── build-profile.json5                # 原生构建配置（CMake 路径、ABI）
│   ├── hvigorfile.ts
│   └── src/main/
│       ├── ets/
│       │   ├── entryability/EntryAbility.ets  # UIAbility 生命周期
│       │   └── pages/Index.ets               # 全屏 XComponent
│       ├── cpp/
│       │   ├── CMakeLists.txt                 # 链接 libvulkan.so 等
│       │   ├── napi_init.cpp                  # NAPI + XComponent 回调
│       │   ├── vulkan_renderer.h              # 渲染器接口
│       │   └── vulkan_renderer.cpp            # 完整 Vulkan 管线
│       ├── module.json5                       # 模块配置
│       └── resources/
│           ├── base/
│           │   ├── element/string.json
│           │   └── profile/main_pages.json
│           └── rawfile/shaders/               # SPIR-V 文件放置目录
├── build-profile.json5                    # 项目级构建配置
└── hvigorfile.ts
```
