#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#define VK_USE_PLATFORM_OHOS 1
#include <vulkan/vulkan.h>
#include <native_window/external_window.h>

namespace gpu_bench {

constexpr uint32_t kWindowWidth  = 1280;
constexpr uint32_t kWindowHeight = 720;
constexpr uint32_t kMaxFramesInFlight    = 2;
constexpr uint32_t kParticleCount        = 65536;
constexpr uint32_t kComputeWorkGroupSize = 256;

struct Particle {
    float px, py, pz, pw;
    float vx, vy, vz, vw;
};

struct ComputeParams {
    float deltaTime;
    float bounds;
};

struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;
    std::optional<uint32_t> computeFamily;
    bool IsComplete() const {
        return graphicsFamily.has_value() && presentFamily.has_value()
            && computeFamily.has_value();
    }
};

class VulkanRenderer {
public:
    VulkanRenderer() = default;
    ~VulkanRenderer();

    void Init(OHNativeWindow* window, uint32_t width, uint32_t height);
    void StartRenderLoop();
    void StopRenderLoop();

private:
    void GenerateParticles();
    void CreateInstance();
    void CreateSurface();
    void PickPhysicalDevice();
    void CreateLogicalDevice();
    void CreateSwapChain();
    void CreateImageViews();
    void CreateRenderPass();
    void CreateGraphicsPipeline();
    void CreateComputeDescriptorSetLayout();
    void CreateParticleBuffer();
    void CreateComputeDescriptorResources();
    void CreateComputePipeline();
    void CreateFramebuffers();
    void CreateCommandPool();
    void CreateCommandBuffers();
    void CreateSyncObjects();
    void CreateTimestampQueryPool();
    void DrawFrame(float deltaTime);
    void RecordCommandBuffer(uint32_t imageIndex, float deltaTime);
    void CollectTimestampResults(uint32_t slot);
    void Cleanup();

    QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice dev) const;
    bool               IsDeviceSuitable(VkPhysicalDevice dev) const;
    bool               CheckDeviceExtensionSupport(VkPhysicalDevice dev) const;
    VkShaderModule     CreateShaderModule(const std::vector<char>& code) const;
    uint32_t           FindMemoryType(uint32_t filter, VkMemoryPropertyFlags props) const;
    std::vector<char>  ReadRawFile(const std::string& filename) const;

    OHNativeWindow* nativeWindow_ = nullptr;
    uint32_t        surfaceWidth_  = 0;
    uint32_t        surfaceHeight_ = 0;

    std::atomic<bool> running_{false};
    std::thread       renderThread_;

    std::vector<Particle> particles_;

    VkInstance       instance_       = VK_NULL_HANDLE;
    VkSurfaceKHR     surface_        = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice         device_         = VK_NULL_HANDLE;

    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_  = VK_NULL_HANDLE;
    VkQueue computeQueue_  = VK_NULL_HANDLE;

    VkSwapchainKHR              swapChain_          = VK_NULL_HANDLE;
    std::vector<VkImage>        swapChainImages_;
    std::vector<VkImageView>    swapChainImageViews_;
    std::vector<VkFramebuffer>  swapChainFramebuffers_;
    VkFormat                    swapChainImageFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D                  swapChainExtent_{};

    VkRenderPass     renderPass_             = VK_NULL_HANDLE;
    VkPipelineLayout graphicsPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline       graphicsPipeline_       = VK_NULL_HANDLE;

    VkDescriptorSetLayout computeDescriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      descriptorPool_              = VK_NULL_HANDLE;
    VkDescriptorSet       computeDescriptorSet_        = VK_NULL_HANDLE;
    VkPipelineLayout      computePipelineLayout_       = VK_NULL_HANDLE;
    VkPipeline            computePipeline_             = VK_NULL_HANDLE;

    VkBuffer       particleBuffer_       = VK_NULL_HANDLE;
    VkDeviceMemory particleBufferMemory_ = VK_NULL_HANDLE;

    VkCommandPool                commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;

    std::array<VkSemaphore, kMaxFramesInFlight> imageAvailableSemaphores_{};
    std::array<VkSemaphore, kMaxFramesInFlight> renderFinishedSemaphores_{};
    std::array<VkFence,     kMaxFramesInFlight> inFlightFences_{};
    uint32_t currentFrame_ = 0;

    static constexpr uint32_t kTimestampsPerFrame = 4;
    VkQueryPool timestampQueryPool_ = VK_NULL_HANDLE;
    float       timestampPeriodNs_  = 0.0f;
    bool        timestampsSupported_ = false;

    double accumComputeMs_  = 0.0;
    double accumRenderMs_   = 0.0;
    double accumTotalGpuMs_ = 0.0;
    uint32_t timingSampleCount_ = 0;

    std::string shaderDir_;
};

}  // namespace gpu_bench
