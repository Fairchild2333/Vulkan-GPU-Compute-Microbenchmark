#include <ace/xcomponent/native_interface_xcomponent.h>
#include <hilog/log.h>
#include <napi/native_api.h>

#include "vulkan_renderer.h"

#undef LOG_TAG
#define LOG_TAG "GpuBench"
#define LOG_DOMAIN 0x0000

static gpu_bench::VulkanRenderer* g_renderer = nullptr;

static void OnSurfaceCreatedCB(OH_NativeXComponent* component, void* window) {
    OH_LOG_INFO(LOG_APP, "OnSurfaceCreated");

    auto* nativeWindow = static_cast<OHNativeWindow*>(window);

    uint64_t width = 0, height = 0;
    int32_t ret = OH_NativeXComponent_GetXComponentSize(component, nativeWindow, &width, &height);
    if (ret != 0) {
        OH_LOG_ERROR(LOG_APP, "GetXComponentSize failed: %{public}d", ret);
        width = 1280;
        height = 720;
    }

    OH_LOG_INFO(LOG_APP, "Surface size: %{public}lu x %{public}lu",
                static_cast<unsigned long>(width),
                static_cast<unsigned long>(height));

    if (g_renderer) {
        delete g_renderer;
    }
    g_renderer = new gpu_bench::VulkanRenderer();
    g_renderer->Init(nativeWindow, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    g_renderer->StartRenderLoop();
}

static void OnSurfaceChangedCB(OH_NativeXComponent* component, void* window) {
    OH_LOG_INFO(LOG_APP, "OnSurfaceChanged");

    if (!g_renderer) return;

    uint64_t width = 0, height = 0;
    OH_NativeXComponent_GetXComponentSize(component, window, &width, &height);
    OH_LOG_INFO(LOG_APP, "New size: %{public}lu x %{public}lu",
                static_cast<unsigned long>(width),
                static_cast<unsigned long>(height));
}

static void OnSurfaceDestroyedCB(OH_NativeXComponent* component, void* window) {
    OH_LOG_INFO(LOG_APP, "OnSurfaceDestroyed");

    if (g_renderer) {
        g_renderer->StopRenderLoop();
        delete g_renderer;
        g_renderer = nullptr;
    }
}

static napi_value Init(napi_env env, napi_value exports) {
    OH_LOG_INFO(LOG_APP, "NAPI Init");

    napi_value exportInstance = nullptr;
    OH_NativeXComponent* nativeXComponent = nullptr;

    napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &exportInstance);
    if (!exportInstance) {
        OH_LOG_ERROR(LOG_APP, "Failed to get XComponent object");
        return exports;
    }

    napi_unwrap(env, exportInstance, reinterpret_cast<void**>(&nativeXComponent));
    if (!nativeXComponent) {
        OH_LOG_ERROR(LOG_APP, "Failed to unwrap NativeXComponent");
        return exports;
    }

    OH_NativeXComponent_Callback callback;
    callback.OnSurfaceCreated = OnSurfaceCreatedCB;
    callback.OnSurfaceChanged = OnSurfaceChangedCB;
    callback.OnSurfaceDestroyed = OnSurfaceDestroyedCB;
    callback.DispatchTouchEvent = nullptr;

    OH_NativeXComponent_RegisterCallback(nativeXComponent, &callback);
    OH_LOG_INFO(LOG_APP, "XComponent callbacks registered");

    return exports;
}

EXTERN_C_START
static napi_value NapiExport(napi_env env, napi_value exports) {
    return Init(env, exports);
}
EXTERN_C_END

static napi_module g_module = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = NapiExport,
    .nm_modname = "gpubench",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterModule() {
    napi_module_register(&g_module);
}
