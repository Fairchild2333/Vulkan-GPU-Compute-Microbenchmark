#ifdef HAVE_VULKAN

#include "vulkan_backend.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>

namespace gpu_bench {

void VulkanBackend::InitBackend() {
    CreateInstance();
    CreateSurface();
    PickPhysicalDevice();
    CreateLogicalDevice();
    CreateSwapChain();
    CreateImageViews();
    CreateRenderPass();
    CreateGraphicsPipeline();
    CreateComputeDescriptorSetLayout();
    CreateCommandPool();
    CreateParticleBuffer();
    CreateComputeDescriptorResources();
    CreateComputePipeline();
    CreateFramebuffers();
    CreateCommandBuffers();
    CreateSyncObjects();
    CreateTimestampQueryPool();
}

void VulkanBackend::WaitIdle() {
    if (device_ != VK_NULL_HANDLE)
        vkDeviceWaitIdle(device_);
}

// -----------------------------------------------------------------------
// Instance & Surface
// -----------------------------------------------------------------------

void VulkanBackend::CreateInstance() {
    VkApplicationInfo appInfo{};
    appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName   = "GpuComputeBenchmark";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 2, 0);
    appInfo.pEngineName        = "NoEngine";
    appInfo.engineVersion      = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion         = VK_API_VERSION_1_2;

    std::uint32_t glfwExtCount = 0;
    const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
    std::vector<const char*> extensions(glfwExts, glfwExts + glfwExtCount);

    VkInstanceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo        = &appInfo;
    ci.enabledExtensionCount   = static_cast<std::uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();

    if (vkCreateInstance(&ci, nullptr, &instance_) != VK_SUCCESS)
        throw std::runtime_error("vkCreateInstance failed");
}

void VulkanBackend::CreateSurface() {
    if (glfwCreateWindowSurface(instance_, window_, nullptr, &surface_) != VK_SUCCESS)
        throw std::runtime_error("glfwCreateWindowSurface failed");
}

// -----------------------------------------------------------------------
// Physical / Logical device
// -----------------------------------------------------------------------

QueueFamilyIndices VulkanBackend::FindQueueFamilies(VkPhysicalDevice device) const {
    QueueFamilyIndices indices;
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

    for (std::uint32_t i = 0; i < count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) indices.graphicsFamily = i;
        if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)  indices.computeFamily  = i;
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &present);
        if (present) indices.presentFamily = i;
        if (indices.IsComplete()) break;
    }
    return indices;
}

SwapChainSupportDetails VulkanBackend::QuerySwapChainSupport(VkPhysicalDevice device) const {
    SwapChainSupportDetails d;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface_, &d.capabilities);

    std::uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &n, nullptr);
    if (n) { d.formats.resize(n); vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &n, d.formats.data()); }

    n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &n, nullptr);
    if (n) { d.presentModes.resize(n); vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &n, d.presentModes.data()); }

    return d;
}

bool VulkanBackend::CheckDeviceExtensionSupport(VkPhysicalDevice device) const {
    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data());

    std::set<std::string> required(std::begin(kRequiredDeviceExtensions),
                                   std::end(kRequiredDeviceExtensions));
    for (auto& ext : available) required.erase(ext.extensionName);
    return required.empty();
}

bool VulkanBackend::IsDeviceSuitable(VkPhysicalDevice device) const {
    const auto idx = FindQueueFamilies(device);
    if (!idx.IsComplete() || !CheckDeviceExtensionSupport(device)) return false;
    const auto sc = QuerySwapChainSupport(device);
    return !sc.formats.empty() && !sc.presentModes.empty();
}

const char* VulkanBackend::DeviceTypeName(VkPhysicalDeviceType type) {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "Discrete GPU";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "Integrated GPU";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "Virtual GPU";
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "CPU";
        default:                                     return "Other";
    }
}

