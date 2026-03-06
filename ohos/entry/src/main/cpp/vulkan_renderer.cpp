#include "vulkan_renderer.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>

#include <hilog/log.h>
#include <rawfile/raw_file_manager.h>

#undef LOG_TAG
#define LOG_TAG "GpuBench"
#define LOG_DOMAIN 0x0000

#define VK_CHECK(expr, msg)                                   \
    do {                                                      \
        if ((expr) != VK_SUCCESS) {                           \
            OH_LOG_ERROR(LOG_APP, "%{public}s", msg);         \
            throw std::runtime_error(msg);                    \
        }                                                     \
    } while (0)

namespace gpu_bench {

static const char* kRequiredDeviceExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

VulkanRenderer::~VulkanRenderer() {
    StopRenderLoop();
    Cleanup();
}

// ---------------------------------------------------------------------------
// Init / lifecycle
// ---------------------------------------------------------------------------

void VulkanRenderer::Init(OHNativeWindow* window, uint32_t width, uint32_t height) {
    nativeWindow_ = window;
    surfaceWidth_  = width;
    surfaceHeight_ = height;

    GenerateParticles();
    CreateInstance();
    CreateSurface();
    PickPhysicalDevice();
    CreateLogicalDevice();
    CreateSwapChain();
    CreateImageViews();
    CreateRenderPass();
    CreateGraphicsPipeline();
    CreateComputeDescriptorSetLayout();
    CreateParticleBuffer();
    CreateComputeDescriptorResources();
    CreateComputePipeline();
    CreateFramebuffers();
    CreateCommandPool();
    CreateCommandBuffers();
    CreateSyncObjects();
    CreateTimestampQueryPool();

    OH_LOG_INFO(LOG_APP, "Vulkan initialisation complete");
}

void VulkanRenderer::StartRenderLoop() {
    if (running_.load()) return;
    running_.store(true);
    renderThread_ = std::thread([this]() {
        OH_LOG_INFO(LOG_APP, "Render thread started");
        auto lastTime = std::chrono::high_resolution_clock::now();
        double timingTimer = 0.0;
        uint32_t frameCount = 0;

        while (running_.load()) {
            auto now = std::chrono::high_resolution_clock::now();
            double dt = std::chrono::duration<double>(now - lastTime).count();
            lastTime = now;

            try {
                DrawFrame(static_cast<float>(dt));
            } catch (const std::exception& e) {
                OH_LOG_ERROR(LOG_APP, "DrawFrame error: %{public}s", e.what());
                break;
            }

            ++frameCount;
            timingTimer += dt;
            if (timingTimer >= 1.0) {
                double fps = frameCount / timingTimer;
                if (timingSampleCount_ > 0) {
                    double avgCompute = accumComputeMs_ / timingSampleCount_;
                    double avgRender  = accumRenderMs_  / timingSampleCount_;
                    double avgTotal   = accumTotalGpuMs_ / timingSampleCount_;
                    OH_LOG_INFO(LOG_APP,
                        "Compute: %.3f ms | Render: %.3f ms | Total: %.3f ms | FPS: %d",
                        avgCompute, avgRender, avgTotal, static_cast<int>(fps));
                } else {
                    OH_LOG_INFO(LOG_APP, "FPS: %d", static_cast<int>(fps));
                }
                accumComputeMs_ = accumRenderMs_ = accumTotalGpuMs_ = 0.0;
                timingSampleCount_ = 0;
                frameCount = 0;
                timingTimer = 0.0;
            }
        }

        if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
        OH_LOG_INFO(LOG_APP, "Render thread exiting");
    });
}

void VulkanRenderer::StopRenderLoop() {
    running_.store(false);
    if (renderThread_.joinable()) renderThread_.join();
}

void VulkanRenderer::GenerateParticles() {
    particles_.resize(kParticleCount);
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> posDist(-0.8f, 0.8f);
    std::uniform_real_distribution<float> velDist(-0.2f, 0.2f);
    for (uint32_t i = 0; i < kParticleCount; ++i) {
        particles_[i] = {
            posDist(rng), posDist(rng), 0.0f, 1.0f,
            velDist(rng), velDist(rng), 0.0f, 0.0f,
        };
    }
}

// ---------------------------------------------------------------------------
// File I/O — read SPIR-V from the application's rawfile directory
// ---------------------------------------------------------------------------

std::vector<char> VulkanRenderer::ReadRawFile(const std::string& filename) const {
    std::string path = shaderDir_ + filename;
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        OH_LOG_ERROR(LOG_APP, "Failed to open shader: %{public}s", path.c_str());
        throw std::runtime_error("Failed to open shader: " + path);
    }
    auto size = static_cast<size_t>(file.tellg());
    std::vector<char> buf(size);
    file.seekg(0);
    file.read(buf.data(), static_cast<std::streamsize>(size));
    return buf;
}

