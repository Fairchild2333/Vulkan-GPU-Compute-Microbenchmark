// Vulkan GPU Compute & Rendering Pipeline
//
// A combined compute + graphics pipeline that renders a particle system.
// Each frame the compute shader integrates particle positions, then the
// graphics pipeline draws the updated particles as point-list primitives.
// Targets Vulkan 1.2 for broad driver support on both discrete and
// integrated GPUs.

#ifndef GLFW_INCLUDE_VULKAN
#define GLFW_INCLUDE_VULKAN
#endif
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kWindowWidth = 1280;
constexpr std::uint32_t kWindowHeight = 720;
// Double-buffered frame synchronisation; allows the CPU to prepare frame N+1
// while the GPU is still rendering frame N.
constexpr std::uint32_t kMaxFramesInFlight = 2;
constexpr std::uint32_t kParticleCount = 65536;
// Must match local_size_x in compute.comp.
constexpr std::uint32_t kComputeWorkGroupSize = 256;

// 4 timestamps per frame: before/after compute, before/after render.
constexpr std::uint32_t kTimestampsPerFrame = 4;
// Print GPU timing summary once per second.
constexpr double kTimingReportIntervalSec = 1.0;

// Must match the GLSL Particle struct layout (2 x vec4 = 32 bytes per particle).
// STORAGE_BUFFER_BIT on the buffer ensures the compute shader can read/write
// the same memory through an SSBO binding.
struct Particle {
    float px, py, pz, pw;   // position (xy used; zw reserved)
    float vx, vy, vz, vw;   // velocity (xy used; zw reserved)
};

// Push-constant block passed to the compute shader each frame.
// Must match the layout(push_constant) declaration in compute.comp.
struct ComputeParams {
    float deltaTime;
    float bounds;
};

// We require three distinct queue capabilities:
//   - Graphics: rasterisation and draw commands
//   - Present:  displaying rendered images on the window surface
//   - Compute:  general-purpose GPU compute dispatches
// On most GPUs the graphics and compute families coincide, but they are
// tracked separately to remain correct on hardware where they differ.
struct QueueFamilyIndices {
    std::optional<std::uint32_t> graphicsFamily;
    std::optional<std::uint32_t> presentFamily;
    std::optional<std::uint32_t> computeFamily;

    bool IsComplete() const {
        return graphicsFamily.has_value() && presentFamily.has_value() && computeFamily.has_value();
    }
};

// Cached result of querying surface capabilities, supported formats, and
// present modes.  Used to configure the swapchain at creation time.
struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

class VulkanApp {
public:
    explicit VulkanApp(std::int32_t gpuIndex = -1, std::string shaderDir = "")
        : requestedGpuIndex_(gpuIndex), shaderDir_(std::move(shaderDir)) {}

    void Run() {
        InitWindow();
        InitVulkan();
        MainLoop();
        Cleanup();
    }

private:
    std::int32_t requestedGpuIndex_ = -1;
    std::string shaderDir_;
    GLFWwindow* window_ = nullptr;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;

    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    VkQueue computeQueue_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapChain_ = VK_NULL_HANDLE;
    std::vector<VkImage> swapChainImages_;
    std::vector<VkImageView> swapChainImageViews_;
    std::vector<VkFramebuffer> swapChainFramebuffers_;
    VkFormat swapChainImageFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapChainExtent_{};

    VkRenderPass renderPass_ = VK_NULL_HANDLE;

    VkPipelineLayout graphicsPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline graphicsPipeline_ = VK_NULL_HANDLE;

    // Compute pipeline resources.
    VkDescriptorSetLayout computeDescriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet computeDescriptorSet_ = VK_NULL_HANDLE;
    VkPipelineLayout computePipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline computePipeline_ = VK_NULL_HANDLE;

    VkBuffer particleBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory particleBufferMemory_ = VK_NULL_HANDLE;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;

    std::array<VkSemaphore, kMaxFramesInFlight> imageAvailableSemaphores_{};
    std::array<VkSemaphore, kMaxFramesInFlight> renderFinishedSemaphores_{};
    std::array<VkFence, kMaxFramesInFlight> inFlightFences_{};
    std::uint32_t currentFrame_ = 0;

    double lastFrameTime_ = 0.0;

    // --- GPU timestamp profiling ---
    VkQueryPool timestampQueryPool_ = VK_NULL_HANDLE;
    // Nanoseconds per timestamp tick, converted from the device limit.
    float timestampPeriodNs_ = 0.0f;
    // Whether the selected queue family supports timestamp queries.
    bool timestampsSupported_ = false;
    // Accumulate GPU timing over the report interval.
    double accumComputeMs_ = 0.0;
    double accumRenderMs_ = 0.0;
    double accumTotalGpuMs_ = 0.0;
    std::uint32_t timingSampleCount_ = 0;
    double timingReportTimer_ = 0.0;
    std::uint32_t frameCount_ = 0;

    static constexpr const char* kRequiredDeviceExtensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };

    // GLFW_NO_API tells GLFW not to create an OpenGL context; we manage
    // the Vulkan surface ourselves.  Resizing is disabled to avoid the
    // complexity of swapchain recreation for now.
    void InitWindow() {
        if (glfwInit() != GLFW_TRUE) {
            throw std::runtime_error("glfwInit failed");
        }

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

        window_ = glfwCreateWindow(
            static_cast<int>(kWindowWidth),
            static_cast<int>(kWindowHeight),
            "Vulkan GPU Compute & Rendering Pipeline",
            nullptr,
            nullptr);

        if (window_ == nullptr) {
            throw std::runtime_error("glfwCreateWindow failed");
        }
    }

    // Initialisation order matters:
    //   1. Instance + Surface must exist before we can query physical devices.
    //   2. Logical device must exist before any resource creation.
    //   3. Swapchain + image views + render pass must precede the graphics
    //      pipeline (which references the render pass and swapchain extent).
    //   4. The particle buffer is created after the pipeline so that the
    //      pipeline layout is already available for future descriptor sets.
    //   5. Framebuffers wrap image views, so they come after image views.
    //   6. Command buffers are recorded last because they reference the
    //      pipeline, vertex buffer, and framebuffers.
    void InitVulkan() {
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
    }

    // Vulkan 1.2 is chosen for broad compatibility across both NVIDIA and
    // AMD drivers whilst still providing features like timeline semaphores.
    // GLFW tells us which instance extensions are needed for window-surface
    // integration (e.g. VK_KHR_surface, VK_KHR_win32_surface).
    void CreateInstance() {
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "VulkanGpuComputeRenderer";
        appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.pEngineName = "NoEngine";
        appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.apiVersion = VK_API_VERSION_1_2;

        std::uint32_t glfwExtensionCount = 0;
        const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

        std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();

        if (vkCreateInstance(&createInfo, nullptr, &instance_) != VK_SUCCESS) {
            throw std::runtime_error("vkCreateInstance failed");
        }
    }

    void CreateSurface() {
        if (glfwCreateWindowSurface(instance_, window_, nullptr, &surface_) != VK_SUCCESS) {
            throw std::runtime_error("glfwCreateWindowSurface failed");
        }
    }

    // Enumerate queue families and record the first index that supports each
    // capability.  Early-out once all three are found.
    QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device) const {
        QueueFamilyIndices indices;

        std::uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

        std::uint32_t i = 0;
        for (const auto& queueFamily : queueFamilies) {
            if ((queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
                indices.graphicsFamily = i;
            }

            if ((queueFamily.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0U) {
                indices.computeFamily = i;
            }

            VkBool32 presentSupport = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &presentSupport);
            if (presentSupport == VK_TRUE) {
                indices.presentFamily = i;
            }

            if (indices.IsComplete()) {
                break;
            }

            ++i;
        }

        return indices;
    }

    // Two-pass pattern: first call with nullptr to get the count, then
    // allocate and call again to fill the data.  Standard Vulkan idiom.
    SwapChainSupportDetails QuerySwapChainSupport(VkPhysicalDevice device) const {
        SwapChainSupportDetails details;

        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface_, &details.capabilities);

        std::uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, nullptr);
        if (formatCount > 0) {
            details.formats.resize(formatCount);
            vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, details.formats.data());
        }

        std::uint32_t presentModeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount, nullptr);
        if (presentModeCount > 0) {
            details.presentModes.resize(presentModeCount);
            vkGetPhysicalDeviceSurfacePresentModesKHR(
                device,
                surface_,
                &presentModeCount,
                details.presentModes.data());
        }

        return details;
    }

    // Verify that the device supports every extension in kRequiredDeviceExtensions
    // by removing available ones from a set and checking the set is empty.
    bool CheckDeviceExtensionSupport(VkPhysicalDevice device) const {
        std::uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

        std::vector<VkExtensionProperties> availableExtensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

        std::set<std::string> requiredExtensions(
            std::begin(kRequiredDeviceExtensions),
            std::end(kRequiredDeviceExtensions));

        for (const auto& extension : availableExtensions) {
            requiredExtensions.erase(extension.extensionName);
        }

        return requiredExtensions.empty();
    }

    // A device is suitable only if it has all required queue families, supports
    // the needed extensions (VK_KHR_swapchain), and offers at least one
    // surface format + present mode pair for swapchain creation.
    bool IsDeviceSuitable(VkPhysicalDevice device) const {
        const QueueFamilyIndices indices = FindQueueFamilies(device);
        const bool extensionsSupported = CheckDeviceExtensionSupport(device);

        bool swapChainAdequate = false;
        if (extensionsSupported) {
            const auto swapChainSupport = QuerySwapChainSupport(device);
            swapChainAdequate =
                !swapChainSupport.formats.empty() && !swapChainSupport.presentModes.empty();
        }

        return indices.IsComplete() && extensionsSupported && swapChainAdequate;
    }

    // Open the file at the end (ios::ate) so tellg() immediately gives us
    // the file size without an extra seek, then rewind and read in one go.
    static std::vector<char> ReadFile(const std::string& filename) {
        std::ifstream file(filename, std::ios::ate | std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open file: " + filename);
        }
        const auto fileSize = static_cast<std::size_t>(file.tellg());
        std::vector<char> buffer(fileSize);
        file.seekg(0);
        file.read(buffer.data(), static_cast<std::streamsize>(fileSize));
        return buffer;
    }

    // Wrap raw SPIR-V bytecode in a VkShaderModule.  The module is only needed
    // during pipeline creation and can be destroyed immediately afterwards.
    VkShaderModule CreateShaderModule(const std::vector<char>& code) const {
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = code.size();
        createInfo.pCode = reinterpret_cast<const std::uint32_t*>(code.data());

        VkShaderModule shaderModule = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device_, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
            throw std::runtime_error("vkCreateShaderModule failed");
        }
        return shaderModule;
    }

    // Walk the physical device's memory types and return the first index whose
    // bit is set in typeFilter AND whose property flags include all requested
    // flags (e.g. HOST_VISIBLE | HOST_COHERENT for CPU-mappable memory).
    std::uint32_t FindMemoryType(std::uint32_t typeFilter, VkMemoryPropertyFlags properties) const {
        VkPhysicalDeviceMemoryProperties memProperties{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProperties);

        for (std::uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
            if ((typeFilter & (1u << i)) != 0 &&
                (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        throw std::runtime_error("Failed to find suitable memory type");
    }

    // Translate the Vulkan deviceType enum (reported by the driver) to a
    // human-readable string for the GPU selection menu.
    static const char* DeviceTypeName(VkPhysicalDeviceType type) {
        switch (type) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "Discrete GPU";
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "Integrated GPU";
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "Virtual GPU";
            case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "CPU";
            default:                                     return "Other";
        }
    }

    // GPU selection strategy:
    //   --gpu N  : use the device at index N (from command line).
    //   1 device : auto-select without prompting.
    //   N devices: list all and prompt the user interactively.
    void PickPhysicalDevice() {
        std::uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);
        if (deviceCount == 0) {
            throw std::runtime_error("No Vulkan physical device found");
        }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());

        std::vector<std::uint32_t> suitableIndices;
        std::cout << "Available GPUs:\n";
        for (std::uint32_t i = 0; i < deviceCount; ++i) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(devices[i], &props);
            const bool suitable = IsDeviceSuitable(devices[i]);
            std::cout << "  [" << i << "] " << props.deviceName
                      << " (" << DeviceTypeName(props.deviceType) << ")"
                      << (suitable ? "" : " [unsuitable]") << '\n';
            if (suitable) {
                suitableIndices.push_back(i);
            }
        }

        if (suitableIndices.empty()) {
            throw std::runtime_error("No suitable Vulkan physical device found");
        }

        std::uint32_t chosen = suitableIndices[0];

        if (requestedGpuIndex_ >= 0) {
            const auto idx = static_cast<std::uint32_t>(requestedGpuIndex_);
            if (idx >= deviceCount || !IsDeviceSuitable(devices[idx])) {
                throw std::runtime_error("Requested GPU index " +
                    std::to_string(requestedGpuIndex_) + " is out of range or unsuitable");
            }
            chosen = idx;
        } else if (suitableIndices.size() == 1) {
            chosen = suitableIndices[0];
        } else {
            std::cout << "Enter GPU index to use: ";
            std::string line;
            if (std::getline(std::cin, line) && !line.empty()) {
                const auto idx = static_cast<std::uint32_t>(std::stoi(line));
                if (idx >= deviceCount || !IsDeviceSuitable(devices[idx])) {
                    throw std::runtime_error("GPU index " + line + " is out of range or unsuitable");
                }
                chosen = idx;
            }
        }

        physicalDevice_ = devices[chosen];

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physicalDevice_, &props);
        std::cout << "Selected GPU [" << chosen << "]: " << props.deviceName << std::endl;

        glfwShowWindow(window_);
    }

    // Create a logical device with one queue from each unique family.  A set
    // is used because on many GPUs the graphics, present, and compute families
    // share the same index; creating duplicate queues for the same family
    // would be invalid.
    void CreateLogicalDevice() {
        const QueueFamilyIndices indices = FindQueueFamilies(physicalDevice_);

        std::set<std::uint32_t> uniqueQueueFamilies = {
            indices.graphicsFamily.value(),
            indices.presentFamily.value(),
            indices.computeFamily.value()};

        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        const float queuePriority = 1.0f;

        for (const auto queueFamily : uniqueQueueFamilies) {
            VkDeviceQueueCreateInfo queueCreateInfo{};
            queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueCreateInfo.queueFamilyIndex = queueFamily;
            queueCreateInfo.queueCount = 1;
            queueCreateInfo.pQueuePriorities = &queuePriority;
            queueCreateInfos.push_back(queueCreateInfo);
        }

        // Enable largePoints if the GPU supports it so the vertex shader can
        // write gl_PointSize > 1.0.  Without this feature the driver clamps
        // point size to 1 pixel.
        VkPhysicalDeviceFeatures supportedFeatures{};
        vkGetPhysicalDeviceFeatures(physicalDevice_, &supportedFeatures);

        VkPhysicalDeviceFeatures deviceFeatures{};
        if (supportedFeatures.largePoints == VK_TRUE) {
            deviceFeatures.largePoints = VK_TRUE;
        }

        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queueCreateInfos.size());
        createInfo.pQueueCreateInfos = queueCreateInfos.data();
        createInfo.pEnabledFeatures = &deviceFeatures;
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(std::size(kRequiredDeviceExtensions));
        createInfo.ppEnabledExtensionNames = kRequiredDeviceExtensions;

        if (vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_) != VK_SUCCESS) {
            throw std::runtime_error("vkCreateDevice failed");
        }

        vkGetDeviceQueue(device_, indices.graphicsFamily.value(), 0, &graphicsQueue_);
        vkGetDeviceQueue(device_, indices.presentFamily.value(), 0, &presentQueue_);
        vkGetDeviceQueue(device_, indices.computeFamily.value(), 0, &computeQueue_);
    }

    // Allocate a single buffer that serves dual purpose:
    //   VERTEX_BUFFER_BIT  - bound as vertex input for the graphics pipeline
    //   STORAGE_BUFFER_BIT - bound as an SSBO for the compute pipeline (planned)
    // HOST_VISIBLE | HOST_COHERENT memory lets us map the buffer and write
    // initial particle data directly from the CPU.  A production renderer
    // would use a staging buffer + DEVICE_LOCAL memory for better throughput.
    void CreateParticleBuffer() {
        const VkDeviceSize bufferSize = sizeof(Particle) * kParticleCount;

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = bufferSize;
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(device_, &bufferInfo, nullptr, &particleBuffer_) != VK_SUCCESS) {
            throw std::runtime_error("vkCreateBuffer failed for particle buffer");
        }

        VkMemoryRequirements memRequirements{};
        vkGetBufferMemoryRequirements(device_, particleBuffer_, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = FindMemoryType(
            memRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(device_, &allocInfo, nullptr, &particleBufferMemory_) != VK_SUCCESS) {
            throw std::runtime_error("vkAllocateMemory failed for particle buffer");
        }

        vkBindBufferMemory(device_, particleBuffer_, particleBufferMemory_, 0);

        void* data = nullptr;
        vkMapMemory(device_, particleBufferMemory_, 0, bufferSize, 0, &data);

        // Seed with a fixed value for reproducible particle distributions.
        // Positions span [-0.8, 0.8] in NDC so particles stay within view;
        // velocities are small random values for the compute-driven animation.
        auto* particles = static_cast<Particle*>(data);
        std::mt19937 rng(42);
        std::uniform_real_distribution<float> posDist(-0.8f, 0.8f);
        std::uniform_real_distribution<float> velDist(-0.2f, 0.2f);

        for (std::uint32_t i = 0; i < kParticleCount; ++i) {
            particles[i] = {
                posDist(rng), posDist(rng), 0.0f, 1.0f,
                velDist(rng), velDist(rng), 0.0f, 0.0f,
            };
        }

        vkUnmapMemory(device_, particleBufferMemory_);
        std::cout << "Created particle buffer: " << kParticleCount << " particles\n";
    }

    // Build the complete graphics pipeline.  In Vulkan, nearly all rasterisation
    // state is baked into an immutable pipeline object at creation time, unlike
    // OpenGL where state is mutable.  The stages configured here are:
    //
    //   Shader stages          -> vertex + fragment SPIR-V modules
    //   Vertex input            -> one binding (Particle), two attributes (pos, vel)
    //   Input assembly          -> POINT_LIST topology (one point per particle)
    //   Viewport / scissor      -> matches swapchain extent
    //   Rasterisation           -> fill mode, no back-face culling
    //   Multisampling           -> 1 sample (no MSAA)
    //   Colour blending         -> standard alpha blending (src-alpha, 1-src-alpha)
    //   Pipeline layout         -> empty for now; will gain descriptor set
    //                              layouts once the compute pipeline is added
    void CreateGraphicsPipeline() {
        const auto vertCode = ReadFile(shaderDir_ + "particle.vert.spv");
        const auto fragCode = ReadFile(shaderDir_ + "particle.frag.spv");

        VkShaderModule vertModule = CreateShaderModule(vertCode);
        VkShaderModule fragModule = CreateShaderModule(fragCode);

        // --- Programmable shader stages ---
        VkPipelineShaderStageCreateInfo vertStageInfo{};
        vertStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertStageInfo.module = vertModule;
        vertStageInfo.pName = "main";

        VkPipelineShaderStageCreateInfo fragStageInfo{};
        fragStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragStageInfo.module = fragModule;
        fragStageInfo.pName = "main";

        VkPipelineShaderStageCreateInfo shaderStages[] = {vertStageInfo, fragStageInfo};

        // --- Vertex input ---
        // Binding 0 walks the Particle array with a stride of sizeof(Particle).
        // Attribute 0 (location 0) = position (vec4 at offset 0).
        // Attribute 1 (location 1) = velocity (vec4 at offset 16 bytes).
        VkVertexInputBindingDescription bindingDesc{};
        bindingDesc.binding = 0;
        bindingDesc.stride = sizeof(Particle);
        bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::array<VkVertexInputAttributeDescription, 2> attrDescs{};
        attrDescs[0].binding = 0;
        attrDescs[0].location = 0;
        attrDescs[0].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attrDescs[0].offset = offsetof(Particle, px);

        attrDescs[1].binding = 0;
        attrDescs[1].location = 1;
        attrDescs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attrDescs[1].offset = offsetof(Particle, vx);

        VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputInfo.vertexBindingDescriptionCount = 1;
        vertexInputInfo.pVertexBindingDescriptions = &bindingDesc;
        vertexInputInfo.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attrDescs.size());
        vertexInputInfo.pVertexAttributeDescriptions = attrDescs.data();

        // --- Input assembly ---
        // Each vertex is an independent point; no index buffer needed.
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        inputAssembly.primitiveRestartEnable = VK_FALSE;

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(swapChainExtent_.width);
        viewport.height = static_cast<float>(swapChainExtent_.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = swapChainExtent_;

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1;
        viewportState.pScissors = &scissor;

        // --- Rasterisation ---
        // No culling because point primitives are view-facing by definition.
        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.depthClampEnable = VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_NONE;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.depthBiasEnable = VK_FALSE;

        // --- Multisampling (disabled) ---
        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.sampleShadingEnable = VK_FALSE;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // --- Colour blending ---
        // Standard alpha blending: outColour = srcAlpha * srcColour + (1 - srcAlpha) * dstColour.
        // This produces correct semi-transparent particle overlaps.
        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachment.blendEnable = VK_TRUE;
        colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.logicOpEnable = VK_FALSE;
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &colorBlendAttachment;

        // --- Pipeline layout ---
        // Currently empty (no descriptor sets or push constants).  Will be
        // extended when the compute pipeline adds descriptor set layouts.
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

        if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &graphicsPipelineLayout_) !=
            VK_SUCCESS) {
            throw std::runtime_error("vkCreatePipelineLayout failed");
        }

        // --- Assemble the pipeline ---
        // All fixed-function and programmable state is combined into a single
        // immutable VkPipeline object.  Vulkan validates everything at creation
        // time so there are no hidden state mismatches at draw time.
        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shaderStages;
        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.layout = graphicsPipelineLayout_;
        pipelineInfo.renderPass = renderPass_;
        pipelineInfo.subpass = 0;

        if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                      &graphicsPipeline_) != VK_SUCCESS) {
            throw std::runtime_error("vkCreateGraphicsPipelines failed");
        }

        // Shader modules are only needed during pipeline creation; the compiled
        // GPU code is now embedded in the pipeline object.
        vkDestroyShaderModule(device_, fragModule, nullptr);
        vkDestroyShaderModule(device_, vertModule, nullptr);
    }

    // -----------------------------------------------------------------------
    // Compute pipeline setup
    // -----------------------------------------------------------------------

    // One descriptor binding: the particle SSBO at set=0, binding=0,
    // accessed only from the compute stage.
    void CreateComputeDescriptorSetLayout() {
        VkDescriptorSetLayoutBinding ssboBinding{};
        ssboBinding.binding = 0;
        ssboBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ssboBinding.descriptorCount = 1;
        ssboBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &ssboBinding;

        if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr,
                                        &computeDescriptorSetLayout_) != VK_SUCCESS) {
            throw std::runtime_error("vkCreateDescriptorSetLayout failed");
        }
    }

    // Allocate a descriptor pool large enough for one STORAGE_BUFFER descriptor
    // and allocate a single descriptor set that points to the particle buffer.
    void CreateComputeDescriptorResources() {
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = 1;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        poolInfo.maxSets = 1;

        if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
            throw std::runtime_error("vkCreateDescriptorPool failed");
        }

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = descriptorPool_;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &computeDescriptorSetLayout_;

        if (vkAllocateDescriptorSets(device_, &allocInfo, &computeDescriptorSet_) != VK_SUCCESS) {
            throw std::runtime_error("vkAllocateDescriptorSets failed");
        }

        // Point the descriptor at the particle buffer.
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = particleBuffer_;
        bufferInfo.offset = 0;
        bufferInfo.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = computeDescriptorSet_;
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(device_, 1, &descriptorWrite, 0, nullptr);
    }

    // Build the compute pipeline: load SPIR-V, create the pipeline layout
    // (with one descriptor set + push constants), then create the pipeline.
    void CreateComputePipeline() {
        const auto compCode = ReadFile(shaderDir_ + "compute.comp.spv");
        VkShaderModule compModule = CreateShaderModule(compCode);

        VkPipelineShaderStageCreateInfo stageInfo{};
        stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stageInfo.module = compModule;
        stageInfo.pName = "main";

        // Push constant range for ComputeParams (deltaTime + bounds).
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(ComputeParams);

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &computeDescriptorSetLayout_;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;

        if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &computePipelineLayout_) !=
            VK_SUCCESS) {
            throw std::runtime_error("vkCreatePipelineLayout failed for compute");
        }

        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage = stageInfo;
        pipelineInfo.layout = computePipelineLayout_;

        if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                     &computePipeline_) != VK_SUCCESS) {
            throw std::runtime_error("vkCreateComputePipelines failed");
        }

        vkDestroyShaderModule(device_, compModule, nullptr);
    }

    // Prefer SRGB colour space with B8G8R8A8 format for correct gamma-aware
    // blending.  Falls back to the first available format if SRGB is absent.
    VkSurfaceFormatKHR ChooseSwapSurfaceFormat(
        const std::vector<VkSurfaceFormatKHR>& availableFormats) const {
        for (const auto& availableFormat : availableFormats) {
            if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB &&
                availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return availableFormat;
            }
        }

        return availableFormats[0];
    }

    // MAILBOX is preferred: triple-buffered, low-latency presentation that
    // replaces queued frames with newer ones.  Falls back to FIFO (vsync),
    // which is guaranteed by the spec on all conformant drivers.
    VkPresentModeKHR ChooseSwapPresentMode(
        const std::vector<VkPresentModeKHR>& availablePresentModes) const {
        for (const auto& availablePresentMode : availablePresentModes) {
            if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
                return availablePresentMode;
            }
        }

        return VK_PRESENT_MODE_FIFO_KHR;
    }

    // If the surface reports a fixed extent, use it directly.  A width of
    // UINT32_MAX signals that the surface size is determined by the swapchain
    // extent; in that case, query the framebuffer size from GLFW and clamp to
    // the surface's min/max range.
    VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) const {
        if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
            return capabilities.currentExtent;
        }

        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window_, &width, &height);

        VkExtent2D actualExtent = {
            static_cast<std::uint32_t>(width),
            static_cast<std::uint32_t>(height),
        };

        actualExtent.width = std::clamp(
            actualExtent.width,
            capabilities.minImageExtent.width,
            capabilities.maxImageExtent.width);
        actualExtent.height = std::clamp(
            actualExtent.height,
            capabilities.minImageExtent.height,
            capabilities.maxImageExtent.height);

        return actualExtent;
    }

    void CreateSwapChain() {
        const SwapChainSupportDetails swapChainSupport = QuerySwapChainSupport(physicalDevice_);

        const VkSurfaceFormatKHR surfaceFormat = ChooseSwapSurfaceFormat(swapChainSupport.formats);
        const VkPresentModeKHR presentMode = ChooseSwapPresentMode(swapChainSupport.presentModes);
        const VkExtent2D extent = ChooseSwapExtent(swapChainSupport.capabilities);

        std::uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;
        if (swapChainSupport.capabilities.maxImageCount > 0 &&
            imageCount > swapChainSupport.capabilities.maxImageCount) {
            imageCount = swapChainSupport.capabilities.maxImageCount;
        }

        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = surface_;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        const QueueFamilyIndices indices = FindQueueFamilies(physicalDevice_);
        const std::uint32_t queueFamilyIndices[] = {
            indices.graphicsFamily.value(),
            indices.presentFamily.value(),
        };

        // If graphics and present queues belong to different families we must
        // use CONCURRENT sharing mode so both families can access the images
        // without explicit ownership transfers.  EXCLUSIVE is faster but only
        // valid when both queues share the same family.
        if (indices.graphicsFamily != indices.presentFamily) {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = queueFamilyIndices;
        } else {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }

        createInfo.preTransform = swapChainSupport.capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = VK_NULL_HANDLE;

        if (vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapChain_) != VK_SUCCESS) {
            throw std::runtime_error("vkCreateSwapchainKHR failed");
        }

        vkGetSwapchainImagesKHR(device_, swapChain_, &imageCount, nullptr);
        swapChainImages_.resize(imageCount);
        vkGetSwapchainImagesKHR(device_, swapChain_, &imageCount, swapChainImages_.data());

        swapChainImageFormat_ = surfaceFormat.format;
        swapChainExtent_ = extent;
    }

    void CreateImageViews() {
        swapChainImageViews_.resize(swapChainImages_.size());

        for (std::size_t i = 0; i < swapChainImages_.size(); ++i) {
            VkImageViewCreateInfo createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            createInfo.image = swapChainImages_[i];
            createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            createInfo.format = swapChainImageFormat_;
            createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            createInfo.subresourceRange.baseMipLevel = 0;
            createInfo.subresourceRange.levelCount = 1;
            createInfo.subresourceRange.baseArrayLayer = 0;
            createInfo.subresourceRange.layerCount = 1;

            if (vkCreateImageView(device_, &createInfo, nullptr, &swapChainImageViews_[i]) !=
                VK_SUCCESS) {
                throw std::runtime_error("vkCreateImageView failed");
            }
        }
    }

    // A render pass describes attachment usage, subpass organisation, and
    // memory dependencies.  We have a single colour attachment that is
    // cleared at the start (LOAD_OP_CLEAR) and stored for presentation
    // (STORE_OP_STORE).  The image transitions from UNDEFINED (we don't
    // care about previous contents) to PRESENT_SRC (ready for display).
    void CreateRenderPass() {
        VkAttachmentDescription colourAttachment{};
        colourAttachment.format = swapChainImageFormat_;
        colourAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colourAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colourAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colourAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colourAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colourAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colourAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colourAttachmentRef{};
        colourAttachmentRef.attachment = 0;
        colourAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colourAttachmentRef;

        // The subpass dependency ensures that the colour attachment output
        // stage waits until the swapchain image has been acquired (external
        // dependency).  Without this, the render pass might begin writing
        // to an image that is still being presented.
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &colourAttachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies = &dependency;

        if (vkCreateRenderPass(device_, &renderPassInfo, nullptr, &renderPass_) != VK_SUCCESS) {
            throw std::runtime_error("vkCreateRenderPass failed");
        }
    }

    void CreateFramebuffers() {
        swapChainFramebuffers_.resize(swapChainImageViews_.size());

        for (std::size_t i = 0; i < swapChainImageViews_.size(); ++i) {
            VkImageView attachments[] = {swapChainImageViews_[i]};

            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = renderPass_;
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments = attachments;
            framebufferInfo.width = swapChainExtent_.width;
            framebufferInfo.height = swapChainExtent_.height;
            framebufferInfo.layers = 1;

            if (vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &swapChainFramebuffers_[i]) !=
                VK_SUCCESS) {
                throw std::runtime_error("vkCreateFramebuffer failed");
            }
        }
    }

    // The command pool manages memory for command buffers.  RESET_COMMAND_BUFFER_BIT
    // allows individual buffers to be reset and re-recorded (needed for per-frame
    // recording once compute updates are added).
    void CreateCommandPool() {
        const QueueFamilyIndices queueFamilyIndices = FindQueueFamilies(physicalDevice_);

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();

        if (vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_) != VK_SUCCESS) {
            throw std::runtime_error("vkCreateCommandPool failed");
        }
    }

    // Allocate one command buffer per swapchain image.  Recording is deferred
    // to RecordCommandBuffer() and happens every frame so the compute dispatch
    // can inject the current deltaTime via push constants.
    void CreateCommandBuffers() {
        commandBuffers_.resize(swapChainFramebuffers_.size());

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool_;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = static_cast<std::uint32_t>(commandBuffers_.size());

        if (vkAllocateCommandBuffers(device_, &allocInfo, commandBuffers_.data()) != VK_SUCCESS) {
            throw std::runtime_error("vkAllocateCommandBuffers failed");
        }
    }

    // Record a single frame's command buffer:
    //   1. Bind the compute pipeline, push deltaTime, dispatch particle update.
    //   2. Insert a buffer memory barrier (compute write -> vertex attribute read)
    //      so the rasteriser sees the updated positions.
    //   3. Begin the render pass, bind the graphics pipeline, draw, end pass.
    void RecordCommandBuffer(std::uint32_t imageIndex, float deltaTime) {
        VkCommandBuffer cmd = commandBuffers_[imageIndex];

        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
            throw std::runtime_error("vkBeginCommandBuffer failed");
        }

        // Timestamp query indices for the current frame slot.
        const std::uint32_t tsBase = currentFrame_ * kTimestampsPerFrame;
        if (timestampsSupported_) {
            vkCmdResetQueryPool(cmd, timestampQueryPool_, tsBase, kTimestampsPerFrame);
        }

        // --- Timestamp 0: before compute dispatch ---
        if (timestampsSupported_) {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                timestampQueryPool_, tsBase + 0);
        }

        // --- Compute pass: update particle positions ---
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                computePipelineLayout_, 0, 1,
                                &computeDescriptorSet_, 0, nullptr);

        ComputeParams params{deltaTime, 0.9f};
        vkCmdPushConstants(cmd, computePipelineLayout_,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(ComputeParams), &params);

        vkCmdDispatch(cmd, kParticleCount / kComputeWorkGroupSize, 1, 1);

        // --- Timestamp 1: after compute dispatch ---
        if (timestampsSupported_) {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                timestampQueryPool_, tsBase + 1);
        }

        // --- Barrier: compute shader write -> vertex attribute read ---
        VkBufferMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = particleBuffer_;
        barrier.offset = 0;
        barrier.size = VK_WHOLE_SIZE;

        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
            0,
            0, nullptr,
            1, &barrier,
            0, nullptr);

        // --- Timestamp 2: before render pass ---
        if (timestampsSupported_) {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                timestampQueryPool_, tsBase + 2);
        }

        // --- Graphics pass: render the particles ---
        VkClearValue clearColour = {{{0.04f, 0.08f, 0.14f, 1.0f}}};

        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = renderPass_;
        renderPassInfo.framebuffer = swapChainFramebuffers_[imageIndex];
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = swapChainExtent_;
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues = &clearColour;

        vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline_);

        VkBuffer vertexBuffers[] = {particleBuffer_};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);

        vkCmdDraw(cmd, kParticleCount, 1, 0, 0);

        vkCmdEndRenderPass(cmd);

        // --- Timestamp 3: after render pass ---
        if (timestampsSupported_) {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                timestampQueryPool_, tsBase + 3);
        }

        if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
            throw std::runtime_error("vkEndCommandBuffer failed");
        }
    }

    // Per-frame synchronisation primitives:
    //   imageAvailableSemaphore  - signalled when the swapchain image is ready
    //   renderFinishedSemaphore  - signalled when rendering is complete
    //   inFlightFence            - CPU-side wait to avoid overwriting a frame
    //                              that the GPU is still processing
    // Fences are created SIGNALLED so the first WaitForFences in DrawFrame
    // returns immediately rather than blocking forever.
    void CreateSyncObjects() {
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
            if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailableSemaphores_[i]) !=
                    VK_SUCCESS ||
                vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &renderFinishedSemaphores_[i]) !=
                    VK_SUCCESS ||
                vkCreateFence(device_, &fenceInfo, nullptr, &inFlightFences_[i]) != VK_SUCCESS) {
                throw std::runtime_error("Failed to create synchronisation objects");
            }
        }
    }

    // Creates a pool of timestamp queries used to measure how long
    // individual GPU workloads take.  Each frame writes 4 timestamps
    // (before/after compute, before/after render), so the pool holds
    // kTimestampsPerFrame * kMaxFramesInFlight entries.
    //
    // If the chosen queue family does not support timestamps
    // (timestampValidBits == 0), profiling is silently disabled.
    void CreateTimestampQueryPool() {
        const QueueFamilyIndices indices = FindQueueFamilies(physicalDevice_);

        std::uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount, queueFamilies.data());

        const auto gfxIdx = indices.graphicsFamily.value();
        if (queueFamilies[gfxIdx].timestampValidBits == 0) {
            std::cout << "[Profiling] Graphics queue family does not support timestamps – profiling disabled." << std::endl;
            return;
        }

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physicalDevice_, &props);
        timestampPeriodNs_ = props.limits.timestampPeriod;

        VkQueryPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        poolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        poolInfo.queryCount = kTimestampsPerFrame * kMaxFramesInFlight;

        if (vkCreateQueryPool(device_, &poolInfo, nullptr, &timestampQueryPool_) != VK_SUCCESS) {
            std::cerr << "[Profiling] Failed to create timestamp query pool – profiling disabled.\n";
            return;
        }

        timestampsSupported_ = true;
        std::cout << "[Profiling] Timestamp queries enabled  (period = "
                  << timestampPeriodNs_ << " ns/tick)" << std::endl;
    }

    // Reads back the 4 timestamp values for the given frame slot, converts
    // the deltas into milliseconds, and accumulates them.
    // Uses non-blocking readback (no VK_QUERY_RESULT_WAIT_BIT) so that the
    // very first invocation — before any timestamps have been written —
    // returns VK_NOT_READY instead of hanging.
    void CollectTimestampResults(std::uint32_t frameSlot) {
        if (!timestampsSupported_) return;

        const std::uint32_t firstQuery = frameSlot * kTimestampsPerFrame;
        std::array<std::uint64_t, kTimestampsPerFrame> timestamps{};

        const VkResult res = vkGetQueryPoolResults(
            device_,
            timestampQueryPool_,
            firstQuery,
            kTimestampsPerFrame,
            sizeof(timestamps),
            timestamps.data(),
            sizeof(std::uint64_t),
            VK_QUERY_RESULT_64_BIT);

        if (res != VK_SUCCESS) return;

        const double toMs = static_cast<double>(timestampPeriodNs_) / 1'000'000.0;
        const double computeMs = static_cast<double>(timestamps[1] - timestamps[0]) * toMs;
        const double renderMs  = static_cast<double>(timestamps[3] - timestamps[2]) * toMs;
        const double totalMs   = static_cast<double>(timestamps[3] - timestamps[0]) * toMs;

        accumComputeMs_  += computeMs;
        accumRenderMs_   += renderMs;
        accumTotalGpuMs_ += totalMs;
        ++timingSampleCount_;
    }

    // Prints a one-line summary of averaged GPU timings to stdout and
    // updates the window title with the current FPS.
    void ReportTimingIfDue(double deltaTime) {
        timingReportTimer_ += deltaTime;
        ++frameCount_;

        if (timingReportTimer_ >= kTimingReportIntervalSec) {
            const double fps = static_cast<double>(frameCount_) / timingReportTimer_;

            if (timestampsSupported_ && timingSampleCount_ > 0) {
                const double avgCompute = accumComputeMs_ / timingSampleCount_;
                const double avgRender  = accumRenderMs_  / timingSampleCount_;
                const double avgTotal   = accumTotalGpuMs_ / timingSampleCount_;

                std::cout << "[GPU Timing] Compute: " << std::fixed << std::setprecision(3)
                          << avgCompute << " ms | Render: " << avgRender
                          << " ms | Total GPU: " << avgTotal
                          << " ms | FPS: " << static_cast<int>(fps) << std::endl;
            } else {
                std::cout << "[FPS] " << static_cast<int>(fps) << std::endl;
            }

            std::string title = "Vulkan Particle Sim  |  FPS: " + std::to_string(static_cast<int>(fps));
            if (timestampsSupported_ && timingSampleCount_ > 0) {
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(2)
                    << "  |  GPU: " << (accumTotalGpuMs_ / timingSampleCount_) << " ms";
                title += oss.str();
            }
            glfwSetWindowTitle(window_, title.c_str());

            accumComputeMs_   = 0.0;
            accumRenderMs_    = 0.0;
            accumTotalGpuMs_  = 0.0;
            timingSampleCount_ = 0;
            frameCount_       = 0;
            timingReportTimer_ = 0.0;
        }
    }

    // Per-frame workflow:
    //   1. Compute delta time from the previous frame.
    //   2. Wait on the in-flight fence (CPU-side throttle).
    //   3. Collect timestamp results from the previous submission of this slot.
    //   4. Acquire the next swapchain image.
    //   5. Record the command buffer (compute dispatch + barrier + draw).
    //   6. Submit to the graphics queue.
    //   7. Present the rendered image.
    //   8. Report averaged GPU timing once per second.
    //   9. Advance the frame index.
    void DrawFrame() {
        const double currentTime = glfwGetTime();
        const auto deltaTime = static_cast<float>(currentTime - lastFrameTime_);
        lastFrameTime_ = currentTime;

        vkWaitForFences(device_, 1, &inFlightFences_[currentFrame_], VK_TRUE, UINT64_MAX);

        // The fence guarantees the previous submission for this slot has
        // completed, so its timestamp results are now safe to read back.
        CollectTimestampResults(currentFrame_);

        std::uint32_t imageIndex = 0;
        const VkResult acquireResult = vkAcquireNextImageKHR(
            device_,
            swapChain_,
            UINT64_MAX,
            imageAvailableSemaphores_[currentFrame_],
            VK_NULL_HANDLE,
            &imageIndex);

        if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
            return;
        }
        if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
            throw std::runtime_error("vkAcquireNextImageKHR failed");
        }

        vkResetFences(device_, 1, &inFlightFences_[currentFrame_]);

        RecordCommandBuffer(imageIndex, deltaTime);

        // The submit waits at the colour-attachment-output stage until the
        // swapchain image is available; this lets earlier pipeline stages
        // (e.g. vertex shading) proceed in parallel with image acquisition.
        VkSemaphore waitSemaphores[] = {imageAvailableSemaphores_[currentFrame_]};
        VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        VkSemaphore signalSemaphores[] = {renderFinishedSemaphores_[currentFrame_]};

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffers_[imageIndex];
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;

        if (vkQueueSubmit(graphicsQueue_, 1, &submitInfo, inFlightFences_[currentFrame_]) !=
            VK_SUCCESS) {
            throw std::runtime_error("vkQueueSubmit failed");
        }

        VkSwapchainKHR swapChains[] = {swapChain_};
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapChains;
        presentInfo.pImageIndices = &imageIndex;

        const VkResult presentResult = vkQueuePresentKHR(presentQueue_, &presentInfo);
        if (presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR &&
            presentResult != VK_ERROR_OUT_OF_DATE_KHR) {
            throw std::runtime_error("vkQueuePresentKHR failed");
        }

        ReportTimingIfDue(static_cast<double>(deltaTime));

        currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
    }

    void MainLoop() {
        lastFrameTime_ = glfwGetTime();

        while (glfwWindowShouldClose(window_) == GLFW_FALSE) {
            glfwPollEvents();
            DrawFrame();
        }

        vkDeviceWaitIdle(device_);
    }

    void CleanupSwapChain() {
        for (const auto framebuffer : swapChainFramebuffers_) {
            vkDestroyFramebuffer(device_, framebuffer, nullptr);
        }
        swapChainFramebuffers_.clear();

        for (const auto imageView : swapChainImageViews_) {
            vkDestroyImageView(device_, imageView, nullptr);
        }
        swapChainImageViews_.clear();

        if (renderPass_ != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device_, renderPass_, nullptr);
            renderPass_ = VK_NULL_HANDLE;
        }

        if (swapChain_ != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device_, swapChain_, nullptr);
            swapChain_ = VK_NULL_HANDLE;
        }
    }

    // Vulkan resources must be destroyed in roughly the reverse order of
    // creation.  Synchronisation objects and command pools first (they
    // reference the device), then buffers and pipelines, then swapchain
    // resources, and finally the device, surface, and instance.
    void Cleanup() {
        for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
            if (imageAvailableSemaphores_[i] != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, imageAvailableSemaphores_[i], nullptr);
            }
            if (renderFinishedSemaphores_[i] != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, renderFinishedSemaphores_[i], nullptr);
            }
            if (inFlightFences_[i] != VK_NULL_HANDLE) {
                vkDestroyFence(device_, inFlightFences_[i], nullptr);
            }
        }

        if (timestampQueryPool_ != VK_NULL_HANDLE) {
            vkDestroyQueryPool(device_, timestampQueryPool_, nullptr);
            timestampQueryPool_ = VK_NULL_HANDLE;
        }

        if (commandPool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, commandPool_, nullptr);
            commandPool_ = VK_NULL_HANDLE;
        }

        if (particleBuffer_ != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, particleBuffer_, nullptr);
            particleBuffer_ = VK_NULL_HANDLE;
        }
        if (particleBufferMemory_ != VK_NULL_HANDLE) {
            vkFreeMemory(device_, particleBufferMemory_, nullptr);
            particleBufferMemory_ = VK_NULL_HANDLE;
        }

        if (computePipeline_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_, computePipeline_, nullptr);
            computePipeline_ = VK_NULL_HANDLE;
        }
        if (computePipelineLayout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device_, computePipelineLayout_, nullptr);
            computePipelineLayout_ = VK_NULL_HANDLE;
        }
        if (descriptorPool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
            descriptorPool_ = VK_NULL_HANDLE;
        }
        if (computeDescriptorSetLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_, computeDescriptorSetLayout_, nullptr);
            computeDescriptorSetLayout_ = VK_NULL_HANDLE;
        }

        if (graphicsPipeline_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_, graphicsPipeline_, nullptr);
            graphicsPipeline_ = VK_NULL_HANDLE;
        }
        if (graphicsPipelineLayout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device_, graphicsPipelineLayout_, nullptr);
            graphicsPipelineLayout_ = VK_NULL_HANDLE;
        }

        CleanupSwapChain();

        if (device_ != VK_NULL_HANDLE) {
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }

        if (surface_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }

        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }

        if (window_ != nullptr) {
            glfwDestroyWindow(window_);
            window_ = nullptr;
        }

        glfwTerminate();
    }
};

}  // namespace

// Extract the directory containing the executable so SPIR-V shader files
// (placed next to the exe by the CMake post-build step) can be located
// regardless of the working directory.
std::string ExeDirectory(const char* argv0) {
    std::string path(argv0);
    const auto pos = path.find_last_of("\\/");
    return pos != std::string::npos ? path.substr(0, pos + 1) : "";
}

int main(int argc, char* argv[]) {
    std::int32_t gpuIndex = -1;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--gpu") == 0 && i + 1 < argc) {
            gpuIndex = std::stoi(argv[++i]);
        }
    }

    const std::string shaderDir = ExeDirectory(argv[0]);

    try {
        VulkanApp app(gpuIndex, shaderDir);
        app.Run();
    } catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