void VulkanBackend::PickPhysicalDevice() {
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) throw std::runtime_error("No Vulkan physical device found");

    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    std::vector<std::uint32_t> suitable;
    std::cout << "Available GPUs:\n";
    for (std::uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(devices[i], &p);
        bool ok = IsDeviceSuitable(devices[i]);
        std::cout << "  [" << i << "] " << p.deviceName
                  << " (" << DeviceTypeName(p.deviceType) << ")"
                  << (ok ? "" : " [unsuitable]") << '\n';
        if (ok) suitable.push_back(i);
    }
    if (suitable.empty()) throw std::runtime_error("No suitable Vulkan device");

    std::uint32_t chosen = suitable[0];
    if (requestedGpuIndex_ >= 0) {
        auto idx = static_cast<std::uint32_t>(requestedGpuIndex_);
        if (idx >= count || !IsDeviceSuitable(devices[idx]))
            throw std::runtime_error("Requested GPU index unsuitable");
        chosen = idx;
    } else if (suitable.size() > 1) {
        std::cout << "Enter GPU index (or 'b' to go back): ";
        std::string line;
        if (std::getline(std::cin, line) && !line.empty()) {
            if (line == "b" || line == "B")
                throw gpu_bench::BackToMenuException();
            auto idx = static_cast<std::uint32_t>(std::stoi(line));
            if (idx >= count || !IsDeviceSuitable(devices[idx]))
                throw std::runtime_error("GPU index unsuitable");
            chosen = idx;
        }
    }

    physicalDevice_ = devices[chosen];
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice_, &props);
    deviceName_ = props.deviceName;

    // Decode driver version — NVIDIA uses a custom encoding, others use Vulkan standard.
    std::uint32_t dv = props.driverVersion;
    if (props.vendorID == 0x10de) {
        driverVersion_ = std::to_string((dv >> 22) & 0x3ff) + "."
                       + std::to_string((dv >> 14) & 0xff) + "."
                       + std::to_string((dv >> 6) & 0xff) + "."
                       + std::to_string(dv & 0x3f);
    } else {
        driverVersion_ = std::to_string(VK_VERSION_MAJOR(dv)) + "."
                       + std::to_string(VK_VERSION_MINOR(dv)) + "."
                       + std::to_string(VK_VERSION_PATCH(dv));
    }

    // Try Vulkan 1.2 driver properties for a more descriptive string.
    VkPhysicalDeviceDriverProperties driverProps{};
    driverProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &driverProps;
    vkGetPhysicalDeviceProperties2(physicalDevice_, &props2);
    if (driverProps.driverInfo[0] != '\0')
        driverVersion_ = std::string(driverProps.driverName) + " " + driverProps.driverInfo;
    else if (driverProps.driverName[0] != '\0')
        driverVersion_ = std::string(driverProps.driverName) + " " + driverVersion_;

    std::cout << "Selected GPU [" << chosen << "]: " << deviceName_
              << "  |  Driver: " << driverVersion_ << std::endl;
}

void VulkanBackend::CreateLogicalDevice() {
    const auto indices = FindQueueFamilies(physicalDevice_);
    std::set<std::uint32_t> unique = {
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

    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(physicalDevice_, &supported);
    VkPhysicalDeviceFeatures features{};
    if (supported.largePoints) features.largePoints = VK_TRUE;

    VkDeviceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount    = static_cast<std::uint32_t>(queueCIs.size());
    ci.pQueueCreateInfos       = queueCIs.data();
    ci.pEnabledFeatures        = &features;
    ci.enabledExtensionCount   = static_cast<std::uint32_t>(std::size(kRequiredDeviceExtensions));
    ci.ppEnabledExtensionNames = kRequiredDeviceExtensions;

    if (vkCreateDevice(physicalDevice_, &ci, nullptr, &device_) != VK_SUCCESS)
        throw std::runtime_error("vkCreateDevice failed");

    vkGetDeviceQueue(device_, indices.graphicsFamily.value(), 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, indices.presentFamily.value(),  0, &presentQueue_);
    vkGetDeviceQueue(device_, indices.computeFamily.value(),  0, &computeQueue_);
}

// -----------------------------------------------------------------------
// Buffers
// -----------------------------------------------------------------------

VkShaderModule VulkanBackend::CreateShaderModule(const std::vector<char>& code) const {
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const std::uint32_t*>(code.data());
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &ci, nullptr, &m) != VK_SUCCESS)
        throw std::runtime_error("vkCreateShaderModule failed");
    return m;
}

std::uint32_t VulkanBackend::FindMemoryType(std::uint32_t filter, VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &mem);
    for (std::uint32_t i = 0; i < mem.memoryTypeCount; ++i)
        if ((filter & (1u << i)) && (mem.memoryTypes[i].propertyFlags & props) == props)
            return i;
    throw std::runtime_error("Failed to find suitable memory type");
}