// ---------------------------------------------------------------------------
// Instance & Surface (OHOS-specific)
// ---------------------------------------------------------------------------

void VulkanRenderer::CreateInstance() {
    VkApplicationInfo appInfo{};
    appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName   = "GpuComputeBenchmark";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 3, 0);
    appInfo.pEngineName        = "NoEngine";
    appInfo.engineVersion      = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion         = VK_API_VERSION_1_2;

    std::vector<const char*> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_OHOS_SURFACE_EXTENSION_NAME,
    };

    VkInstanceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo        = &appInfo;
    ci.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();

    VK_CHECK(vkCreateInstance(&ci, nullptr, &instance_), "vkCreateInstance failed");
    OH_LOG_INFO(LOG_APP, "Vulkan instance created");
}

void VulkanRenderer::CreateSurface() {
    VkSurfaceCreateInfoOHOS surfaceCI{};
    surfaceCI.sType  = VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS;
    surfaceCI.window = nativeWindow_;

    VK_CHECK(vkCreateSurfaceOHOS(instance_, &surfaceCI, nullptr, &surface_),
             "vkCreateSurfaceOHOS failed");
    OH_LOG_INFO(LOG_APP, "Vulkan surface created from OHNativeWindow");
}

// ---------------------------------------------------------------------------
// Physical / Logical device (identical to VulkanBackend)
// ---------------------------------------------------------------------------

QueueFamilyIndices VulkanRenderer::FindQueueFamilies(VkPhysicalDevice device) const {
    QueueFamilyIndices indices;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

    for (uint32_t i = 0; i < count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) indices.graphicsFamily = i;
        if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)  indices.computeFamily  = i;
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &present);
        if (present) indices.presentFamily = i;
        if (indices.IsComplete()) break;
    }
    return indices;
}

bool VulkanRenderer::CheckDeviceExtensionSupport(VkPhysicalDevice device) const {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data());
    std::set<std::string> required(std::begin(kRequiredDeviceExtensions),
                                   std::end(kRequiredDeviceExtensions));
    for (auto& ext : available) required.erase(ext.extensionName);
    return required.empty();
}

bool VulkanRenderer::IsDeviceSuitable(VkPhysicalDevice device) const {
    auto idx = FindQueueFamilies(device);
    if (!idx.IsComplete() || !CheckDeviceExtensionSupport(device)) return false;

    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface_, &caps);
    uint32_t fmtCount = 0, pmCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &fmtCount, nullptr);
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &pmCount, nullptr);
    return fmtCount > 0 && pmCount > 0;
}

void VulkanRenderer::PickPhysicalDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) throw std::runtime_error("No Vulkan physical device");

    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    for (auto& dev : devices) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(dev, &p);
        OH_LOG_INFO(LOG_APP, "GPU: %{public}s (type %d)", p.deviceName, p.deviceType);

        if (IsDeviceSuitable(dev)) {
            physicalDevice_ = dev;
            OH_LOG_INFO(LOG_APP, "Selected GPU: %{public}s", p.deviceName);
            return;
        }
    }
    throw std::runtime_error("No suitable Vulkan device");
}

