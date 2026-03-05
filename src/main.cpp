// Multi-backend GPU Compute & Rendering Pipeline
//
// Supports Vulkan, DX12, and DX11 backends selectable via --backend flag.
// Each backend implements the same particle simulation (compute shader)
// and point-cloud rendering (graphics pipeline) with GPU timestamp profiling.

#include "app_base.h"

#ifdef HAVE_VULKAN
#include "vulkan_backend.h"
#endif
#ifdef HAVE_DX12
#include "dx12_backend.h"
#endif
#ifdef HAVE_DX11
#include "dx11_backend.h"
#endif

#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#ifdef _WIN32
#include <windows.h>
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

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(CrashHandler);
#endif

    std::string backend = "auto";
    std::int32_t gpuIndex = -1;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            backend = argv[++i];
        } else if (std::strcmp(argv[i], "--gpu") == 0 && i + 1 < argc) {
            gpuIndex = std::stoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "  --backend <vulkan|dx12|dx11>  Select rendering backend (default: auto)\n"
                      << "  --gpu <index>                 Select GPU by index\n"
                      << "  --help                        Show this help\n\n"
                      << "Available backends:";
#ifdef HAVE_VULKAN
            std::cout << " vulkan";
#endif
#ifdef HAVE_DX12
            std::cout << " dx12";
#endif
#ifdef HAVE_DX11
            std::cout << " dx11";
#endif
            std::cout << '\n';
            return 0;
        }
    }

    const std::string shaderDir = ExeDirectory(argv[0]);

    if (backend == "auto") {
#ifdef HAVE_DX11
        backend = "dx11";
#elif defined(HAVE_DX12)
        backend = "dx12";
#elif defined(HAVE_VULKAN)
        backend = "vulkan";
#else
        std::cerr << "No backend compiled in.\n";
        return 1;
#endif
    }

    try {
        std::unique_ptr<gpu_bench::AppBase> app;

#ifdef HAVE_VULKAN
        if (backend == "vulkan") {
            app = std::make_unique<gpu_bench::VulkanBackend>(gpuIndex, shaderDir);
        }
#endif
#ifdef HAVE_DX12
        if (backend == "dx12") {
            app = std::make_unique<gpu_bench::DX12Backend>(gpuIndex, shaderDir);
        }
#endif
#ifdef HAVE_DX11
        if (backend == "dx11") {
            app = std::make_unique<gpu_bench::DX11Backend>(gpuIndex, shaderDir);
        }
#endif

        if (!app) {
            std::cerr << "Unknown or unavailable backend: " << backend << '\n';
            return 1;
        }

        std::cout << "Backend: " << app->GetBackendName() << '\n';
        app->Run();
    } catch (const std::exception& ex) {
        std::cout << "Fatal error: " << ex.what() << std::endl;
        return 1;
    } catch (...) {
        std::cout << "Fatal error: unknown exception" << std::endl;
        return 1;
    }

    return 0;
}
