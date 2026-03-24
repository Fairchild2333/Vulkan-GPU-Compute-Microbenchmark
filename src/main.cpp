#include "app_base.h"
#include "benchmark_results.h"

#ifdef HAVE_VULKAN
#include "vulkan_backend.h"
#endif
#ifdef HAVE_DX12
#include "dx12_backend.h"
#endif
#ifdef HAVE_DX11
#include "dx11_backend.h"
#endif
#ifdef HAVE_METAL
#include "metal_backend.h"
#include "metal_probe.h"
#endif
#ifdef HAVE_OPENGL
#include "opengl_backend.h"
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>

#ifdef _WIN32
#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#pragma comment(lib, "dxgi.lib")

// Hint NVIDIA Optimus and AMD Switchable Graphics to prefer the discrete GPU
// for OpenGL contexts.  These must be exported from the final executable.
extern "C" {
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int   AmdPowerXpressRequestHighPerformance = 1;
}
#endif

#ifdef HAVE_VULKAN
#include <vulkan/vulkan.h>
#endif

static bool SafeStoi(const std::string& s, int& out) {
    try { out = std::stoi(s); return true; }
    catch (...) { return false; }
}

static std::string ExeDirectory(const char* argv0) {
    std::string path(argv0);
    auto pos = path.find_last_of("\\/");
    return pos != std::string::npos ? path.substr(0, pos + 1) : "";
}

#ifdef _WIN32
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    std::cout << "\n[CRASH] Unhandled exception: 0x"
              << std::hex << code << std::dec << std::endl;

    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        std::cout << "  -> Access violation (segfault)" << std::endl; break;
    case EXCEPTION_STACK_OVERFLOW:
        std::cout << "  -> Stack overflow" << std::endl; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        std::cout << "  -> Integer divide by zero" << std::endl; break;
    default:
        std::cout << "  -> SEH exception code: 0x" << std::hex << code << std::endl; break;
    }

    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

// ---------------------------------------------------------------------------
// Lightweight API probe — enumerates GPUs and checks which backends each one
// supports, *without* creating a full device or window.
// ---------------------------------------------------------------------------

struct GpuInfo {
    std::string name;
    bool isSoftware  = false;
    bool isDiscrete  = false;
    std::uint64_t vramMB = 0;
    bool supportsVulkan = false;
    bool supportsDX12   = false;
    bool supportsDX11   = true;   // virtually everything supports DX11
    bool supportsMetal  = false;
    bool supportsOpenGL = false;
    std::int32_t dxgiRawIndex = -1;  // DXGI EnumAdapters1 index (stable across reordering)
    std::int32_t vkPhysDevIndex = -1; // Vulkan vkEnumeratePhysicalDevices index
    std::int64_t luidHigh = 0;  // DXGI adapter LUID for cross-factory matching
    std::int64_t luidLow  = 0;
};

static bool NameLooksIntegrated(const std::string& name) {
    // Intel integrated: HD Graphics, UHD Graphics, Iris
    if (name.find("Intel") != std::string::npos) {
        if (name.find("Arc") != std::string::npos)
            return false;  // Intel Arc is discrete
        return true;       // all other Intel GPUs are integrated
    }
    // NVIDIA desktop GPUs are always discrete
    if (name.find("NVIDIA") != std::string::npos ||
        name.find("GeForce") != std::string::npos ||
        name.find("Quadro") != std::string::npos ||
        name.find("Tesla") != std::string::npos)
        return false;
    // AMD: discrete GPUs carry a model family (HD, RX, R9, R7, R5, Pro, VII, W)
    if (name.find("Radeon") != std::string::npos) {
        if (name.find("HD ")  != std::string::npos ||
            name.find("RX ")  != std::string::npos ||
            name.find("R9 ")  != std::string::npos ||
            name.find("R7 ")  != std::string::npos ||
            name.find("R5 ")  != std::string::npos ||
            name.find("VII")  != std::string::npos ||
            name.find("Pro ") != std::string::npos ||
            name.find(" W")   != std::string::npos)
            return false;  // discrete
        return true;       // generic "Radeon Graphics" = APU integrated
    }
    return false;  // unknown vendor, assume discrete
}

static std::vector<GpuInfo> ProbeGpus() {
    std::vector<GpuInfo> gpus;

#ifdef _WIN32
    // --- DXGI enumeration (allow multiple identical GPUs, deduplicate by LUID) ---
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        struct DevLuid { LONG high; DWORD low; };
        std::vector<DevLuid> seen;

        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);

            bool dup = false;
            for (const auto& s : seen) {
                if (s.high == desc.AdapterLuid.HighPart &&
                    s.low  == desc.AdapterLuid.LowPart) {
                    dup = true; break;
                }
            }
            if (dup) { adapter.Reset(); continue; }
            seen.push_back({desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart});

            GpuInfo info;
            char name[256]{};
            size_t converted = 0;
            wcstombs_s(&converted, name, sizeof(name), desc.Description, _TRUNCATE);
            info.name = name;
            info.isSoftware = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            info.vramMB = desc.DedicatedVideoMemory / (1024 * 1024);
            info.dxgiRawIndex = static_cast<std::int32_t>(i);
            info.luidHigh = desc.AdapterLuid.HighPart;
            info.luidLow  = desc.AdapterLuid.LowPart;

#ifdef HAVE_DX12
            {
                extern bool ProbeDX12Support(IDXGIAdapter1*);
                info.supportsDX12 = ProbeDX12Support(adapter.Get());
            }
#endif
            gpus.push_back(info);
            adapter.Reset();
        }

        // --- DXGI 1.6: classify discrete vs integrated ---
        // EnumAdapterByGpuPreference(HIGH_PERFORMANCE) returns the adapter
        // Windows considers "high performance" first — typically the discrete GPU.
        // Only use this heuristic when there are 2+ non-software GPUs; with a
        // single GPU the preference order is meaningless and would incorrectly
        // label an integrated GPU as discrete.
        std::size_t hwGpuCount = 0;
        for (const auto& g : gpus) { if (!g.isSoftware) ++hwGpuCount; }

        if (hwGpuCount >= 2) {
            Microsoft::WRL::ComPtr<IDXGIFactory6> factory6;
            if (SUCCEEDED(factory.As(&factory6))) {
                Microsoft::WRL::ComPtr<IDXGIAdapter1> hpAdapter;
                for (UINT hp = 0;
                     SUCCEEDED(factory6->EnumAdapterByGpuPreference(
                         hp, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                         IID_PPV_ARGS(&hpAdapter)));
                     ++hp)
                {
                    DXGI_ADAPTER_DESC1 hpDesc{};
                    hpAdapter->GetDesc1(&hpDesc);
                    if (hpDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                        hpAdapter.Reset();
                        continue;
                    }
                    char hpName[256]{};
                    size_t hpConverted = 0;
                    wcstombs_s(&hpConverted, hpName, sizeof(hpName), hpDesc.Description, _TRUNCATE);
                    for (auto& g : gpus) {
                        if (g.name == hpName) { g.isDiscrete = true; break; }
                    }
                    hpAdapter.Reset();
                    break;  // only the first non-software high-perf adapter is discrete
                }
            }
        } else {
            // Single hardware GPU: DXGI preference API can't distinguish, and
            // old GPUs may lack Vulkan.  Fall back to a name-based heuristic.
            for (auto& g : gpus) {
                if (!g.isSoftware)
                    g.isDiscrete = !NameLooksIntegrated(g.name);
            }
        }
    }

#endif  // _WIN32

#ifdef HAVE_METAL
    {
        auto metalDevices = gpu_bench::ProbeMetalDevices();
        for (auto& md : metalDevices) {
            auto it = std::find_if(gpus.begin(), gpus.end(), [&](const GpuInfo& g) {
                return g.name.find(md.name) != std::string::npos
                    || md.name.find(g.name) != std::string::npos;
            });
            if (it != gpus.end()) {
                it->supportsMetal = true;
                if (it->vramMB == 0 && md.vramBytes > 0)
                    it->vramMB = static_cast<std::uint32_t>(md.vramBytes / (1024 * 1024));
            } else {
                GpuInfo info;
                info.name          = md.name;
                info.supportsMetal = true;
                info.vramMB        = static_cast<std::uint32_t>(md.vramBytes / (1024 * 1024));
                info.isDiscrete    = !md.isLowPower;
                info.isSoftware    = false;
                gpus.push_back(info);
            }
        }
    }
#endif

    // --- Vulkan probe (if compiled in) ---