void VulkanRenderer::CreateLogicalDevice() {
    auto indices = FindQueueFamilies(physicalDevice_);
    std::set<uint32_t> unique = {
        indices.graphicsFamily.value(),
        indices.presentFamily.value(),
        indices.computeFamily.value()
    };

    std::vector<VkDeviceQueueCreateInfo> queueCIs;
    float prio = 1.0f;
    for (auto fam : unique) {
        VkDeviceQueueCreateInfo qi{};
        qi.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = fam;
        qi.queueCount       = 1;
        qi.pQueuePriorities = &prio;
        queueCIs.push_back(qi);
    }

    VkPhysicalDeviceFeatures features{};

    VkDeviceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount    = static_cast<uint32_t>(queueCIs.size());
    ci.pQueueCreateInfos       = queueCIs.data();
    ci.pEnabledFeatures        = &features;
    ci.enabledExtensionCount   = static_cast<uint32_t>(std::size(kRequiredDeviceExtensions));
    ci.ppEnabledExtensionNames = kRequiredDeviceExtensions;

    VK_CHECK(vkCreateDevice(physicalDevice_, &ci, nullptr, &device_),
             "vkCreateDevice failed");

    vkGetDeviceQueue(device_, indices.graphicsFamily.value(), 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, indices.presentFamily.value(),  0, &presentQueue_);
    vkGetDeviceQueue(device_, indices.computeFamily.value(),  0, &computeQueue_);
}

// ---------------------------------------------------------------------------
// Shader helpers
// ---------------------------------------------------------------------------

VkShaderModule VulkanRenderer::CreateShaderModule(const std::vector<char>& code) const {
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device_, &ci, nullptr, &m),
             "vkCreateShaderModule failed");
    return m;
}

uint32_t VulkanRenderer::FindMemoryType(uint32_t filter, VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
        if ((filter & (1u << i)) && (mem.memoryTypes[i].propertyFlags & props) == props)
            return i;
    throw std::runtime_error("Failed to find suitable memory type");
}

// ---------------------------------------------------------------------------
// Particle buffer
// ---------------------------------------------------------------------------

void VulkanRenderer::CreateParticleBuffer() {
    VkDeviceSize size = sizeof(Particle) * kParticleCount;

    VkBufferCreateInfo bi{};
    bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size        = size;
    bi.usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(device_, &bi, nullptr, &particleBuffer_),
             "vkCreateBuffer failed");

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, particleBuffer_, &req);

    VkMemoryAllocateInfo ai{};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &particleBufferMemory_),
             "vkAllocateMemory failed");

    vkBindBufferMemory(device_, particleBuffer_, particleBufferMemory_, 0);

    void* data = nullptr;
    vkMapMemory(device_, particleBufferMemory_, 0, size, 0, &data);
    std::memcpy(data, particles_.data(), static_cast<size_t>(size));
    vkUnmapMemory(device_, particleBufferMemory_);

    OH_LOG_INFO(LOG_APP, "Particle buffer created: %u particles", kParticleCount);
}

// ---------------------------------------------------------------------------
// Swap chain
// ---------------------------------------------------------------------------