void VulkanBackend::CreateParticleBuffer() {
    const VkDeviceSize size = sizeof(Particle) * config_.particleCount;

    auto createBuffer = [&](VkDeviceSize sz, VkBufferUsageFlags usage,
                            VkMemoryPropertyFlags memProps,
                            VkBuffer& buf, VkDeviceMemory& mem) {
        VkBufferCreateInfo bi{};
        bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size        = sz;
        bi.usage       = usage;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device_, &bi, nullptr, &buf) != VK_SUCCESS)
            throw std::runtime_error("vkCreateBuffer failed");

        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device_, buf, &req);

        VkMemoryAllocateInfo ai{};
        ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, memProps);
        if (vkAllocateMemory(device_, &ai, nullptr, &mem) != VK_SUCCESS)
            throw std::runtime_error("vkAllocateMemory failed");

        vkBindBufferMemory(device_, buf, mem, 0);
    };

    const VkBufferUsageFlags gpuUsage =
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    if (config_.hostMemory) {
        createBuffer(size, gpuUsage,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     particleBuffer_, particleBufferMemory_);

        void* data = nullptr;
        vkMapMemory(device_, particleBufferMemory_, 0, size, 0, &data);
        std::memcpy(data, initialParticles_.data(), static_cast<std::size_t>(size));
        vkUnmapMemory(device_, particleBufferMemory_);

        std::cout << "Created particle buffer (host-visible): "
                  << config_.particleCount << " particles\n";
    } else {
        createBuffer(size, gpuUsage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     particleBuffer_, particleBufferMemory_);

        VkBuffer       stagingBuf = VK_NULL_HANDLE;
        VkDeviceMemory stagingMem = VK_NULL_HANDLE;
        createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     stagingBuf, stagingMem);

        void* data = nullptr;
        vkMapMemory(device_, stagingMem, 0, size, 0, &data);
        std::memcpy(data, initialParticles_.data(), static_cast<std::size_t>(size));
        vkUnmapMemory(device_, stagingMem);

        VkCommandBufferAllocateInfo cmdAi{};
        cmdAi.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdAi.commandPool        = commandPool_;
        cmdAi.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdAi.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(device_, &cmdAi, &cmd);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        VkBufferCopy region{};
        region.size = size;
        vkCmdCopyBuffer(cmd, stagingBuf, particleBuffer_, 1, &region);

        vkEndCommandBuffer(cmd);

        VkSubmitInfo si{};
        si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers    = &cmd;
        vkQueueSubmit(graphicsQueue_, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(graphicsQueue_);

        vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
        vkDestroyBuffer(device_, stagingBuf, nullptr);
        vkFreeMemory(device_, stagingMem, nullptr);

        std::cout << "Created particle buffer (device-local via staging): "
                  << config_.particleCount << " particles\n";
    }
}

// -----------------------------------------------------------------------
// Swap chain
// -----------------------------------------------------------------------

VkSurfaceFormatKHR VulkanBackend::ChooseSwapSurfaceFormat(
        const std::vector<VkSurfaceFormatKHR>& available) const {
    for (auto& f : available)
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return f;
    return available[0];
}

VkPresentModeKHR VulkanBackend::ChooseSwapPresentMode(
        const std::vector<VkPresentModeKHR>& available) const {
    if (!config_.vsync) {
        for (auto m : available)
            if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) return m;
        for (auto m : available)
            if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m;
    } else {
        for (auto m : available)
            if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanBackend::ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& caps) const {
    if (caps.currentExtent.width != std::numeric_limits<std::uint32_t>::max())
        return caps.currentExtent;
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    return {
        std::clamp(static_cast<std::uint32_t>(w), caps.minImageExtent.width,  caps.maxImageExtent.width),
        std::clamp(static_cast<std::uint32_t>(h), caps.minImageExtent.height, caps.maxImageExtent.height)
    };
}

