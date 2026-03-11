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
#endif
#ifdef HAVE_OPENGL
#include "opengl_backend.h"
#endif

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

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
};

static std::vector<GpuInfo> ProbeGpus() {
    std::vector<GpuInfo> gpus;

#ifdef _WIN32
    // --- DXGI enumeration (deduplicated by VendorId + DeviceId + SubSysId) ---
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        struct DevKey { UINT vendor; UINT device; UINT subSys; };
        std::vector<DevKey> seen;

        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);

            bool dup = false;
            for (const auto& s : seen) {
                if (s.vendor == desc.VendorId &&
                    s.device == desc.DeviceId &&
                    s.subSys == desc.SubSysId) {
                    dup = true; break;
                }
            }
            if (dup) { adapter.Reset(); continue; }
            seen.push_back({desc.VendorId, desc.DeviceId, desc.SubSysId});

            GpuInfo info;
            char name[256]{};
            wcstombs(name, desc.Description, sizeof(name) - 1);
            info.name = name;
            info.isSoftware = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            info.vramMB = desc.DedicatedVideoMemory / (1024 * 1024);

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
                wcstombs(hpName, hpDesc.Description, sizeof(hpName) - 1);
                for (auto& g : gpus) {
                    if (g.name == hpName) { g.isDiscrete = true; break; }
                }
                hpAdapter.Reset();
                break;  // only the first non-software high-perf adapter is discrete
            }
        }
    }

#endif  // _WIN32

#ifdef __APPLE__
    {
        GpuInfo info;
        info.name = "Apple GPU (Metal)";
        info.supportsMetal = true;
        gpus.push_back(info);
    }
#endif

    // --- Vulkan probe (if compiled in) ---
#ifdef HAVE_VULKAN
    {
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.apiVersion = VK_API_VERSION_1_2;

        VkInstanceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &appInfo;

        VkInstance inst = VK_NULL_HANDLE;
        if (vkCreateInstance(&ci, nullptr, &inst) == VK_SUCCESS) {
            std::uint32_t count = 0;
            vkEnumeratePhysicalDevices(inst, &count, nullptr);
            std::vector<VkPhysicalDevice> devs(count);
            vkEnumeratePhysicalDevices(inst, &count, devs.data());

            for (const auto& dev : devs) {
                VkPhysicalDeviceProperties props{};
                vkGetPhysicalDeviceProperties(dev, &props);

                bool vkIsSoftware = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU);
                bool vkIsDiscrete = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);

                // Match Vulkan device to an existing DXGI entry by name.
                bool matched = false;
                for (auto& gpu : gpus) {
                    if (gpu.name.find(props.deviceName) != std::string::npos ||
                        std::string(props.deviceName).find(gpu.name) != std::string::npos) {
                        gpu.supportsVulkan = true;
                        if (vkIsSoftware) gpu.isSoftware = true;
                        if (vkIsDiscrete) gpu.isDiscrete = true;
                        matched = true;
                        break;
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
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "  --backend <vulkan|dx12|dx11|metal|opengl>  Select rendering backend (default: auto)\n"
                      << "  --gpu <index>                       Select GPU by index\n"
                      << "  --warp                               Use WARP software renderer (DX11/DX12 only)\n"
                      << "  --vsync                              Enable vertical sync (default: off)\n"
                      << "  --host-memory                        Keep particle buffer in host-visible RAM (slower on dGPU)\n"
                      << "  --particles <count>                 Particle count (skips difficulty menu, rounded to 256)\n"
                      << "  --time <seconds>                    Auto-stop after N seconds (default: 15)\n"
                      << "  --no-time-limit                     Run until window is closed\n"
                      << "  --benchmark [frames]                Run benchmark (default: 2000 frames), then exit\n"
                      << "  --results                           List all saved benchmark results\n"
                      << "  --results-delete <id>               Delete a saved result by ID\n"
                      << "  --results-clear                     Delete all saved results\n"
                      << "  --results-export <file.csv>         Export results to CSV file\n"
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

    bool directBenchmark = (backend != "auto") || benchCfg.benchmarkMode;

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
    else if (recommendedApi == "vulkan")  recommendedApiLabel = "Vulkan 1.2";
    else if (recommendedApi == "dx12")    recommendedApiLabel = "DirectX 12";
    else if (recommendedApi == "dx11")    recommendedApiLabel = "DirectX 11";
    else if (recommendedApi == "opengl")  recommendedApiLabel = "OpenGL 4.3";

    bool hasLastRun = false;

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
                      << "  [5] Exit\n"
                      << "====================================\n"
                      << "Select (default: 0): " << std::flush;

            std::string mline;
            int mchoice = 0;
            if (std::getline(std::cin, mline) && !mline.empty())
                if (!SafeStoi(mline, mchoice)) { std::cout << "Invalid input.\n"; continue; }

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
                if (saved.empty()) {
                    std::cout << "No saved results.\n";
                    continue;
                }
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
                continue;
            } else if (mchoice == 5) {
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
                else if (available[i].id == "vulkan")  note = "Vulkan 1.2";
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

        std::int32_t effectiveGpuIndex = warp ? -2 : gpuIndex;

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

    return 0;
}