void VulkanRenderer::CreateSwapChain() {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &caps);

    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &fmtCount, formats.data());

    uint32_t pmCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &pmCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(pmCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &pmCount, presentModes.data());

    VkSurfaceFormatKHR fmt = formats[0];
    for (auto& f : formats)
        if (f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_SRGB) {
            fmt = f; break;
        }

    VkPresentModeKHR pm = VK_PRESENT_MODE_FIFO_KHR;
    for (auto m : presentModes)
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) { pm = m; break; }

    VkExtent2D ext;
    if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        ext = caps.currentExtent;
    } else {
        ext = { surfaceWidth_, surfaceHeight_ };
        ext.width  = std::clamp(ext.width,  caps.minImageExtent.width,  caps.maxImageExtent.width);
        ext.height = std::clamp(ext.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t imgCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imgCount > caps.maxImageCount)
        imgCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci{};
    ci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface          = surface_;
    ci.minImageCount    = imgCount;
    ci.imageFormat      = fmt.format;
    ci.imageColorSpace  = fmt.colorSpace;
    ci.imageExtent      = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    auto idx = FindQueueFamilies(physicalDevice_);
    uint32_t qfi[] = { idx.graphicsFamily.value(), idx.presentFamily.value() };
    if (idx.graphicsFamily != idx.presentFamily) {
        ci.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = 2;
        ci.pQueueFamilyIndices   = qfi;
    } else {
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    ci.preTransform   = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode    = pm;
    ci.clipped        = VK_TRUE;

    VK_CHECK(vkCreateSwapchainKHR(device_, &ci, nullptr, &swapChain_),
             "vkCreateSwapchainKHR failed");

    vkGetSwapchainImagesKHR(device_, swapChain_, &imgCount, nullptr);
    swapChainImages_.resize(imgCount);
    vkGetSwapchainImagesKHR(device_, swapChain_, &imgCount, swapChainImages_.data());
    swapChainImageFormat_ = fmt.format;
    swapChainExtent_      = ext;

    OH_LOG_INFO(LOG_APP, "Swap chain: %ux%u, %u images",
                ext.width, ext.height, imgCount);
}

void VulkanRenderer::CreateImageViews() {
    swapChainImageViews_.resize(swapChainImages_.size());
    for (size_t i = 0; i < swapChainImages_.size(); ++i) {
        VkImageViewCreateInfo ci{};
        ci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ci.image    = swapChainImages_[i];
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ci.format   = swapChainImageFormat_;
        ci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VK_CHECK(vkCreateImageView(device_, &ci, nullptr, &swapChainImageViews_[i]),
                 "vkCreateImageView failed");
    }
}

void VulkanRenderer::CreateRenderPass() {
    VkAttachmentDescription color{};
    color.format         = swapChainImageFormat_;
    color.samples        = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &ref;

    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo ci{};
    ci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = 1; ci.pAttachments  = &color;
    ci.subpassCount    = 1; ci.pSubpasses    = &subpass;
    ci.dependencyCount = 1; ci.pDependencies = &dep;

    VK_CHECK(vkCreateRenderPass(device_, &ci, nullptr, &renderPass_),
             "vkCreateRenderPass failed");
}

void VulkanRenderer::CreateFramebuffers() {
    swapChainFramebuffers_.resize(swapChainImageViews_.size());
    for (size_t i = 0; i < swapChainImageViews_.size(); ++i) {
        VkImageView att[] = { swapChainImageViews_[i] };
        VkFramebufferCreateInfo ci{};
        ci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        ci.renderPass      = renderPass_;
        ci.attachmentCount = 1;
        ci.pAttachments    = att;
        ci.width           = swapChainExtent_.width;
        ci.height          = swapChainExtent_.height;
        ci.layers          = 1;
        VK_CHECK(vkCreateFramebuffer(device_, &ci, nullptr, &swapChainFramebuffers_[i]),
                 "vkCreateFramebuffer failed");
    }
}

// ---------------------------------------------------------------------------
// Pipelines (identical logic to VulkanBackend)
// ---------------------------------------------------------------------------

void VulkanRenderer::CreateGraphicsPipeline() {
    auto vertCode = ReadRawFile("particle.vert.spv");
    auto fragCode = ReadRawFile("particle.frag.spv");
    VkShaderModule vertMod = CreateShaderModule(vertCode);
    VkShaderModule fragMod = CreateShaderModule(fragCode);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragMod;
    stages[1].pName  = "main";

    VkVertexInputBindingDescription bind{ 0, sizeof(Particle), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0] = { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Particle, px) };
    attrs[1] = { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Particle, vx) };

    VkPipelineVertexInputStateCreateInfo vertIn{};
    vertIn.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertIn.vertexBindingDescriptionCount   = 1; vertIn.pVertexBindingDescriptions   = &bind;
    vertIn.vertexAttributeDescriptionCount = 2; vertIn.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

    VkViewport vp{ 0, 0, static_cast<float>(swapChainExtent_.width),
                   static_cast<float>(swapChainExtent_.height), 0, 1 };
    VkRect2D sc{ {0,0}, swapChainExtent_ };
    VkPipelineViewportStateCreateInfo vs{};
    vs.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vs.viewportCount = 1; vs.pViewports = &vp;
    vs.scissorCount  = 1; vs.pScissors  = &sc;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth   = 1.0f;
    rs.cullMode    = VK_CULL_MODE_NONE;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                       | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable         = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp        = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba.alphaBlendOp        = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &cba;

    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VK_CHECK(vkCreatePipelineLayout(device_, &pli, nullptr, &graphicsPipelineLayout_),
             "vkCreatePipelineLayout failed");

    VkGraphicsPipelineCreateInfo pi{};
    pi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.stageCount          = 2;
    pi.pStages             = stages;
    pi.pVertexInputState   = &vertIn;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState      = &vs;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState   = &ms;
    pi.pColorBlendState    = &cb;
    pi.layout              = graphicsPipelineLayout_;
    pi.renderPass          = renderPass_;
    pi.subpass             = 0;

    VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pi, nullptr, &graphicsPipeline_),
             "vkCreateGraphicsPipelines failed");

    vkDestroyShaderModule(device_, fragMod, nullptr);
    vkDestroyShaderModule(device_, vertMod, nullptr);
}

