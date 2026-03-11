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

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#pragma comment(lib, "dxgi.lib")
#endif

#ifdef HAVE_VULKAN
#include <vulkan/vulkan.h>
#endif

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
    std::cout << "  #   GPU";
    // Pad to 42 chars for alignment.
    std::cout << std::string(38, ' ') << "VRAM     Vulkan  DX12  DX11  Metal\n";
    std::cout << "  --- ";
    std::cout << std::string(42, '-') << " ------  ------  ----  ----  -----\n";

    for (std::uint32_t i = 0; i < gpus.size(); ++i) {
        const auto& g = gpus[i];
        std::string label = g.name;
        if (g.isSoftware)       label += " (Software)";
        else if (g.isDiscrete)  label += " (Discrete)";
        else                    label += " (Integrated)";
        if (label.size() > 42) label = label.substr(0, 39) + "...";

        std::cout << "  [" << i << "] " << label;
        if (label.size() < 42) std::cout << std::string(42 - label.size(), ' ');

        if (g.vramMB > 0)
            std::cout << " " << g.vramMB << " MB";
        else
            std::cout << "     - ";

        auto yn = [](bool v) { return v ? "  YES " : "   -  "; };
        std::cout << yn(g.supportsVulkan)
                  << yn(g.supportsDX12)
                  << yn(g.supportsDX11)
                  << yn(g.supportsMetal)
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
                      << "  --backend <vulkan|dx12|dx11|metal>  Select rendering backend (default: auto)\n"
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
#ifdef HAVE_VULKAN
        {"vulkan", [](const GpuInfo& g){ return g.supportsVulkan; }},
#endif
#ifdef HAVE_DX12
        {"dx12",   [](const GpuInfo& g){ return g.supportsDX12; }},
#endif
#ifdef HAVE_DX11
        {"dx11",   [](const GpuInfo& g){ return g.supportsDX11; }},
#endif
#ifdef HAVE_METAL
        {"metal",  [](const GpuInfo& g){ return g.supportsMetal; }},
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
    if (recGpu.supportsVulkan)      recommendedApi = "vulkan";
    else if (recGpu.supportsDX12)   recommendedApi = "dx12";
    else if (recGpu.supportsDX11)   recommendedApi = "dx11";
    else if (recGpu.supportsMetal)  recommendedApi = "metal";

    std::string recommendedApiLabel;
    if (recommendedApi == "vulkan")       recommendedApiLabel = "Vulkan 1.2";
    else if (recommendedApi == "dx12")    recommendedApiLabel = "DirectX 12";
    else if (recommendedApi == "dx11")    recommendedApiLabel = "DirectX 11";
    else if (recommendedApi == "metal")   recommendedApiLabel = "Metal";

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
                mchoice = std::stoi(mline);

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
                gpu_bench::PrintComparisonTable(saved);
                if (saved.size() >= 2) {
                    std::cout << "Enter two IDs for detailed comparison (or Enter to skip):\n"
                              << "ID 1: " << std::flush;
                    std::string s1;
                    if (std::getline(std::cin, s1) && !s1.empty()) {
                        std::cout << "ID 2: " << std::flush;
                        std::string s2;
                        if (std::getline(std::cin, s2) && !s2.empty()) {
                            const gpu_bench::BenchmarkResult* r1 = nullptr;
                            const gpu_bench::BenchmarkResult* r2 = nullptr;
                            for (const auto& r : saved) {
                                if (r.id == s1) r1 = &r;
                                if (r.id == s2) r2 = &r;
                            }
                            if (r1 && r2)
                                gpu_bench::PrintDetailedComparison(*r1, *r2);
                            else
                                std::cout << "One or both IDs not found.\n";
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
                std::cout << "Enter ID to delete (or 'all' to clear, Enter to go back): "
                          << std::flush;
                std::string did;
                if (std::getline(std::cin, did) && !did.empty()) {
                    if (did == "all") {
                        gpu_bench::ClearResults();
                        std::cout << "All results cleared.\n";
                    } else if (gpu_bench::DeleteResult(did)) {
                        std::cout << "Deleted: " << did << "\n";
                    } else {
                        std::cout << "ID not found: " << did << "\n";
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

            std::cout << "Available Graphics APIs:\n";
            for (std::uint32_t i = 0; i < available.size(); ++i) {
                std::string note;
                if (available[i].id == "vulkan")  note = "Vulkan 1.2";
                else if (available[i].id == "dx12")  note = "DirectX 12";
                else if (available[i].id == "dx11")  note = "DirectX 11";
                else if (available[i].id == "metal") note = "Metal";
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
                choice = static_cast<std::uint32_t>(std::stoi(line));
                if (choice >= available.size()) {
                    std::cerr << "Invalid index, try again.\n\n";
                    continue;
                }
            }
            selectedBackend = available[choice].id;
            std::cout << "[Graphics API] Selected: " << selectedBackend << std::endl;
        }

        if (warp && selectedBackend != "dx11" && selectedBackend != "dx12") {
            warp = false;
        }

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
                dchoice = static_cast<std::uint32_t>(std::stoi(dline));
                if (dchoice > 3) dchoice = 1;
            }
            benchCfg.particleCount = presets[dchoice].count;
            benchCfg.difficultyLabel = presets[dchoice].name;
        }

        std::int32_t effectiveGpuIndex = warp ? -2 : gpuIndex;

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