void VulkanBackend::CreateSwapChain() {
    auto sc  = QuerySwapChainSupport(physicalDevice_);
    auto fmt = ChooseSwapSurfaceFormat(sc.formats);
    auto pm  = ChooseSwapPresentMode(sc.presentModes);
    auto ext = ChooseSwapExtent(sc.capabilities);

    std::uint32_t imgCount = sc.capabilities.minImageCount + 1;
    if (sc.capabilities.maxImageCount > 0 && imgCount > sc.capabilities.maxImageCount)
        imgCount = sc.capabilities.maxImageCount;

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
    std::uint32_t qfi[] = { idx.graphicsFamily.value(), idx.presentFamily.value() };
    if (idx.graphicsFamily != idx.presentFamily) {
        ci.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = 2;
        ci.pQueueFamilyIndices   = qfi;
    } else {
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    ci.preTransform   = sc.capabilities.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode    = pm;
    ci.clipped        = VK_TRUE;

    if (vkCreateSwapchainKHR(device_, &ci, nullptr, &swapChain_) != VK_SUCCESS)
        throw std::runtime_error("vkCreateSwapchainKHR failed");

    vkGetSwapchainImagesKHR(device_, swapChain_, &imgCount, nullptr);
    swapChainImages_.resize(imgCount);
    vkGetSwapchainImagesKHR(device_, swapChain_, &imgCount, swapChainImages_.data());
    swapChainImageFormat_ = fmt.format;
    swapChainExtent_      = ext;
}

void VulkanBackend::CreateImageViews() {
    swapChainImageViews_.resize(swapChainImages_.size());
    for (std::size_t i = 0; i < swapChainImages_.size(); ++i) {
        VkImageViewCreateInfo ci{};
        ci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ci.image    = swapChainImages_[i];
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ci.format   = swapChainImageFormat_;
        ci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(device_, &ci, nullptr, &swapChainImageViews_[i]) != VK_SUCCESS)
            throw std::runtime_error("vkCreateImageView failed");
    }
}

void VulkanBackend::CreateRenderPass() {
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

    if (vkCreateRenderPass(device_, &ci, nullptr, &renderPass_) != VK_SUCCESS)
        throw std::runtime_error("vkCreateRenderPass failed");
}

void VulkanBackend::CreateFramebuffers() {
    swapChainFramebuffers_.resize(swapChainImageViews_.size());
    for (std::size_t i = 0; i < swapChainImageViews_.size(); ++i) {
        VkImageView att[] = { swapChainImageViews_[i] };
        VkFramebufferCreateInfo ci{};
        ci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        ci.renderPass      = renderPass_;
        ci.attachmentCount = 1;
        ci.pAttachments    = att;
        ci.width           = swapChainExtent_.width;
        ci.height          = swapChainExtent_.height;
        ci.layers          = 1;
        if (vkCreateFramebuffer(device_, &ci, nullptr, &swapChainFramebuffers_[i]) != VK_SUCCESS)
            throw std::runtime_error("vkCreateFramebuffer failed");
    }
}

// -----------------------------------------------------------------------
// Pipelines
// -----------------------------------------------------------------------

void VulkanBackend::CreateGraphicsPipeline() {
    auto vertCode = ReadFileBytes(shaderDir_ + "particle.vert.spv");
    auto fragCode = ReadFileBytes(shaderDir_ + "particle.frag.spv");
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

    VkViewport vp{ 0, 0, (float)swapChainExtent_.width, (float)swapChainExtent_.height, 0, 1 };
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
    if (vkCreatePipelineLayout(device_, &pli, nullptr, &graphicsPipelineLayout_) != VK_SUCCESS)
        throw std::runtime_error("vkCreatePipelineLayout failed");

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

    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pi, nullptr, &graphicsPipeline_) != VK_SUCCESS)
        throw std::runtime_error("vkCreateGraphicsPipelines failed");

    vkDestroyShaderModule(device_, fragMod, nullptr);
    vkDestroyShaderModule(device_, vertMod, nullptr);
}

void VulkanBackend::CreateComputeDescriptorSetLayout() {
    VkDescriptorSetLayoutBinding ssbo{};
    ssbo.binding         = 0;
    ssbo.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ssbo.descriptorCount = 1;
    ssbo.stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = 1;
    ci.pBindings    = &ssbo;

    if (vkCreateDescriptorSetLayout(device_, &ci, nullptr, &computeDescriptorSetLayout_) != VK_SUCCESS)
        throw std::runtime_error("vkCreateDescriptorSetLayout failed");
}