void VulkanRenderer::CreateComputeDescriptorSetLayout() {
    VkDescriptorSetLayoutBinding ssbo{};
    ssbo.binding         = 0;
    ssbo.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ssbo.descriptorCount = 1;
    ssbo.stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = 1;
    ci.pBindings    = &ssbo;

    VK_CHECK(vkCreateDescriptorSetLayout(device_, &ci, nullptr, &computeDescriptorSetLayout_),
             "vkCreateDescriptorSetLayout failed");
}

void VulkanRenderer::CreateComputeDescriptorResources() {
    VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo pi{};
    pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.poolSizeCount = 1;  pi.pPoolSizes = &ps;
    pi.maxSets       = 1;
    VK_CHECK(vkCreateDescriptorPool(device_, &pi, nullptr, &descriptorPool_),
             "vkCreateDescriptorPool failed");

    VkDescriptorSetAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool     = descriptorPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &computeDescriptorSetLayout_;
    VK_CHECK(vkAllocateDescriptorSets(device_, &ai, &computeDescriptorSet_),
             "vkAllocateDescriptorSets failed");

    VkDescriptorBufferInfo bi{ particleBuffer_, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet wr{};
    wr.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet          = computeDescriptorSet_;
    wr.dstBinding      = 0;
    wr.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wr.descriptorCount = 1;
    wr.pBufferInfo     = &bi;
    vkUpdateDescriptorSets(device_, 1, &wr, 0, nullptr);
}

void VulkanRenderer::CreateComputePipeline() {
    auto code = ReadRawFile("compute.comp.spv");
    VkShaderModule mod = CreateShaderModule(code);

    VkPipelineShaderStageCreateInfo si{};
    si.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    si.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    si.module = mod;
    si.pName  = "main";

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.size       = sizeof(ComputeParams);

    VkPipelineLayoutCreateInfo li{};
    li.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.setLayoutCount         = 1; li.pSetLayouts          = &computeDescriptorSetLayout_;
    li.pushConstantRangeCount = 1; li.pPushConstantRanges  = &pcr;
    VK_CHECK(vkCreatePipelineLayout(device_, &li, nullptr, &computePipelineLayout_),
             "vkCreatePipelineLayout (compute) failed");

    VkComputePipelineCreateInfo ci{};
    ci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage  = si;
    ci.layout = computePipelineLayout_;
    VK_CHECK(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &computePipeline_),
             "vkCreateComputePipelines failed");

    vkDestroyShaderModule(device_, mod, nullptr);
}

// ---------------------------------------------------------------------------
// Commands & sync
// ---------------------------------------------------------------------------

void VulkanRenderer::CreateCommandPool() {
    auto idx = FindQueueFamilies(physicalDevice_);
    VkCommandPoolCreateInfo ci{};
    ci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = idx.graphicsFamily.value();
    VK_CHECK(vkCreateCommandPool(device_, &ci, nullptr, &commandPool_),
             "vkCreateCommandPool failed");
}

void VulkanRenderer::CreateCommandBuffers() {
    commandBuffers_.resize(swapChainFramebuffers_.size());
    VkCommandBufferAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool        = commandPool_;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = static_cast<uint32_t>(commandBuffers_.size());
    VK_CHECK(vkAllocateCommandBuffers(device_, &ai, commandBuffers_.data()),
             "vkAllocateCommandBuffers failed");
}

