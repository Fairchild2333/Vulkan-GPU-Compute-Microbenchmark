#pragma once

#ifdef HAVE_VULKAN

#include "app_base.h"

#ifndef GLFW_INCLUDE_VULKAN
#define GLFW_INCLUDE_VULKAN
#endif
#include <GLFW/glfw3.h>

#include <optional>

namespace gpu_bench {

struct QueueFamilyIndices {
    std::optional<std::uint32_t> graphicsFamily;
    std::optional<std::uint32_t> presentFamily;
    std::optional<std::uint32_t> computeFamily;

    bool IsComplete() const {
        return graphicsFamily.has_value() && presentFamily.has_value()
            && computeFamily.has_value();
    }
};

struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR          capabilities{};
    std::vector<VkSurfaceFormatKHR>   formats;
    std::vector<VkPresentModeKHR>     presentModes;
};

class VulkanBackend : public AppBase {
public:
    using AppBase::AppBase;

    std::string GetBackendName()    const override { return "Vulkan"; }
    std::string GetDeviceName()     const override { return deviceName_; }
    std::string GetDriverVersion()  const override { return driverVersion_; }

protected:
    void InitBackend()            override;
    void DrawFrame(float deltaTime) override;
    void CleanupBackend()         override;
    void WaitIdle()               override;

private:
    std::string deviceName_;
    std::string driverVersion_;

    VkInstance       instance_       = VK_NULL_HANDLE;
    VkSurfaceKHR    surface_        = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice         device_        = VK_NULL_HANDLE;

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

    VkCommandPool              commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;
    std::vector<VkCommandBuffer> headlessCmdBuffers_;

    std::vector<VkSemaphore> imageAvailableSemaphores_;
    std::vector<VkSemaphore> renderFinishedSemaphores_;
    std::vector<VkFence>     inFlightFences_;
    std::vector<VkFence>                        imagesInFlight_;
    std::uint32_t currentFrame_ = 0;

    static constexpr std::uint32_t kTimestampsPerFrame = 4;
    VkQueryPool timestampQueryPool_ = VK_NULL_HANDLE;
    float       timestampPeriodNs_  = 0.0f;
    bool        timestampsSupported_ = false;

    PFN_vkCmdBeginDebugUtilsLabelEXT  vkCmdBeginDebugUtilsLabel_  = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT    vkCmdEndDebugUtilsLabel_    = nullptr;
    PFN_vkSetDebugUtilsObjectNameEXT  vkSetDebugUtilsObjectName_  = nullptr;
    bool debugUtilsAvailable_ = false;

    void LoadDebugUtilsFunctions();
    void SetObjectName(VkObjectType type, std::uint64_t handle, const char* name) const;
    void BeginDebugLabel(VkCommandBuffer cmd, const char* name, float r, float g, float b) const;
    void EndDebugLabel(VkCommandBuffer cmd) const;

    static constexpr const char* kRequiredDeviceExtensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };

    void CreateInstance();
    void CreateSurface();
    QueueFamilyIndices      FindQueueFamilies(VkPhysicalDevice dev) const;
    SwapChainSupportDetails QuerySwapChainSupport(VkPhysicalDevice dev) const;
    bool CheckDeviceExtensionSupport(VkPhysicalDevice dev) const;
    bool IsDeviceSuitable(VkPhysicalDevice dev) const;
    VkShaderModule CreateShaderModule(const std::vector<char>& code) const;
    std::uint32_t  FindMemoryType(std::uint32_t typeFilter, VkMemoryPropertyFlags props) const;
    static const char* DeviceTypeName(VkPhysicalDeviceType type);
    void PickPhysicalDevice();
    void CreateLogicalDevice();
    void CreateParticleBuffer();
    void CreateGraphicsPipeline();
    void CreateComputeDescriptorSetLayout();
    void CreateComputeDescriptorResources();
    void CreateComputePipeline();
    VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>&) const;
    VkPresentModeKHR   ChooseSwapPresentMode(const std::vector<VkPresentModeKHR>&) const;
    VkExtent2D         ChooseSwapExtent(const VkSurfaceCapabilitiesKHR&) const;
    void CreateSwapChain();
    void CreateImageViews();
    void CreateRenderPass();
    void CreateFramebuffers();
    void CreateCommandPool();
    void CreateCommandBuffers();
    void RecordCommandBuffer(std::uint32_t imageIndex, float deltaTime);
    void CreateSyncObjects();
    void CreateTimestampQueryPool();
    void CollectTimestampResults(std::uint32_t frameSlot);
    void CleanupSwapChain();
};

}  // namespace gpu_bench

#endif  // HAVE_VULKAN