void VulkanBackend::CreateComputeDescriptorResources() {
    VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo pi{};
    pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.poolSizeCount = 1;  pi.pPoolSizes = &ps;
    pi.maxSets       = 1;
    if (vkCreateDescriptorPool(device_, &pi, nullptr, &descriptorPool_) != VK_SUCCESS)
        throw std::runtime_error("vkCreateDescriptorPool failed");

    VkDescriptorSetAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool     = descriptorPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &computeDescriptorSetLayout_;
    if (vkAllocateDescriptorSets(device_, &ai, &computeDescriptorSet_) != VK_SUCCESS)
        throw std::runtime_error("vkAllocateDescriptorSets failed");

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

void VulkanBackend::CreateComputePipeline() {
    auto code = ReadFileBytes(shaderDir_ + "compute.comp.spv");
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
    if (vkCreatePipelineLayout(device_, &li, nullptr, &computePipelineLayout_) != VK_SUCCESS)
        throw std::runtime_error("vkCreatePipelineLayout failed");

    VkComputePipelineCreateInfo ci{};
    ci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage  = si;
    ci.layout = computePipelineLayout_;
    if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &computePipeline_) != VK_SUCCESS)
        throw std::runtime_error("vkCreateComputePipelines failed");

    vkDestroyShaderModule(device_, mod, nullptr);
}

// -----------------------------------------------------------------------
// Command infrastructure
// -----------------------------------------------------------------------

void VulkanBackend::CreateCommandPool() {
    auto idx = FindQueueFamilies(physicalDevice_);
    VkCommandPoolCreateInfo ci{};
    ci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = idx.graphicsFamily.value();
    if (vkCreateCommandPool(device_, &ci, nullptr, &commandPool_) != VK_SUCCESS)
        throw std::runtime_error("vkCreateCommandPool failed");
}

void VulkanBackend::CreateCommandBuffers() {
    commandBuffers_.resize(swapChainFramebuffers_.size());
    VkCommandBufferAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool        = commandPool_;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = static_cast<std::uint32_t>(commandBuffers_.size());
    if (vkAllocateCommandBuffers(device_, &ai, commandBuffers_.data()) != VK_SUCCESS)
        throw std::runtime_error("vkAllocateCommandBuffers failed");
}

void VulkanBackend::CreateSyncObjects() {
    VkSemaphoreCreateInfo si{}; si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (vkCreateSemaphore(device_, &si, nullptr, &imageAvailableSemaphores_[i]) != VK_SUCCESS ||
            vkCreateSemaphore(device_, &si, nullptr, &renderFinishedSemaphores_[i]) != VK_SUCCESS ||
            vkCreateFence(device_, &fi, nullptr, &inFlightFences_[i]) != VK_SUCCESS)
            throw std::runtime_error("Failed to create sync objects");
    }
    imagesInFlight_.resize(swapChainImages_.size(), VK_NULL_HANDLE);
}

void VulkanBackend::CreateTimestampQueryPool() {
    auto idx = FindQueueFamilies(physicalDevice_);
    std::uint32_t cnt = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &cnt, nullptr);
    std::vector<VkQueueFamilyProperties> fams(cnt);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &cnt, fams.data());

    if (fams[idx.graphicsFamily.value()].timestampValidBits == 0) {
        std::cout << "[Profiling] Timestamps not supported -- disabled.\n";
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
        std::cerr << "[Profiling] Failed to create query pool -- disabled.\n";
        return;
    }

    timestampsSupported_ = true;
    std::cout << "[Profiling] Timestamp queries enabled (period = "
              << timestampPeriodNs_ << " ns/tick)\n";
}