void VulkanRenderer::CreateSyncObjects() {
    VkSemaphoreCreateInfo si{}; si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        VK_CHECK(vkCreateSemaphore(device_, &si, nullptr, &imageAvailableSemaphores_[i]),
                 "vkCreateSemaphore failed");
        VK_CHECK(vkCreateSemaphore(device_, &si, nullptr, &renderFinishedSemaphores_[i]),
                 "vkCreateSemaphore failed");
        VK_CHECK(vkCreateFence(device_, &fi, nullptr, &inFlightFences_[i]),
                 "vkCreateFence failed");
    }
}

void VulkanRenderer::CreateTimestampQueryPool() {
    auto idx = FindQueueFamilies(physicalDevice_);
    uint32_t cnt = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &cnt, nullptr);
    std::vector<VkQueueFamilyProperties> fams(cnt);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &cnt, fams.data());

    if (fams[idx.graphicsFamily.value()].timestampValidBits == 0) {
        OH_LOG_WARN(LOG_APP, "Timestamps not supported — disabled");
        return;
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice_, &props);
    timestampPeriodNs_ = props.limits.timestampPeriod;

    VkQueryPoolCreateInfo ci{};
    ci.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    ci.queryType  = VK_QUERY_TYPE_TIMESTAMP;
    ci.queryCount = kTimestampsPerFrame * kMaxFramesInFlight;
    if (vkCreateQueryPool(device_, &ci, nullptr, &timestampQueryPool_) != VK_SUCCESS) {
        OH_LOG_WARN(LOG_APP, "Failed to create query pool — disabled");
        return;
    }

    timestampsSupported_ = true;
    OH_LOG_INFO(LOG_APP, "Timestamp queries enabled (period = %.1f ns/tick)",
                timestampPeriodNs_);
}

void VulkanRenderer::CollectTimestampResults(uint32_t slot) {
    if (!timestampsSupported_) return;

    uint32_t first = slot * kTimestampsPerFrame;
    std::array<uint64_t, 4> ts{};
    if (vkGetQueryPoolResults(device_, timestampQueryPool_, first, 4,
            sizeof(ts), ts.data(), sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;

    double toMs = static_cast<double>(timestampPeriodNs_) / 1'000'000.0;
    accumComputeMs_  += static_cast<double>(ts[1] - ts[0]) * toMs;
    accumRenderMs_   += static_cast<double>(ts[3] - ts[2]) * toMs;
    accumTotalGpuMs_ += static_cast<double>(ts[3] - ts[0]) * toMs;
    ++timingSampleCount_;
}

// ---------------------------------------------------------------------------
// Per-frame recording & draw
// ---------------------------------------------------------------------------

void VulkanRenderer::RecordCommandBuffer(uint32_t imageIndex, float deltaTime) {
    VkCommandBuffer cmd = commandBuffers_[imageIndex];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer failed");

    uint32_t tsBase = currentFrame_ * kTimestampsPerFrame;
    if (timestampsSupported_)
        vkCmdResetQueryPool(cmd, timestampQueryPool_, tsBase, kTimestampsPerFrame);

    if (timestampsSupported_)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestampQueryPool_, tsBase);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            computePipelineLayout_, 0, 1, &computeDescriptorSet_, 0, nullptr);
    ComputeParams params{ deltaTime, 0.9f };
    vkCmdPushConstants(cmd, computePipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(ComputeParams), &params);
    vkCmdDispatch(cmd, kParticleCount / kComputeWorkGroupSize, 1, 1);

    if (timestampsSupported_)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timestampQueryPool_, tsBase + 1);

    VkBufferMemoryBarrier barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask       = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer              = particleBuffer_;
    barrier.size                = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
        0, 0, nullptr, 1, &barrier, 0, nullptr);

    if (timestampsSupported_)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestampQueryPool_, tsBase + 2);

    VkClearValue clear = {{{0.04f, 0.08f, 0.14f, 1.0f}}};
    VkRenderPassBeginInfo rp{};
    rp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass        = renderPass_;
    rp.framebuffer       = swapChainFramebuffers_[imageIndex];
    rp.renderArea.extent = swapChainExtent_;
    rp.clearValueCount   = 1;
    rp.pClearValues      = &clear;

    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline_);
    VkBuffer     bufs[] = { particleBuffer_ };
    VkDeviceSize offs[] = { 0 };
    vkCmdBindVertexBuffers(cmd, 0, 1, bufs, offs);
    vkCmdDraw(cmd, kParticleCount, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    if (timestampsSupported_)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, timestampQueryPool_, tsBase + 3);

    VK_CHECK(vkEndCommandBuffer(cmd), "vkEndCommandBuffer failed");
}