#ifdef HAVE_VULKAN
    {
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.apiVersion = VK_API_VERSION_1_1;

        VkInstanceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &appInfo;

        VkInstance inst = VK_NULL_HANDLE;
        if (vkCreateInstance(&ci, nullptr, &inst) == VK_SUCCESS) {
            std::uint32_t count = 0;
            vkEnumeratePhysicalDevices(inst, &count, nullptr);
            std::vector<VkPhysicalDevice> devs(count);
            vkEnumeratePhysicalDevices(inst, &count, devs.data());

            for (std::uint32_t di = 0; di < devs.size(); ++di) {
                const auto& dev = devs[di];
                VkPhysicalDeviceProperties props{};
                vkGetPhysicalDeviceProperties(dev, &props);

                bool vkIsSoftware = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU);
                bool vkIsDiscrete = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);

                // Match Vulkan device to an existing DXGI entry by name.
                // Prefer an unmatched entry first so that duplicate GPUs each get their own match.
                bool matched = false;
                auto nameMatches = [&](const GpuInfo& gpu) {
                    return gpu.name.find(props.deviceName) != std::string::npos ||
                           std::string(props.deviceName).find(gpu.name) != std::string::npos;
                };
                // First pass: match an entry that hasn't been marked yet.
                for (auto& gpu : gpus) {
                    if (!gpu.supportsVulkan && nameMatches(gpu)) {
                        gpu.supportsVulkan = true;
                        gpu.vkPhysDevIndex = static_cast<std::int32_t>(di);
                        if (vkIsSoftware) gpu.isSoftware = true;
                        if (vkIsDiscrete) gpu.isDiscrete = true;
                        matched = true;
                        break;
                    }
                }
                // Second pass: all matching entries already marked — this is an
                // additional GPU with the same name (e.g. dual identical cards).
                // Create a new entry right after the first match so ordering is logical.
                if (!matched) {
                    for (std::size_t gi = 0; gi < gpus.size(); ++gi) {
                        if (nameMatches(gpus[gi])) {
                            GpuInfo info;
                            info.name = gpus[gi].name;  // copy DXGI name so disambiguation matches
                            info.supportsVulkan = true;
                            info.isSoftware = vkIsSoftware;
                            info.isDiscrete = vkIsDiscrete;
                            info.vramMB = gpus[gi].vramMB;
                            info.supportsDX12 = gpus[gi].supportsDX12;
                            info.supportsDX11 = gpus[gi].supportsDX11;
                            info.supportsOpenGL = gpus[gi].supportsOpenGL;
                            info.dxgiRawIndex = gpus[gi].dxgiRawIndex;
                            info.luidHigh = gpus[gi].luidHigh;
                            info.luidLow  = gpus[gi].luidLow;
                            info.vkPhysDevIndex = static_cast<std::int32_t>(di);
                            gpus.insert(gpus.begin() + static_cast<std::ptrdiff_t>(gi) + 1, info);
                            matched = true;
                            break;
                        }
                    }
                }
                // Partial match: try matching by a substring of the name.
                if (!matched) {
                    std::string vkName(props.deviceName);
                    for (auto& gpu : gpus) {
                        if (!gpu.isSoftware && !gpu.supportsVulkan) {
                            if (vkName.size() > 6 && gpu.name.size() > 6) {
                                std::string shortVk = vkName.substr(0, vkName.size() / 2);
                                if (gpu.name.find(shortVk) != std::string::npos) {
                                    gpu.supportsVulkan = true;
                                    if (vkIsDiscrete) gpu.isDiscrete = true;
                                    matched = true;
                                    break;
                                }
                            }
                        }
                    }
                }
                if (!matched) {
                    GpuInfo info;
                    info.name = props.deviceName;
                    info.supportsVulkan = true;
                    info.isSoftware = vkIsSoftware;
                    info.isDiscrete = vkIsDiscrete;
                    info.supportsDX11 = false;
                    info.supportsDX12 = false;
                    gpus.push_back(info);
                }
            }
            vkDestroyInstance(inst, nullptr);
        }
    }
#endif

    // --- OpenGL 4.3 probe (if compiled in) ---
#ifdef HAVE_OPENGL
    {
        if (glfwInit() == GLFW_FALSE)
            return gpus;  // GLFW not available — skip OpenGL probe

        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_CLIENT_API,            GLFW_OPENGL_API);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,  4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,  3);
        glfwWindowHint(GLFW_OPENGL_PROFILE,         GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_VISIBLE,                GLFW_FALSE);
#ifdef __APPLE__
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT,  GLFW_TRUE);
#endif
        GLFWwindow* probe = glfwCreateWindow(1, 1, "", nullptr, nullptr);
        if (probe) {
            glfwMakeContextCurrent(probe);

            // Read GL_RENDERER for software detection and fallback GPU entry.
            using GetStringFn = const unsigned char* (*)(unsigned int);
            auto glGetStringFn = reinterpret_cast<GetStringFn>(
                glfwGetProcAddress("glGetString"));
            std::string glRenderer;
            if (glGetStringFn) {
                const char* r = reinterpret_cast<const char*>(
                    glGetStringFn(0x1F01));  // GL_RENDERER
                if (r) glRenderer = r;
            }

            bool isSoftwareGL =
                glRenderer.find("llvmpipe") != std::string::npos ||
                glRenderer.find("softpipe") != std::string::npos ||
                glRenderer.find("swrast")   != std::string::npos ||
                glRenderer.find("lavapipe") != std::string::npos ||
                glRenderer.find("Software") != std::string::npos;

            // Mark all existing hardware GPUs as OpenGL-capable.
            for (auto& gpu : gpus)
                if (!gpu.isSoftware)
                    gpu.supportsOpenGL = true;

            // Mark existing software GPUs if the GL renderer is software-based.
            if (isSoftwareGL) {
                for (auto& gpu : gpus)
                    if (gpu.isSoftware)
                        gpu.supportsOpenGL = true;
            }

            // On Linux without DXGI, gpus may be empty if no Vulkan SDK
            // is installed.  Create a GPU entry from GL_RENDERER so the
            // application has at least one usable device.
            if (!glRenderer.empty()) {
                bool alreadyListed = false;
                for (const auto& gpu : gpus) {
                    if (glRenderer.find(gpu.name) != std::string::npos ||
                        gpu.name.find(glRenderer) != std::string::npos) {
                        alreadyListed = true;
                        break;
                    }
                }
                if (!alreadyListed) {
                    GpuInfo info;
                    info.name = glRenderer;
                    info.supportsOpenGL = true;
                    info.isSoftware = isSoftwareGL;
                    info.supportsDX11 = false;
                    info.supportsDX12 = false;
                    gpus.push_back(info);
                }
            }

            glfwMakeContextCurrent(nullptr);
            glfwDestroyWindow(probe);
        }
        glfwTerminate();
    }
#endif

    // Disambiguate GPUs with identical names by appending #1, #2, etc.
    for (std::size_t i = 0; i < gpus.size(); ++i) {
        const std::string original = gpus[i].name;
        int count = 0;
        for (std::size_t j = 0; j < gpus.size(); ++j)
            if (gpus[j].name == original) ++count;
        if (count > 1) {
            int idx = 1;
            for (std::size_t j = 0; j < gpus.size(); ++j) {
                if (gpus[j].name == original) {
                    gpus[j].name += " #" + std::to_string(idx++);
                }
            }
        }
    }

    return gpus;
}

#ifdef HAVE_DX12
#include <d3d12.h>
bool ProbeDX12Support(IDXGIAdapter1* adapter) {
    return SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0,
                                       __uuidof(ID3D12Device), nullptr));
}
#endif

// ---------------------------------------------------------------------------

static void PrintGpuTable(const std::vector<GpuInfo>& gpus) {
    std::cout << "\n============================================================\n"
              << "                    GPU & API Detection\n"
              << "============================================================\n";
    std::cout << "  GPU";
    std::cout << std::string(41, ' ') << "VRAM     Vulkan  DX12  DX11  Metal  OpenGL\n";
    std::cout << "  ";
    std::cout << std::string(44, '-') << " ------  ------  ----  ----  -----  ------\n";

    for (std::uint32_t i = 0; i < gpus.size(); ++i) {
        const auto& g = gpus[i];
        std::string label = g.name;
        if (g.isSoftware)       label += " (Software)";
        else if (g.isDiscrete)  label += " (Discrete)";
        else                    label += " (Integrated)";
        if (label.size() > 44) label = label.substr(0, 41) + "...";

        std::cout << "  " << label;
        if (label.size() < 44) std::cout << std::string(44 - label.size(), ' ');

        if (g.vramMB > 0)
            std::cout << " " << g.vramMB << " MB";
        else
            std::cout << "     - ";

        auto yn = [](bool v) { return v ? "  YES " : "   -  "; };
        std::cout << yn(g.supportsVulkan)
                  << yn(g.supportsDX12)
                  << yn(g.supportsDX11)
                  << yn(g.supportsMetal)
                  << yn(g.supportsOpenGL)
                  << "\n";
    }
    std::cout << "============================================================\n\n";
}

// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(CrashHandler);
#endif

    std::string backend = "auto";
    std::int32_t gpuIndex = -1;
    bool useWarp = false;
    bool runAll = false;
    bool fullAnalysis = false;
    gpu_bench::BenchmarkConfig benchCfg;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            backend = argv[++i];
        } else if (std::strcmp(argv[i], "--gpu") == 0 && i + 1 < argc) {
            gpuIndex = std::stoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--warp") == 0) {
            useWarp = true;
        } else if (std::strcmp(argv[i], "--vsync") == 0) {
            benchCfg.vsync = true;
        } else if (std::strcmp(argv[i], "--benchmark") == 0) {
            benchCfg.benchmarkMode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                benchCfg.benchFrames = static_cast<std::uint32_t>(
                    std::stoi(argv[++i]));
            }
        } else if (std::strcmp(argv[i], "--host-memory") == 0) {
            benchCfg.hostMemory = true;
        } else if (std::strcmp(argv[i], "--flights") == 0 && i + 1 < argc) {
            int n = std::stoi(argv[++i]);
            if (n < 1) n = 1;
            if (n > 16) n = 16;
            benchCfg.framesInFlight = static_cast<std::uint32_t>(n);
        } else if (std::strcmp(argv[i], "--headless") == 0) {
            benchCfg.headless = true;
        } else if (std::strcmp(argv[i], "--time") == 0 && i + 1 < argc) {
            benchCfg.maxRunTimeSec = std::stod(argv[++i]);
        } else if (std::strcmp(argv[i], "--no-time-limit") == 0) {
            benchCfg.maxRunTimeSec = 0.0;
        } else if (std::strcmp(argv[i], "--particles") == 0 && i + 1 < argc) {
            auto n = static_cast<std::uint32_t>(std::stoi(argv[++i]));
            const std::uint32_t wg = gpu_bench::kComputeWorkGroupSize;
            benchCfg.particleCount = ((n + wg - 1) / wg) * wg;
            benchCfg.particlesOverridden = true;
        } else if (std::strcmp(argv[i], "--results") == 0) {
            auto results = gpu_bench::LoadResults();
            gpu_bench::PrintResultsTable(results);
            return 0;
        } else if (std::strcmp(argv[i], "--results-delete") == 0 && i + 1 < argc) {
            std::string id = argv[++i];
            if (gpu_bench::DeleteResult(id))
                std::cout << "Deleted result: " << id << "\n";
            else
                std::cout << "Result not found: " << id << "\n";
            return 0;
        } else if (std::strcmp(argv[i], "--results-clear") == 0) {
            gpu_bench::ClearResults();
            std::cout << "All benchmark results cleared.\n";
            return 0;
        } else if (std::strcmp(argv[i], "--compare") == 0) {
            auto results = gpu_bench::LoadResults();
            if (i + 2 < argc && argv[i + 1][0] != '-' && argv[i + 2][0] != '-') {
                std::string id1 = argv[++i];
                std::string id2 = argv[++i];
                const gpu_bench::BenchmarkResult* r1 = nullptr;
                const gpu_bench::BenchmarkResult* r2 = nullptr;
                for (const auto& r : results) {
                    if (r.id == id1) r1 = &r;
                    if (r.id == id2) r2 = &r;
                }
                if (!r1) { std::cerr << "Result not found: " << id1 << "\n"; return 1; }
                if (!r2) { std::cerr << "Result not found: " << id2 << "\n"; return 1; }
                gpu_bench::PrintDetailedComparison(*r1, *r2);
            } else {
                gpu_bench::PrintComparisonTable(results);
            }
            return 0;
        } else if (std::strcmp(argv[i], "--results-export") == 0 && i + 1 < argc) {
            std::string path = argv[++i];
            auto results = gpu_bench::LoadResults();
            if (results.empty()) {
                std::cout << "No results to export.\n";
            } else if (gpu_bench::ExportResultsCsv(path, results)) {
                std::cout << "Exported " << results.size()
                          << " result(s) to " << path << "\n";
            } else {
                std::cerr << "Failed to write to " << path << "\n";
                return 1;
            }
            return 0;
        } else if (std::strcmp(argv[i], "--capture") == 0) {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                benchCfg.captureAtSec = std::stod(argv[++i]);
            } else {
                benchCfg.captureAtSec = 5.0;
            }
        } else if (std::strcmp(argv[i], "--run-all") == 0) {
            runAll = true;
        } else if (std::strcmp(argv[i], "--full-analysis") == 0) {
            fullAnalysis = true;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "  --backend <vulkan|dx12|dx11|metal|opengl>  Select rendering backend (default: auto)\n"
                      << "  --gpu <index>                       Select GPU by index\n"
                      << "  --warp                               Use WARP software renderer (DX11/DX12 only)\n"
                      << "  --vsync                              Enable vertical sync (default: off)\n"
                      << "  --host-memory                        Keep particle buffer in host-visible RAM (slower on dGPU)\n"
                      << "  --flights <N>                       Set frames-in-flight count (default: 2, max: 16)\n"
                      << "  --headless                           Pure compute mode (no window/rendering/present)\n"
                      << "  --particles <count>                 Particle count (skips difficulty menu, rounded to 256)\n"
                      << "  --time <seconds>                    Auto-stop after N seconds (default: 15)\n"
                      << "  --no-time-limit                     Run until window is closed\n"
                      << "  --benchmark [frames]                Run benchmark (default: 2000 frames), then exit\n"
                      << "  --results                           List all saved benchmark results\n"
                      << "  --results-delete <id>               Delete a saved result by ID\n"
                      << "  --results-clear                     Delete all saved results\n"
                      << "  --results-export <file.csv>         Export results to CSV file\n"
                      << "  --run-all                            Benchmark every GPU x API combination, then exit\n"
                      << "  --capture [seconds]                 Auto-capture via RenderDoc at T seconds (default: 5)\n"
                      << "  --full-analysis                     Run all APIs + RenderDoc capture + Python charts (interactive)\n"
                      << "  --compare                           Compare all saved results (ranked by FPS)\n"
                      << "  --compare <id1> <id2>               Detailed side-by-side comparison of two results\n"
                      << "  --help                              Show this help\n\n"
                      << "Available Graphics APIs:";
#ifdef HAVE_VULKAN
            std::cout << " vulkan";
#endif
#ifdef HAVE_DX12
            std::cout << " dx12";
#endif
#ifdef HAVE_DX11
            std::cout << " dx11";
#endif
#ifdef HAVE_METAL
            std::cout << " metal";
#endif
#ifdef HAVE_OPENGL
            std::cout << " opengl";
#endif
            std::cout << '\n';
            return 0;
        }
    }

    const std::string shaderDir = ExeDirectory(argv[0]);

    // ---- Phase 1: Probe all GPUs and APIs ----
    auto gpus = ProbeGpus();
    PrintGpuTable(gpus);

    bool directBenchmark = (backend != "auto") || benchCfg.benchmarkMode || runAll;

    // ---- Build available backends ----
    struct BackendEntry { std::string id; bool hwOnly; };
    std::vector<BackendEntry> hwBackends, swBackends;

    auto hasHwSupport = [&](auto pred) {
        for (const auto& g : gpus) if (!g.isSoftware && pred(g)) return true;
        return false;
    };
    auto hasSwSupport = [&](auto pred) {
        for (const auto& g : gpus) if (g.isSoftware && pred(g)) return true;
        return false;
    };

    struct ApiProbe { const char* id; bool (*pred)(const GpuInfo&); };
    ApiProbe probes[] = {
#ifdef HAVE_METAL
        {"metal",  [](const GpuInfo& g){ return g.supportsMetal; }},
#endif
#ifdef HAVE_VULKAN
        {"vulkan", [](const GpuInfo& g){ return g.supportsVulkan; }},
#endif
#ifdef HAVE_DX12
        {"dx12",   [](const GpuInfo& g){ return g.supportsDX12; }},
#endif
#ifdef HAVE_DX11
        {"dx11",   [](const GpuInfo& g){ return g.supportsDX11; }},
#endif
#ifdef HAVE_OPENGL
        {"opengl", [](const GpuInfo& g){ return g.supportsOpenGL; }},
#endif
    };

    for (const auto& p : probes) {
        bool hw = hasHwSupport(p.pred);
        bool sw = hasSwSupport(p.pred);
        if (hw)       hwBackends.push_back({p.id, true});
        else if (sw)  swBackends.push_back({p.id, false});
    }

    std::vector<BackendEntry> available;
    available.insert(available.end(), hwBackends.begin(), hwBackends.end());
    available.insert(available.end(), swBackends.begin(), swBackends.end());

    if (available.empty()) {
        std::cerr << "No Graphics API available.\n";
        return 1;
    }

    // Determine recommended GPU: discrete hw > integrated hw > software.
    // Within the same tier, prefer more VRAM.
    auto gpuScore = [](const GpuInfo& g) -> int {
        if (g.isSoftware) return 0;
        if (g.isDiscrete) return 2;
        return 1;  // integrated
    };

    std::int32_t recommendedGpuIdx = 0;
    for (std::uint32_t i = 1; i < gpus.size(); ++i) {
        int scoreCur  = gpuScore(gpus[recommendedGpuIdx]);
        int scoreThis = gpuScore(gpus[i]);
        if (scoreThis > scoreCur ||
            (scoreThis == scoreCur && gpus[i].vramMB > gpus[recommendedGpuIdx].vramMB)) {
            recommendedGpuIdx = static_cast<std::int32_t>(i);
        }
    }
    std::string recommendedGpuName = gpus[recommendedGpuIdx].name;

    // Best API for the recommended GPU: Vulkan > DX12 > DX11 > Metal.
    const auto& recGpu = gpus[recommendedGpuIdx];
    std::string recommendedApi;
    if (recGpu.supportsMetal)       recommendedApi = "metal";
    else if (recGpu.supportsVulkan) recommendedApi = "vulkan";
    else if (recGpu.supportsDX12)   recommendedApi = "dx12";
    else if (recGpu.supportsDX11)   recommendedApi = "dx11";
    else if (recGpu.supportsOpenGL) recommendedApi = "opengl";

    std::string recommendedApiLabel;
    if (recommendedApi == "metal")        recommendedApiLabel = "Metal";
    else if (recommendedApi == "vulkan")  recommendedApiLabel = "Vulkan";
    else if (recommendedApi == "dx12")    recommendedApiLabel = "DirectX 12";
    else if (recommendedApi == "dx11")    recommendedApiLabel = "DirectX 11";
    else if (recommendedApi == "opengl")  recommendedApiLabel = "OpenGL 4.3";

    bool hasLastRun = false;

    // ---- --run-all: benchmark every GPU × API combination, then exit ----
    if (runAll) {
        struct RunAllEntry {
            std::int32_t gpuIdx;
            std::string  backendId;
            std::string  gpuName;
            std::string  apiLabel;
            std::int64_t luidHigh = 0;
            std::int64_t luidLow  = 0;
        };
        // Map gpus-array index + backend to the raw index each backend expects.
        auto rawIdx = [&](std::uint32_t gi, const std::string& bid) -> std::int32_t {
            const auto& g = gpus[gi];
            if (g.isSoftware) return -2;
            if (bid == "vulkan") return g.vkPhysDevIndex;
            if (bid == "dx11" || bid == "dx12") return g.dxgiRawIndex;
            return static_cast<std::int32_t>(gi);
        };
        std::vector<RunAllEntry> entries;
        for (std::uint32_t gi = 0; gi < gpus.size(); ++gi) {
            const auto& g = gpus[gi];
#ifdef HAVE_VULKAN
            if (g.supportsVulkan)
                entries.push_back({rawIdx(gi, "vulkan"), "vulkan", g.name, "Vulkan", g.luidHigh, g.luidLow});
#endif
#ifdef HAVE_DX12
            if (g.supportsDX12)
                entries.push_back({rawIdx(gi, "dx12"), "dx12", g.name, "DirectX 12", g.luidHigh, g.luidLow});
#endif
#ifdef HAVE_DX11
            if (g.supportsDX11)
                entries.push_back({rawIdx(gi, "dx11"), "dx11", g.name, "DirectX 11", g.luidHigh, g.luidLow});
#endif
#ifdef HAVE_METAL
            if (g.supportsMetal)
                entries.push_back({rawIdx(gi, "metal"), "metal", g.name, "Metal", g.luidHigh, g.luidLow});
#endif
#ifdef HAVE_OPENGL
            if (g.supportsOpenGL)
                entries.push_back({rawIdx(gi, "opengl"), "opengl", g.name, "OpenGL 4.3", g.luidHigh, g.luidLow});
#endif
        }

        if (entries.empty()) {
            std::cerr << "No runnable GPU x API combinations found.\n";
            return 1;
        }

        std::cout << "========== Run All: " << entries.size()
                  << " benchmark(s) ==========\n";
        for (std::uint32_t i = 0; i < entries.size(); ++i)
            std::cout << "  [" << (i + 1) << "] " << entries[i].apiLabel
                      << " / " << entries[i].gpuName << "\n";
        std::cout << "============================================\n";

        gpu_bench::BenchmarkConfig allCfg;
        allCfg.particleCount = benchCfg.particlesOverridden
            ? benchCfg.particleCount : 1048576;
        allCfg.difficultyLabel = benchCfg.particlesOverridden
            ? benchCfg.difficultyLabel : "Medium";
        allCfg.particlesOverridden = true;
        allCfg.vsync = false;
        if (benchCfg.maxRunTimeSec != 15.0)
            allCfg.maxRunTimeSec = benchCfg.maxRunTimeSec;
        if (benchCfg.benchmarkMode) {
            allCfg.benchmarkMode = true;
            allCfg.benchFrames = benchCfg.benchFrames;
        }

        std::uint32_t passed = 0, failed = 0;
        for (std::uint32_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            allCfg.gpuDisplayName = e.gpuName;
            allCfg.adapterLuidHigh = e.luidHigh;
            allCfg.adapterLuidLow  = e.luidLow;
            std::cout << "\n>>> [" << (i + 1) << "/" << entries.size()
                      << "] " << e.apiLabel << " / " << e.gpuName << " <<<\n";
            try {
                std::unique_ptr<gpu_bench::AppBase> app;
#ifdef HAVE_VULKAN
                if (e.backendId == "vulkan")
                    app = std::make_unique<gpu_bench::VulkanBackend>(
                        e.gpuIdx, shaderDir, allCfg);
#endif
#ifdef HAVE_DX12
                if (e.backendId == "dx12")
                    app = std::make_unique<gpu_bench::DX12Backend>(
                        e.gpuIdx, shaderDir, allCfg);
#endif
#ifdef HAVE_DX11
                if (e.backendId == "dx11")
                    app = std::make_unique<gpu_bench::DX11Backend>(
                        e.gpuIdx, shaderDir, allCfg);
#endif
#ifdef HAVE_METAL
                if (e.backendId == "metal")
                    app = std::make_unique<gpu_bench::MetalBackend>(
                        e.gpuIdx, shaderDir, allCfg);
#endif
#ifdef HAVE_OPENGL
                if (e.backendId == "opengl")
                    app = std::make_unique<gpu_bench::OpenGLBackend>(
                        e.gpuIdx, shaderDir, allCfg);
#endif
                if (!app) {
                    std::cout << "  SKIPPED (backend not available)\n";
                    ++failed;
                    continue;
                }
                app->Run();
                ++passed;
            } catch (const gpu_bench::BackToMenuException&) {
                std::cout << "  SKIPPED (user cancelled)\n";
                ++failed;
            } catch (const std::exception& ex) {
                std::cout << "  FAILED: " << ex.what() << "\n";
                ++failed;
            }
        }

        std::cout << "\n========== Run All Complete ==========\n"
                  << "  Passed: " << passed << " / " << entries.size() << "\n";
        if (failed > 0)
            std::cout << "  Failed/Skipped: " << failed << "\n";
        std::cout << "======================================\n";

        auto allResults = gpu_bench::LoadResults();
        if (!allResults.empty())
            gpu_bench::PrintComparisonTable(allResults);

        glfwTerminate();
        return (failed == entries.size()) ? 1 : 0;
    }

    // ---- Unified main loop ----
    while (true) {

        if (!directBenchmark) {
            auto saved = gpu_bench::LoadResults();

            std::cout << "\n========== GPU Benchmark ==========\n"
                      << "  [0] Quick run (" << recommendedApiLabel
                      << " / " << recommendedGpuName << " / Medium)  <- default\n"
                      << "  [1] Custom run (choose API / GPU / difficulty)\n";
            if (hasLastRun)
                std::cout << "  [2] Run again (same settings)\n";
            std::cout << "  [3] Compare results";
            if (!saved.empty()) std::cout << " (" << saved.size() << " saved)";
            std::cout << "\n  [4] Delete results\n"
                      << "  [5] Full analysis - one GPU (all APIs + RenderDoc + charts)\n"
                      << "  [6] Full analysis - all GPUs x APIs (+ RenderDoc + charts)\n"
                      << "  [7] Flights test  - one GPU (all APIs + RenderDoc, custom flights)\n"
                      << "  [8] Particle test - one GPU (all APIs + RenderDoc, custom particles)\n"
                      << "  [9] Headless compute - one GPU (all APIs, pure compute, no rendering)\n"
                      << "  [10] Exit\n"
                      << "====================================\n"
                      << "Select (default: 0): " << std::flush;

            std::string mline;
            int mchoice = 0;
            if (fullAnalysis) {
                mchoice = 5;
                fullAnalysis = false;
                std::cout << "5  (--full-analysis)\n";
            } else if (std::getline(std::cin, mline) && !mline.empty()) {
                if (!SafeStoi(mline, mchoice)) { std::cout << "Invalid input.\n"; continue; }
            }

            if (mchoice == 0) {
                backend = recommendedApi;
                gpuIndex = recommendedGpuIdx;
                benchCfg.particleCount = 1048576;
                benchCfg.difficultyLabel = "Medium";
                benchCfg.particlesOverridden = true;
            } else if (mchoice == 1) {
                backend = "auto";
                gpuIndex = -1;
                benchCfg.particlesOverridden = false;
            } else if (mchoice == 2 && hasLastRun) {
                benchCfg.particlesOverridden = true;
            } else if (mchoice == 3) {
                if (saved.empty()) {
                    std::cout << "No saved results.\n";
                    continue;
                }
                auto sorted = saved;
                std::sort(sorted.begin(), sorted.end(),
                    [](const gpu_bench::BenchmarkResult& a,
                       const gpu_bench::BenchmarkResult& b) {
                        return a.avgFps > b.avgFps;
                    });
                gpu_bench::PrintComparisonTable(saved);
                if (sorted.size() >= 2) {
                    std::cout << "Enter two rank numbers for detailed comparison (or Enter to skip):\n"
                              << "#1: " << std::flush;
                    std::string s1;
                    if (std::getline(std::cin, s1) && !s1.empty()) {
                        std::cout << "#2: " << std::flush;
                        std::string s2;
                        if (std::getline(std::cin, s2) && !s2.empty()) {
                            int i1 = 0, i2 = 0;
                            if (SafeStoi(s1, i1) && SafeStoi(s2, i2)) {
                                --i1; --i2;
                                if (i1 >= 0 && i1 < static_cast<int>(sorted.size()) &&
                                    i2 >= 0 && i2 < static_cast<int>(sorted.size()))
                                    gpu_bench::PrintDetailedComparison(sorted[i1], sorted[i2]);
                                else
                                    std::cout << "Invalid rank number.\n";
                            } else {
                                std::cout << "Invalid input.\n";
                            }
                        }
                    }
                }
                continue;
            } else if (mchoice == 4) {
                std::cout << "\n========== Delete Data ==========\n"
                          << "  [1] Delete benchmark results";
                if (!saved.empty()) std::cout << " (" << saved.size() << " saved)";
                std::cout << "\n  [2] Delete Python charts & reports\n"
                          << "  [3] Delete all (results + charts + reports)\n"
                          << "  [0] Back\n"
                          << "=================================\n"
                          << "Select: " << std::flush;

                std::string dline;
                int dchoice = 0;
                if (std::getline(std::cin, dline) && !dline.empty())
                    SafeStoi(dline, dchoice);

                auto deletePythonOutputs = [&]() {
#ifdef _WIN32
                    std::string sep = "\\";
#else
                    std::string sep = "/";
#endif
                    std::string projectRoot = shaderDir + ".." + sep + "..";
                    std::string docsDir = projectRoot + sep + "docs" + sep;
                    std::string imagesDir = docsDir + "images" + sep;
                    const char* files[] = {
                        "fps_by_gpu.png", "gpu_time_breakdown.png",
                        "cpu_overhead.png", "scaling.png"
                    };
                    int count = 0;
                    for (const char* f : files) {
                        std::string path = imagesDir + f;
                        if (std::remove(path.c_str()) == 0) ++count;
                    }
                    std::string mdPath = docsDir + "results-table.md";
                    std::string htmlPath = docsDir + "report.html";
                    if (std::remove(mdPath.c_str()) == 0) ++count;
                    if (std::remove(htmlPath.c_str()) == 0) ++count;
                    std::cout << "Deleted " << count << " Python output file(s).\n";
                };

                if (dchoice == 1) {
                    if (saved.empty()) {
                        std::cout << "No saved results.\n";
                    } else {
                        gpu_bench::PrintResultsTable(saved);
                        std::cout << "Enter numbers to delete (e.g. 1,3,5 or 'all', Enter to go back): #"
                                  << std::flush;
                        std::string did;
                        if (std::getline(std::cin, did) && !did.empty()) {
                            if (did == "all") {
                                gpu_bench::ClearResults();
                                std::cout << "All results cleared.\n";
                            } else {
                                std::vector<std::string> idsToDelete;
                                std::istringstream ss(did);
                                std::string token;
                                while (std::getline(ss, token, ',')) {
                                    int idx = 0;
                                    if (!SafeStoi(token, idx)) {
                                        std::cout << "Invalid input: " << token << "\n";
                                        continue;
                                    }
                                    --idx;
                                    if (idx >= 0 && idx < static_cast<int>(saved.size()))
                                        idsToDelete.push_back(saved[idx].id);
                                    else
                                        std::cout << "Invalid number: " << (idx + 1) << "\n";
                                }
                                for (const auto& id : idsToDelete) {
                                    if (gpu_bench::DeleteResult(id))
                                        std::cout << "Deleted: " << id << "\n";
                                }
                                if (!idsToDelete.empty())
                                    std::cout << "Deleted " << idsToDelete.size() << " result(s).\n";
                            }
                        }
                    }
                } else if (dchoice == 2) {
                    deletePythonOutputs();
                } else if (dchoice == 3) {
                    gpu_bench::ClearResults();
                    std::cout << "All benchmark results cleared.\n";
                    deletePythonOutputs();
                }
                continue;
            } else if (mchoice == 5 || mchoice == 6) {
                // ---- Full Analysis: benchmark + RenderDoc capture + Python charts ----
                // [5] = one GPU (user selects, default best), [6] = every GPU
                struct RunAllEntry {
                    std::int32_t gpuIdx;
                    std::string  backendId;
                    std::string  gpuName;
                    std::string  apiLabel;
                    std::int64_t luidHigh = 0;
                    std::int64_t luidLow  = 0;
                };

                std::int32_t selectedGpuForAll = -1;

                if (mchoice == 5) {
                    PrintGpuTable(gpus);
                    std::uint32_t defaultGpu = static_cast<std::uint32_t>(recommendedGpuIdx);
                    std::cout << "Select GPU [0-" << (gpus.size() - 1)
                              << "] (default: " << defaultGpu << "): " << std::flush;
                    std::string gline;
                    std::uint32_t gchoice = defaultGpu;
                    if (std::getline(std::cin, gline) && !gline.empty()) {
                        int tmp = 0;
                        if (SafeStoi(gline, tmp) && tmp >= 0 &&
                            static_cast<std::uint32_t>(tmp) < gpus.size())
                            gchoice = static_cast<std::uint32_t>(tmp);
                    }
                    selectedGpuForAll = static_cast<std::int32_t>(gchoice);
                }

                std::vector<RunAllEntry> entries;
                for (std::uint32_t gi = 0; gi < gpus.size(); ++gi) {
                    if (selectedGpuForAll >= 0 &&
                        gi != static_cast<std::uint32_t>(selectedGpuForAll))
                        continue;

                    const auto& g = gpus[gi];
                    auto faRawIdx = [&](const std::string& bid) -> std::int32_t {
                        if (g.isSoftware) return -2;
                        if (bid == "vulkan") return g.vkPhysDevIndex;
                        if (bid == "dx11" || bid == "dx12") return g.dxgiRawIndex;
                        return static_cast<std::int32_t>(gi);
                    };
#ifdef HAVE_METAL
                    if (g.supportsMetal)
                        entries.push_back({faRawIdx("metal"), "metal", g.name, "Metal", g.luidHigh, g.luidLow});
#endif
#ifdef HAVE_VULKAN
                    if (g.supportsVulkan)
                        entries.push_back({faRawIdx("vulkan"), "vulkan", g.name, "Vulkan", g.luidHigh, g.luidLow});
#endif
#ifdef HAVE_DX12
                    if (g.supportsDX12)
                        entries.push_back({faRawIdx("dx12"), "dx12", g.name, "DirectX 12", g.luidHigh, g.luidLow});
#endif
#ifdef HAVE_DX11
                    if (g.supportsDX11)
                        entries.push_back({faRawIdx("dx11"), "dx11", g.name, "DirectX 11", g.luidHigh, g.luidLow});
#endif
#ifdef HAVE_OPENGL
                    if (g.supportsOpenGL) {
#ifdef _WIN32
                        if (gi == 0)
                            entries.push_back({faRawIdx("opengl"), "opengl", g.name, "OpenGL 4.3", g.luidHigh, g.luidLow});
#else
                        entries.push_back({faRawIdx("opengl"), "opengl", g.name, "OpenGL 4.3", g.luidHigh, g.luidLow});
#endif
                    }
#endif
                }

                if (entries.empty()) {
                    std::cout << "No runnable API combinations found.\n";
                    continue;
                }

                std::string scope = (mchoice == 5) ? "One GPU" : "All GPUs";
                std::cout << "\n====== Full Analysis (" << scope << "): "
                          << entries.size() << " benchmark(s) ======\n";
                for (std::uint32_t i = 0; i < entries.size(); ++i) {
                    std::cout << "  [" << (i + 1) << "] " << entries[i].apiLabel
                              << " / " << entries[i].gpuName;
#ifdef _WIN32
                    if (entries[i].backendId == "opengl")
                        std::cout << "  (Win: always uses system default GPU)";
#endif
                    std::cout << "\n";
                }
                std::cout << "  + RenderDoc capture at 5s mark (if RenderDoc detected)\n"
                          << "  + Python chart generation after all runs\n"
                          << "=======================================================\n"
                          << "Proceed? (Y/n): " << std::flush;
                std::string confirm;
                std::getline(std::cin, confirm);
                if (!confirm.empty() && confirm[0] != 'Y' && confirm[0] != 'y')
                    continue;

                gpu_bench::BenchmarkConfig faCfg;
                faCfg.particleCount = 1048576;
                faCfg.difficultyLabel = "Medium";
                faCfg.particlesOverridden = true;
                faCfg.vsync = false;
                faCfg.captureAtSec = 5.0;

                std::uint32_t passed = 0, failed = 0;
                std::vector<std::string> rdcFiles;

                for (std::uint32_t i = 0; i < entries.size(); ++i) {
                    const auto& e = entries[i];
                    faCfg.gpuDisplayName = e.gpuName;
                    faCfg.adapterLuidHigh = e.luidHigh;
                    faCfg.adapterLuidLow  = e.luidLow;
                    std::cout << "\n>>> [" << (i + 1) << "/" << entries.size()
                              << "] " << e.apiLabel << " / " << e.gpuName
                              << " (15s + RenderDoc @ 5s) <<<\n";
                    try {
                        std::unique_ptr<gpu_bench::AppBase> app;
#ifdef HAVE_VULKAN
                        if (e.backendId == "vulkan")
                            app = std::make_unique<gpu_bench::VulkanBackend>(
                                e.gpuIdx, shaderDir, faCfg);
#endif
#ifdef HAVE_DX12
                        if (e.backendId == "dx12")
                            app = std::make_unique<gpu_bench::DX12Backend>(
                                e.gpuIdx, shaderDir, faCfg);
#endif
#ifdef HAVE_DX11
                        if (e.backendId == "dx11")
                            app = std::make_unique<gpu_bench::DX11Backend>(
                                e.gpuIdx, shaderDir, faCfg);
#endif
#ifdef HAVE_METAL
                        if (e.backendId == "metal")
                            app = std::make_unique<gpu_bench::MetalBackend>(
                                e.gpuIdx, shaderDir, faCfg);
#endif
#ifdef HAVE_OPENGL
                        if (e.backendId == "opengl") {
#ifdef _WIN32
                            std::cout << "  NOTE: OpenGL on Windows cannot select GPU "
                                         "- using system default.\n";
#endif
                            app = std::make_unique<gpu_bench::OpenGLBackend>(
                                e.gpuIdx, shaderDir, faCfg);
                        }
#endif
                        if (!app) {
                            std::cout << "  SKIPPED (backend not available)\n";
                            ++failed;
                            continue;
                        }
                        app->Run();
                        if (!app->GetLastCapturePath().empty())
                            rdcFiles.push_back(app->GetLastCapturePath());
                        ++passed;
                    } catch (const gpu_bench::BackToMenuException&) {
                        std::cout << "  SKIPPED (user cancelled)\n";
                        ++failed;
                    } catch (const std::exception& ex) {
                        std::cout << "  FAILED: " << ex.what() << "\n";
                        ++failed;
                    }
                }

                std::cout << "\n========== Benchmark Phase Complete ==========\n"
                          << "  Passed: " << passed << " / " << entries.size() << "\n";
                if (failed > 0)
                    std::cout << "  Failed/Skipped: " << failed << "\n";

                auto allResults = gpu_bench::LoadResults();
                if (!allResults.empty())
                    gpu_bench::PrintComparisonTable(allResults);

                // shaderDir = exe directory (e.g. build/Release/)
                // project root is two levels up: build/Release/../../
#ifdef _WIN32
                std::string sep = "\\";
#else
                std::string sep = "/";
#endif
                std::string projectRoot = shaderDir + ".." + sep + "..";
                std::string scriptsDir = projectRoot + sep + "scripts" + sep;
                std::string docsDir = projectRoot + sep + "docs" + sep;
                std::string rdocCapDir = projectRoot + sep + "rdoc_captures" + sep;

                // Helper: run a command inside the project root to avoid
                // space-in-path issues with cmd.exe quoting.
                auto runInProjectRoot = [&](const std::string& innerCmd) -> int {
#ifdef _WIN32
                    std::string full = "\"cd /d \"" + projectRoot
                        + "\" && " + innerCmd + "\"";
#else
                    std::string full = "cd \"" + projectRoot
                        + "\" && " + innerCmd;
#endif
                    return std::system(full.c_str());
                };

                // ---- RenderDoc capture -> Chrome JSON conversion ----
                if (!rdcFiles.empty()) {
                    std::cout << "\n========== Converting RenderDoc Captures ==========\n";
                    for (std::uint32_t ci = 0; ci < rdcFiles.size(); ++ci) {
                        std::string jsonOut = rdcFiles[ci];
                        auto dotPos = jsonOut.rfind('.');
                        if (dotPos != std::string::npos)
                            jsonOut = jsonOut.substr(0, dotPos);
                        jsonOut += ".json";

#ifdef _WIN32
                        std::string cmd =
                            "\"\"C:\\Program Files\\RenderDoc\\renderdoccmd.exe\" convert"
                            " -f \"" + rdcFiles[ci] + "\""
                            " -c chrome.json"
                            " -o \"" + jsonOut + "\"\"";
#else
                        std::string cmd = "renderdoccmd convert"
                            " -f \"" + rdcFiles[ci] + "\""
                            " -c chrome.json"
                            " -o \"" + jsonOut + "\"";
#endif

                        std::cout << "  [" << (ci + 1) << "/" << rdcFiles.size()
                                  << "] " << rdcFiles[ci] << "\n";
                        int rcConv = std::system(cmd.c_str());
                        if (rcConv != 0)
                            std::cout << "    WARNING: conversion failed (exit "
                                      << rcConv << ")\n";
                        else
                            std::cout << "    -> " << jsonOut << "\n";
                    }
                    std::cout << "====================================================\n";
                }

                // ---- RenderDoc timing analysis ----
                std::string resultsPath = gpu_bench::ResultsFilePath();
                if (!rdcFiles.empty()) {
                    std::string rdocCmd =
                        "python scripts" + sep + "rdoc_analyse.py"
                        " --captures rdoc_captures"
                        " --results \"" + resultsPath + "\""
                        " --output docs" + sep + "rdoc_comparison.md";
                    std::cout << "\n========== RenderDoc Timing Analysis ==========\n";
                    int rcRdoc = runInProjectRoot(rdocCmd);
                    if (rcRdoc != 0)
                        std::cout << "  WARNING: rdoc_analyse.py failed (exit "
                                  << rcRdoc << ")\n";
                    else
                        std::cout << "  Report saved to docs/rdoc_comparison.md\n";
                    std::cout << "================================================\n";
                }

                // ---- Python chart generation ----
                std::cout << "\n========== Generating Python Charts ==========\n";

                std::string cmdPlot =
                    "python scripts" + sep + "plot_results.py --save docs" + sep + "images";
                std::string cmdExportMd =
                    "python scripts" + sep + "export_report.py --md docs" + sep + "results-table.md";
                std::string cmdExportHtml =
                    "python scripts" + sep + "export_report.py --html docs" + sep + "report.html";

                std::cout << "  [1/3] Generating charts...\n";
                int rc1 = runInProjectRoot(cmdPlot);
                if (rc1 != 0)
                    std::cout << "  WARNING: plot_results.py failed (exit " << rc1
                              << "). Is matplotlib installed? Run: pip install -r scripts/requirements.txt\n";
                else
                    std::cout << "  Charts saved to docs/images/\n";

                std::cout << "  [2/3] Exporting Markdown table...\n";
                int rc2 = runInProjectRoot(cmdExportMd);
                if (rc2 != 0)
                    std::cout << "  WARNING: export_report.py --md failed (exit " << rc2 << ")\n";
                else
                    std::cout << "  Markdown table saved to docs/results-table.md\n";

                std::cout << "  [3/3] Exporting HTML report...\n";
                int rc3 = runInProjectRoot(cmdExportHtml);
                if (rc3 != 0)
                    std::cout << "  WARNING: export_report.py --html failed (exit " << rc3 << ")\n";
                else
                    std::cout << "  HTML report saved to docs/report.html\n";

                std::cout << "\n========== Full Analysis Complete ==========\n";
                if (rc1 == 0 && rc2 == 0 && rc3 == 0)
                    std::cout << "All outputs generated successfully.\n";
                else
                    std::cout << "Some Python scripts failed. Check if dependencies are installed:\n"
                              << "  pip install -r scripts/requirements.txt\n";
                std::cout << "============================================\n";

                hasLastRun = (passed > 0);
                directBenchmark = false;
                continue;

            } else if (mchoice == 7 || mchoice == 8 || mchoice == 9) {
                // ---- Options 7/8/9: Flights test / Particle test / Headless compute ----
                // Similar to option 5 (Full analysis - one GPU) but with specific overrides.
                struct RunAllEntry {
                    std::int32_t gpuIdx;
                    std::string  backendId;
                    std::string  gpuName;
                    std::string  apiLabel;
                    std::int64_t luidHigh = 0;
                    std::int64_t luidLow  = 0;
                };

                PrintGpuTable(gpus);
                std::uint32_t defaultGpu = static_cast<std::uint32_t>(recommendedGpuIdx);
                std::cout << "Select GPU [0-" << (gpus.size() - 1)
                          << "] (default: " << defaultGpu << "): " << std::flush;
                std::string gline;
                std::uint32_t gchoice = defaultGpu;
                if (std::getline(std::cin, gline) && !gline.empty()) {
                    int tmp = 0;
                    if (SafeStoi(gline, tmp) && tmp >= 0 &&
                        static_cast<std::uint32_t>(tmp) < gpus.size())
                        gchoice = static_cast<std::uint32_t>(tmp);
                }
                gpu_bench::BenchmarkConfig testCfg;
                testCfg.particleCount = 1048576;
                testCfg.difficultyLabel = "Medium";
                testCfg.particlesOverridden = true;
                testCfg.vsync = false;

                std::string testLabel;
                if (mchoice == 7) {
                    // Flights test: ask for flights count
                    std::cout << "Enter frames-in-flight count (default: 2): " << std::flush;
                    std::string fline;
                    int flights = 2;
                    if (std::getline(std::cin, fline) && !fline.empty())
                        SafeStoi(fline, flights);
                    if (flights < 1) flights = 1;
                    if (flights > 16) flights = 16;
                    testCfg.framesInFlight = static_cast<std::uint32_t>(flights);
                    testCfg.captureAtSec = 5.0;  // RenderDoc capture
                    testLabel = "Flights=" + std::to_string(flights);
                } else if (mchoice == 8) {
                    // Particle count test: ask for particle count
                    struct Preset { const char* name; std::uint32_t count; };
                    static const Preset presets[] = {
                        {"Light",   65536},
                        {"Medium",  1048576},
                        {"Heavy",   4194304},
                        {"Extreme", 16777216},
                    };
                    std::cout << "\nParticle count presets:\n";
                    for (int p = 0; p < 4; ++p) {
                        std::cout << "  [" << p << "] " << presets[p].name
                                  << " (" << presets[p].count << ")";
                        if (p == 1) std::cout << "  <- default";
                        std::cout << "\n";
                    }
                    std::cout << "Select [0-3] or enter custom count (default: 1): " << std::flush;
                    std::string pline;
                    std::uint32_t pchoice = 1;
                    if (std::getline(std::cin, pline) && !pline.empty()) {
                        int tmp = 0;
                        if (SafeStoi(pline, tmp)) {
                            if (tmp >= 0 && tmp <= 3)
                                pchoice = static_cast<std::uint32_t>(tmp);
                            else if (tmp > 3) {
                                // Treat as raw particle count, round to 256
                                testCfg.particleCount = static_cast<std::uint32_t>((tmp / 256) * 256);
                                if (testCfg.particleCount == 0) testCfg.particleCount = 256;
                                testCfg.difficultyLabel = "Custom";
                                pchoice = 99;  // skip preset
                            }
                        }
                    }
                    if (pchoice <= 3) {
                        testCfg.particleCount = presets[pchoice].count;
                        testCfg.difficultyLabel = presets[pchoice].name;
                    }
                    testCfg.captureAtSec = 5.0;  // RenderDoc capture
                    testLabel = "Particles=" + std::to_string(testCfg.particleCount);
                } else {
                    // Headless compute: no rendering, no RenderDoc
                    testCfg.headless = true;
                    testCfg.captureAtSec = -1.0;  // no RenderDoc
                    testLabel = "Headless";
                }

                // Build entry list for the selected GPU
                std::vector<RunAllEntry> entries;
                {
                    const auto& g = gpus[gchoice];
                    auto faRawIdx = [&](const std::string& bid) -> std::int32_t {
                        if (g.isSoftware) return -2;
                        if (bid == "vulkan") return g.vkPhysDevIndex;
                        if (bid == "dx11" || bid == "dx12") return g.dxgiRawIndex;
                        return static_cast<std::int32_t>(gchoice);
                    };
#ifdef HAVE_METAL
                    if (g.supportsMetal)
                        entries.push_back({faRawIdx("metal"), "metal", g.name, "Metal", g.luidHigh, g.luidLow});
#endif
#ifdef HAVE_VULKAN
                    if (g.supportsVulkan)
                        entries.push_back({faRawIdx("vulkan"), "vulkan", g.name, "Vulkan", g.luidHigh, g.luidLow});
#endif
#ifdef HAVE_DX12
                    if (g.supportsDX12)
                        entries.push_back({faRawIdx("dx12"), "dx12", g.name, "DirectX 12", g.luidHigh, g.luidLow});
#endif
#ifdef HAVE_DX11
                    if (g.supportsDX11)
                        entries.push_back({faRawIdx("dx11"), "dx11", g.name, "DirectX 11", g.luidHigh, g.luidLow});
#endif
#ifdef HAVE_OPENGL
                    if (g.supportsOpenGL) {
#ifdef _WIN32
                        if (gchoice == 0)
                            entries.push_back({faRawIdx("opengl"), "opengl", g.name, "OpenGL 4.3", g.luidHigh, g.luidLow});
#else
                        entries.push_back({faRawIdx("opengl"), "opengl", g.name, "OpenGL 4.3", g.luidHigh, g.luidLow});
#endif
                    }
#endif
                }

                if (entries.empty()) {
                    std::cout << "No runnable API combinations found.\n";
                    continue;
                }

                std::cout << "\n====== " << testLabel << " Test: "
                          << entries.size() << " benchmark(s) ======\n";
                for (std::uint32_t i = 0; i < entries.size(); ++i) {
                    std::cout << "  [" << (i + 1) << "] " << entries[i].apiLabel
                              << " / " << entries[i].gpuName << "\n";
                }
                if (testCfg.captureAtSec > 0.0)
                    std::cout << "  + RenderDoc capture at 5s mark (if RenderDoc detected)\n";
                if (testCfg.headless)
                    std::cout << "  + Pure compute mode (no window/rendering/present)\n";
                std::cout << "=======================================================\n"
                          << "Proceed? (Y/n): " << std::flush;
                std::string confirm;
                std::getline(std::cin, confirm);
                if (!confirm.empty() && confirm[0] != 'Y' && confirm[0] != 'y')
                    continue;

                std::uint32_t passed = 0, failed = 0;
                std::vector<std::string> rdcFiles;

                for (std::uint32_t i = 0; i < entries.size(); ++i) {
                    const auto& e = entries[i];
                    testCfg.gpuDisplayName = e.gpuName;
                    testCfg.adapterLuidHigh = e.luidHigh;
                    testCfg.adapterLuidLow  = e.luidLow;
                    std::cout << "\n>>> [" << (i + 1) << "/" << entries.size()
                              << "] " << e.apiLabel << " / " << e.gpuName
                              << " (" << testLabel << ", 15s";
                    if (testCfg.captureAtSec > 0.0)
                        std::cout << " + RenderDoc @ 5s";
                    std::cout << ") <<<\n";
                    try {
                        std::unique_ptr<gpu_bench::AppBase> app;
#ifdef HAVE_VULKAN
                        if (e.backendId == "vulkan")
                            app = std::make_unique<gpu_bench::VulkanBackend>(
                                e.gpuIdx, shaderDir, testCfg);
#endif
#ifdef HAVE_DX12
                        if (e.backendId == "dx12")
                            app = std::make_unique<gpu_bench::DX12Backend>(
                                e.gpuIdx, shaderDir, testCfg);
#endif
#ifdef HAVE_DX11
                        if (e.backendId == "dx11")
                            app = std::make_unique<gpu_bench::DX11Backend>(
                                e.gpuIdx, shaderDir, testCfg);
#endif
#ifdef HAVE_METAL
                        if (e.backendId == "metal")
                            app = std::make_unique<gpu_bench::MetalBackend>(
                                e.gpuIdx, shaderDir, testCfg);
#endif
#ifdef HAVE_OPENGL
                        if (e.backendId == "opengl") {
#ifdef _WIN32
                            std::cout << "  NOTE: OpenGL on Windows cannot select GPU "
                                         "- using system default.\n";
#endif
                            app = std::make_unique<gpu_bench::OpenGLBackend>(
                                e.gpuIdx, shaderDir, testCfg);
                        }
#endif
                        if (!app) {
                            std::cout << "  SKIPPED (backend not available)\n";
                            ++failed;
                            continue;
                        }
                        app->Run();
                        if (!app->GetLastCapturePath().empty())
                            rdcFiles.push_back(app->GetLastCapturePath());
                        ++passed;
                    } catch (const gpu_bench::BackToMenuException&) {
                        std::cout << "  SKIPPED (user cancelled)\n";
                        ++failed;
                    } catch (const std::exception& ex) {
                        std::cout << "  FAILED: " << ex.what() << "\n";
                        ++failed;
                    }
                }

                std::cout << "\n========== " << testLabel << " Test Complete ==========\n"
                          << "  Passed: " << passed << " / " << entries.size() << "\n";
                if (failed > 0)
                    std::cout << "  Failed/Skipped: " << failed << "\n";

                auto allResults = gpu_bench::LoadResults();
                if (!allResults.empty())
                    gpu_bench::PrintComparisonTable(allResults);

                // RenderDoc capture conversion (same as option 5)
                if (!rdcFiles.empty()) {
                    std::cout << "\n========== Converting RenderDoc Captures ==========\n";
                    for (std::uint32_t ci = 0; ci < rdcFiles.size(); ++ci) {
                        std::string jsonOut = rdcFiles[ci];
                        auto dotPos = jsonOut.rfind('.');
                        if (dotPos != std::string::npos)
                            jsonOut = jsonOut.substr(0, dotPos);
                        jsonOut += ".json";

#ifdef _WIN32
                        std::string cmd =
                            "\"\"C:\\Program Files\\RenderDoc\\renderdoccmd.exe\" convert"
                            " -f \"" + rdcFiles[ci] + "\""
                            " -c chrome.json"
                            " -o \"" + jsonOut + "\"\"";
#else
                        std::string cmd = "renderdoccmd convert"
                            " -f \"" + rdcFiles[ci] + "\""
                            " -c chrome.json"
                            " -o \"" + jsonOut + "\"";
#endif
                        std::cout << "  [" << (ci + 1) << "/" << rdcFiles.size()
                                  << "] " << rdcFiles[ci] << "\n";
                        int rcConv = std::system(cmd.c_str());
                        if (rcConv != 0)
                            std::cout << "    WARNING: conversion failed (exit "
                                      << rcConv << ")\n";
                        else
                            std::cout << "    -> " << jsonOut << "\n";
                    }
                    std::cout << "====================================================\n";
                }

                std::cout << "============================================\n";

                hasLastRun = (passed > 0);
                directBenchmark = false;
                continue;

            } else if (mchoice == 10) {
                return 0;
            } else {
                continue;
            }
        }

        // ---- Backend selection (when backend == "auto") ----
        std::string selectedBackend = backend;
        bool warp = useWarp;

        if (selectedBackend == "auto") {
            if (available.empty()) {
                std::cerr << "No Graphics API available.\n";
                return 1;
            }

            // Find the best default: first hardware backend, else first overall.
            std::uint32_t defaultChoice = 0;
            for (std::uint32_t i = 0; i < available.size(); ++i) {
                if (available[i].hwOnly) { defaultChoice = i; break; }
            }

            PrintGpuTable(gpus);
            std::cout << "Available Graphics APIs:\n";
            for (std::uint32_t i = 0; i < available.size(); ++i) {
                std::string note;
                if (available[i].id == "metal")       note = "Metal";
                else if (available[i].id == "vulkan")  note = "Vulkan";
                else if (available[i].id == "dx12")    note = "DirectX 12";
                else if (available[i].id == "dx11")    note = "DirectX 11";
                else if (available[i].id == "opengl")  note = "OpenGL 4.3";
                if (!available[i].hwOnly)
                    note += "  [Software only - runs on CPU]";
                if (i == defaultChoice)
                    note += "  <- default";
                std::cout << "  [" << i << "] " << note << "\n";
            }

            std::cout << "Select Graphics API [0-" << (available.size() - 1)
                      << "] (default: " << defaultChoice << "): " << std::flush;
            std::string line;
            std::uint32_t choice = defaultChoice;
            if (std::getline(std::cin, line) && !line.empty()) {
                int tmp = 0;
                if (!SafeStoi(line, tmp) || tmp < 0 ||
                    static_cast<std::uint32_t>(tmp) >= available.size()) {
                    std::cerr << "Invalid index, try again.\n\n";
                    continue;
                }
                choice = static_cast<std::uint32_t>(tmp);
            }
            selectedBackend = available[choice].id;
            std::cout << "[Graphics API] Selected: " << selectedBackend << std::endl;
        }

        if (warp && selectedBackend != "dx11" && selectedBackend != "dx12") {
            warp = false;
        }

#ifdef __linux__
        // On Linux, let users interactively choose a GPU for OpenGL
        // via DRI_PRIME, since OpenGL has no built-in GPU enumeration.
        if (selectedBackend == "opengl" && gpuIndex < 0 && gpus.size() > 1) {
            std::cout << "\nSelect GPU for OpenGL (DRI_PRIME):\n";
            for (std::uint32_t i = 0; i < gpus.size(); ++i) {
                std::string label = gpus[i].name;
                if (gpus[i].isSoftware)       label += " (Software)";
                else if (gpus[i].isDiscrete)  label += " (Discrete)";
                else                          label += " (Integrated)";
                std::cout << "  [" << i << "] " << label;
                if (i == 0) std::cout << "  <- default";
                std::cout << "\n";
            }
            std::cout << "Select GPU [0-" << (gpus.size() - 1)
                      << "] (default: 0): " << std::flush;
            std::string gline;
            if (std::getline(std::cin, gline) && !gline.empty()) {
                int gi = 0;
                if (SafeStoi(gline, gi) && gi >= 0 &&
                    static_cast<std::uint32_t>(gi) < gpus.size())
                    gpuIndex = static_cast<std::int32_t>(gi);
            }
        }
#endif

        // -- Difficulty selection (only when --particles not given) --
        if (!benchCfg.particlesOverridden && !benchCfg.benchmarkMode) {
            struct Preset { const char* name; std::uint32_t count; const char* vram; };
            static const Preset presets[] = {
                {"Light",   65536,    "  2 MB"},
                {"Medium",  1048576,  " 32 MB"},
                {"Heavy",   4194304,  "128 MB"},
                {"Extreme", 16777216, "512 MB"},
            };
            std::cout << "\nDifficulty presets:\n";
            for (int p = 0; p < 4; ++p) {
                std::cout << "  [" << p << "] " << presets[p].name
                          << std::string(10 - std::strlen(presets[p].name), ' ')
                          << presets[p].count << " particles (" << presets[p].vram << ")";
                if (p == 1) std::cout << "  <- default";
                std::cout << "\n";
            }
            std::cout << "Select difficulty [0-3] (default: 1): " << std::flush;
            std::string dline;
            std::uint32_t dchoice = 1;
            if (std::getline(std::cin, dline) && !dline.empty()) {
                int tmp = 0;
                if (SafeStoi(dline, tmp) && tmp >= 0 && tmp <= 3)
                    dchoice = static_cast<std::uint32_t>(tmp);
            }
            benchCfg.particleCount = presets[dchoice].count;
            benchCfg.difficultyLabel = presets[dchoice].name;
        }

        // Map gpus-array index to backend-specific raw index.
        // DX11/DX12 use the DXGI EnumAdapters1 index; Vulkan uses the
        // vkEnumeratePhysicalDevices index.  This avoids mismatches when
        // gpus ordering differs from DXGI ordering (e.g. Vulkan-inserted entries).
        std::int32_t effectiveGpuIndex = -1;
        if (warp) {
            effectiveGpuIndex = -2;
        } else if (gpuIndex >= 0 && static_cast<std::size_t>(gpuIndex) < gpus.size()) {
            if (selectedBackend == "vulkan") {
                effectiveGpuIndex = gpus[gpuIndex].vkPhysDevIndex;
            } else if (selectedBackend == "dx11" || selectedBackend == "dx12") {
                effectiveGpuIndex = gpus[gpuIndex].dxgiRawIndex;
            } else {
                effectiveGpuIndex = gpuIndex;  // Metal / OpenGL: use as-is
            }
        }

        if (selectedBackend == "opengl" && gpus.size() > 1) {
#ifdef __linux__
            // On Linux, DRI_PRIME selects the GPU for Mesa OpenGL drivers.
            // Must be set before GLFW creates the OpenGL context.
            if (gpuIndex >= 0) {
                std::string prime = std::to_string(gpuIndex);
                setenv("DRI_PRIME", prime.c_str(), 1);
                std::cout << "[OpenGL] Set DRI_PRIME=" << prime
                          << " for GPU selection.\n" << std::endl;
            } else {
                std::cout << "\n[OpenGL] Tip: use DRI_PRIME=N to select a GPU, "
                             "e.g. DRI_PRIME=1 ./gpu_benchmark --backend opengl\n"
                          << std::endl;
            }
#elif defined(_WIN32)
            // On Windows, OpenGL has no standard per-GPU selection API.
            // NvOptimusEnablement / AmdPowerXpressRequestHighPerformance
            // (exported above) hint the driver on Optimus/PowerXpress laptops,
            // but have no effect on desktop multi-GPU systems.
            if (gpuIndex < 0) {
                std::cout << "\n[OpenGL] Note: OpenGL uses the OS-assigned GPU "
                             "(typically the discrete GPU).\n"
                          << "  To override, go to Windows Settings > System > "
                             "Display > Graphics\n"
                          << "  and assign this executable to the desired GPU.\n"
                          << std::endl;
            }
#endif
        }

        // Set display name and LUID for multi-GPU disambiguation.
        if (gpuIndex >= 0 && static_cast<std::size_t>(gpuIndex) < gpus.size()) {
            benchCfg.gpuDisplayName = gpus[gpuIndex].name;
            benchCfg.adapterLuidHigh = gpus[gpuIndex].luidHigh;
            benchCfg.adapterLuidLow  = gpus[gpuIndex].luidLow;
        }

        // -- Create and run the backend --
        try {
            std::unique_ptr<gpu_bench::AppBase> app;

#ifdef HAVE_VULKAN
            if (selectedBackend == "vulkan")
                app = std::make_unique<gpu_bench::VulkanBackend>(
                    effectiveGpuIndex, shaderDir, benchCfg);
#endif
#ifdef HAVE_DX12
            if (selectedBackend == "dx12")
                app = std::make_unique<gpu_bench::DX12Backend>(
                    effectiveGpuIndex, shaderDir, benchCfg);
#endif
#ifdef HAVE_DX11
            if (selectedBackend == "dx11")
                app = std::make_unique<gpu_bench::DX11Backend>(
                    effectiveGpuIndex, shaderDir, benchCfg);
#endif
#ifdef HAVE_METAL
            if (selectedBackend == "metal")
                app = std::make_unique<gpu_bench::MetalBackend>(
                    effectiveGpuIndex, shaderDir, benchCfg);
#endif
#ifdef HAVE_OPENGL
            if (selectedBackend == "opengl")
                app = std::make_unique<gpu_bench::OpenGLBackend>(
                    effectiveGpuIndex, shaderDir, benchCfg);
#endif

            if (!app) {
                std::cerr << "Graphics API '" << selectedBackend
                          << "' is not compiled in or failed to initialise.\n";
                return 1;
            }

            std::cout << "Backend: " << app->GetBackendName()
                      << "  |  V-Sync: " << (benchCfg.vsync ? "ON" : "OFF")
                      << "  |  Memory: " << (benchCfg.hostMemory ? "Host-visible" : "Device-local")
                      << "  |  Particles: " << benchCfg.particleCount
                      << " (" << benchCfg.difficultyLabel << ")";
            if (benchCfg.benchmarkMode)
                std::cout << "  |  Benchmark: " << benchCfg.benchFrames << " frames";
            else if (benchCfg.maxRunTimeSec > 0.0)
                std::cout << "  |  Auto-stop: " << static_cast<int>(benchCfg.maxRunTimeSec) << "s";
            std::cout << '\n';

            app->Run();

            if (directBenchmark) break;

            hasLastRun = true;
            backend = selectedBackend;
            continue;

        } catch (const gpu_bench::BackToMenuException&) {
            std::cout << "\nReturning to menu...\n" << std::endl;
            backend = "auto";
            gpuIndex = -1;
            benchCfg.particlesOverridden = false;
            continue;
        } catch (const std::exception& ex) {
            std::cout << "Fatal error: " << ex.what() << std::endl;
            return 1;
        } catch (...) {
            std::cout << "Fatal error: unknown exception" << std::endl;
            return 1;
        }
    }

    glfwTerminate();
    return 0;
}