void VulkanBackend::CollectTimestampResults(std::uint32_t slot) {
    if (!timestampsSupported_) return;

    std::uint32_t first = slot * kTimestampsPerFrame;
    std::array<std::uint64_t, 4> ts{};
    if (vkGetQueryPoolResults(device_, timestampQueryPool_, first, 4,
            sizeof(ts), ts.data(), sizeof(std::uint64_t),
            VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;

    const double toMs = static_cast<double>(timestampPeriodNs_) / 1'000'000.0;
    AccumulateTiming(
        static_cast<double>(ts[1] - ts[0]) * toMs,
        static_cast<double>(ts[3] - ts[2]) * toMs,
        static_cast<double>(ts[3] - ts[0]) * toMs);
}

// -----------------------------------------------------------------------
// Per-frame recording
// -----------------------------------------------------------------------

void VulkanBackend::RecordCommandBuffer(std::uint32_t imageIndex, float deltaTime) {
    VkCommandBuffer cmd = commandBuffers_[imageIndex];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS)
        throw std::runtime_error("vkBeginCommandBuffer failed");

    const std::uint32_t tsBase = currentFrame_ * kTimestampsPerFrame;
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
    vkCmdDispatch(cmd, config_.particleCount / kComputeWorkGroupSize, 1, 1);

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
    vkCmdDraw(cmd, config_.particleCount, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    if (timestampsSupported_)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, timestampQueryPool_, tsBase + 3);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
        throw std::runtime_error("vkEndCommandBuffer failed");
}

// -----------------------------------------------------------------------
// Frame
// -----------------------------------------------------------------------

void VulkanBackend::DrawFrame(float deltaTime) {
    vkWaitForFences(device_, 1, &inFlightFences_[currentFrame_], VK_TRUE, UINT64_MAX);
    CollectTimestampResults(currentFrame_);

    std::uint32_t imageIndex = 0;
    VkResult res = vkAcquireNextImageKHR(device_, swapChain_, UINT64_MAX,
                                         imageAvailableSemaphores_[currentFrame_],
                                         VK_NULL_HANDLE, &imageIndex);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) return;
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
        throw std::runtime_error("vkAcquireNextImageKHR failed");

    if (imagesInFlight_[imageIndex] != VK_NULL_HANDLE)
        vkWaitForFences(device_, 1, &imagesInFlight_[imageIndex], VK_TRUE, UINT64_MAX);
    imagesInFlight_[imageIndex] = inFlightFences_[currentFrame_];

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

    res = vkQueueSubmit(graphicsQueue_, 1, &si, inFlightFences_[currentFrame_]);
    if (res != VK_SUCCESS) {
        std::string msg = "vkQueueSubmit failed (VkResult " + std::to_string(static_cast<int>(res)) + ")";
        if (res == VK_ERROR_DEVICE_LOST)
            msg += " -- GPU device lost; try restarting the application or rebooting";
        throw std::runtime_error(msg);
    }

    VkSwapchainKHR chains[] = { swapChain_ };
    VkPresentInfoKHR pi{};
    pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = sigSem;
    pi.swapchainCount     = 1; pi.pSwapchains     = chains;
    pi.pImageIndices      = &imageIndex;
    vkQueuePresentKHR(presentQueue_, &pi);

    currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
}

// -----------------------------------------------------------------------
// Cleanup
// -----------------------------------------------------------------------

void VulkanBackend::CleanupSwapChain() {
    for (auto fb : swapChainFramebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    swapChainFramebuffers_.clear();
    for (auto iv : swapChainImageViews_)   vkDestroyImageView(device_, iv, nullptr);
    swapChainImageViews_.clear();
    imagesInFlight_.clear();
    if (renderPass_ != VK_NULL_HANDLE) { vkDestroyRenderPass(device_, renderPass_, nullptr); renderPass_ = VK_NULL_HANDLE; }
    if (swapChain_  != VK_NULL_HANDLE) { vkDestroySwapchainKHR(device_, swapChain_, nullptr); swapChain_ = VK_NULL_HANDLE; }
}

void VulkanBackend::CleanupBackend() {
    for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (imageAvailableSemaphores_[i]) vkDestroySemaphore(device_, imageAvailableSemaphores_[i], nullptr);
        if (renderFinishedSemaphores_[i]) vkDestroySemaphore(device_, renderFinishedSemaphores_[i], nullptr);
        if (inFlightFences_[i])           vkDestroyFence(device_, inFlightFences_[i], nullptr);
    }
    if (timestampQueryPool_)        { vkDestroyQueryPool(device_, timestampQueryPool_, nullptr); }
    if (commandPool_)               { vkDestroyCommandPool(device_, commandPool_, nullptr); }
    if (particleBuffer_)            { vkDestroyBuffer(device_, particleBuffer_, nullptr); }
    if (particleBufferMemory_)      { vkFreeMemory(device_, particleBufferMemory_, nullptr); }
    if (computePipeline_)           { vkDestroyPipeline(device_, computePipeline_, nullptr); }
    if (computePipelineLayout_)     { vkDestroyPipelineLayout(device_, computePipelineLayout_, nullptr); }
    if (descriptorPool_)            { vkDestroyDescriptorPool(device_, descriptorPool_, nullptr); }
    if (computeDescriptorSetLayout_){ vkDestroyDescriptorSetLayout(device_, computeDescriptorSetLayout_, nullptr); }
    if (graphicsPipeline_)          { vkDestroyPipeline(device_, graphicsPipeline_, nullptr); }
    if (graphicsPipelineLayout_)    { vkDestroyPipelineLayout(device_, graphicsPipelineLayout_, nullptr); }
    CleanupSwapChain();
    if (device_)   vkDestroyDevice(device_, nullptr);
    if (surface_)  vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

}  // namespace gpu_bench

#endif  // HAVE_VULKAN