void VulkanRenderer::DrawFrame(float deltaTime) {
    vkWaitForFences(device_, 1, &inFlightFences_[currentFrame_], VK_TRUE, UINT64_MAX);
    CollectTimestampResults(currentFrame_);

    uint32_t imageIndex = 0;
    VkResult res = vkAcquireNextImageKHR(device_, swapChain_, UINT64_MAX,
                                         imageAvailableSemaphores_[currentFrame_],
                                         VK_NULL_HANDLE, &imageIndex);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) return;
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
        throw std::runtime_error("vkAcquireNextImageKHR failed");

    vkResetFences(device_, 1, &inFlightFences_[currentFrame_]);
    RecordCommandBuffer(imageIndex, deltaTime);

    VkSemaphore          waitSem[]   = { imageAvailableSemaphores_[currentFrame_] };
    VkPipelineStageFlags waitStage[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    VkSemaphore          sigSem[]    = { renderFinishedSemaphores_[currentFrame_] };

    VkSubmitInfo si{};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount   = 1; si.pWaitSemaphores   = waitSem;
    si.pWaitDstStageMask    = waitStage;
    si.commandBufferCount   = 1; si.pCommandBuffers   = &commandBuffers_[imageIndex];
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = sigSem;

    VK_CHECK(vkQueueSubmit(graphicsQueue_, 1, &si, inFlightFences_[currentFrame_]),
             "vkQueueSubmit failed");

    VkSwapchainKHR chains[] = { swapChain_ };
    VkPresentInfoKHR pi{};
    pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = sigSem;
    pi.swapchainCount     = 1; pi.pSwapchains     = chains;
    pi.pImageIndices      = &imageIndex;
    vkQueuePresentKHR(presentQueue_, &pi);

    currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
}

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------

void VulkanRenderer::Cleanup() {
    if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);

    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (imageAvailableSemaphores_[i]) vkDestroySemaphore(device_, imageAvailableSemaphores_[i], nullptr);
        if (renderFinishedSemaphores_[i]) vkDestroySemaphore(device_, renderFinishedSemaphores_[i], nullptr);
        if (inFlightFences_[i])           vkDestroyFence(device_, inFlightFences_[i], nullptr);
    }
    if (timestampQueryPool_)         vkDestroyQueryPool(device_, timestampQueryPool_, nullptr);
    if (commandPool_)                vkDestroyCommandPool(device_, commandPool_, nullptr);
    if (particleBuffer_)             vkDestroyBuffer(device_, particleBuffer_, nullptr);
    if (particleBufferMemory_)       vkFreeMemory(device_, particleBufferMemory_, nullptr);
    if (computePipeline_)            vkDestroyPipeline(device_, computePipeline_, nullptr);
    if (computePipelineLayout_)      vkDestroyPipelineLayout(device_, computePipelineLayout_, nullptr);
    if (descriptorPool_)             vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    if (computeDescriptorSetLayout_) vkDestroyDescriptorSetLayout(device_, computeDescriptorSetLayout_, nullptr);
    if (graphicsPipeline_)           vkDestroyPipeline(device_, graphicsPipeline_, nullptr);
    if (graphicsPipelineLayout_)     vkDestroyPipelineLayout(device_, graphicsPipelineLayout_, nullptr);

    for (auto fb : swapChainFramebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    for (auto iv : swapChainImageViews_)   vkDestroyImageView(device_, iv, nullptr);
    if (renderPass_)  vkDestroyRenderPass(device_, renderPass_, nullptr);
    if (swapChain_)   vkDestroySwapchainKHR(device_, swapChain_, nullptr);
    if (device_)      vkDestroyDevice(device_, nullptr);
    if (surface_)     vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_)    vkDestroyInstance(instance_, nullptr);

    OH_LOG_INFO(LOG_APP, "Vulkan cleanup complete");
}

}  // namespace gpu_bench
