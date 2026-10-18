/*
 * Wayland window buffers the compositor's GPU reads in place
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#include <vector>

#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include "linux-dmabuf-v1-client-protocol.h"

#include "dmabuf_allocator.h"

/* from drm_fourcc.h */
static const uint32_t kFormatArgb8888 = 0x34325241;
static const uint64_t kModifierLinear = 0;
/* row pitch alignment GPUs take for linear textures */
static const size_t kPitchAlign = 256;


namespace {

struct DmabufBuffer final: WindowBuffer {
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer vk_buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;

    ~DmabufBuffer() override
    {
        if (vk_buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, vk_buffer, nullptr);
        if (memory != VK_NULL_HANDLE)
            vkFreeMemory(device, memory, nullptr);
    }
};


/* The compositor's requests and answers go through a queue of their own,
   so that waiting for one dispatches nothing else. */
class DmabufAllocator final: public BufferAllocator {
private:
    struct FormatEntry {
        uint32_t format;
        uint32_t pad;
        uint64_t modifier;
    };

    wl_display *fDisplay;
    wl_event_queue *fQueue = nullptr;
    zwp_linux_dmabuf_v1 *fDmabuf = nullptr;

    /* the compositor's default feedback */
    const FormatEntry *fTable = nullptr;
    size_t fTableSize = 0;
    dev_t fMainDevice = 0;
    bool fHaveMainDevice = false;
    bool fLinearArgb = false;
    bool fFeedbackDone = false;

    /* the answer to a create request */
    bool fCreateDone = false;
    wl_buffer *fCreated = nullptr;

    VkInstance fInstance = VK_NULL_HANDLE;
    VkPhysicalDevice fPhysicalDevice = VK_NULL_HANDLE;
    VkDevice fDevice = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties fMemoryProperties {};
    PFN_vkGetMemoryFdKHR fGetMemoryFd = nullptr;

    static const zwp_linux_dmabuf_feedback_v1_listener kFeedbackListener;
    static const zwp_linux_buffer_params_v1_listener kParamsListener;

    bool Bind(wl_registry *registry, uint32_t name);
    bool OpenDevice();
    int MemoryType(uint32_t type_bits) const;
    wl_buffer *Import(int fd, int width, int height, uint32_t stride);

public:
    explicit DmabufAllocator(wl_display *display): fDisplay(display) {}
    ~DmabufAllocator() override;

    /* false, having said why, when the buffers can't be used */
    bool Init(wl_registry *registry, uint32_t name, uint32_t version);

    WindowBuffer *Create(int width, int height) override;
};


static bool fail(const char *reason)
{
    fprintf(stderr, "Window buffers in shared memory: %s\n", reason);
    return false;
}


DmabufAllocator::~DmabufAllocator()
{
    if (fDevice != VK_NULL_HANDLE)
        vkDestroyDevice(fDevice, nullptr);
    if (fInstance != VK_NULL_HANDLE)
        vkDestroyInstance(fInstance, nullptr);
    if (fTable != nullptr)
        munmap(const_cast<FormatEntry *>(fTable), fTableSize);
    if (fDmabuf != nullptr)
        zwp_linux_dmabuf_v1_destroy(fDmabuf);
    if (fQueue != nullptr)
        wl_event_queue_destroy(fQueue);
}


const zwp_linux_dmabuf_feedback_v1_listener
DmabufAllocator::kFeedbackListener = {
    .done = [](void *data, zwp_linux_dmabuf_feedback_v1 *) {
        static_cast<DmabufAllocator *>(data)->fFeedbackDone = true;
    },
    .format_table = [](void *data, zwp_linux_dmabuf_feedback_v1 *,
                       int32_t fd, uint32_t size) {
        auto *allocator = static_cast<DmabufAllocator *>(data);
        if (allocator->fTable != nullptr) {
            munmap(const_cast<FormatEntry *>(allocator->fTable),
                   allocator->fTableSize);
            allocator->fTable = nullptr;
        }
        void *table = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (table != MAP_FAILED) {
            allocator->fTable = static_cast<const FormatEntry *>(table);
            allocator->fTableSize = size;
        }
    },
    .main_device = [](void *data, zwp_linux_dmabuf_feedback_v1 *,
                      wl_array *device) {
        auto *allocator = static_cast<DmabufAllocator *>(data);
        if (device->size != sizeof(dev_t))
            return;
        memcpy(&allocator->fMainDevice, device->data, sizeof(dev_t));
        allocator->fHaveMainDevice = true;
    },
    .tranche_done = [](void *, zwp_linux_dmabuf_feedback_v1 *) {},
    .tranche_target_device = [](void *, zwp_linux_dmabuf_feedback_v1 *,
                                wl_array *) {},
    .tranche_formats = [](void *data, zwp_linux_dmabuf_feedback_v1 *,
                          wl_array *indices) {
        auto *allocator = static_cast<DmabufAllocator *>(data);
        size_t count = allocator->fTableSize / sizeof(FormatEntry);
        const uint16_t *index = static_cast<const uint16_t *>(indices->data);
        for (size_t i = 0; i < indices->size / sizeof(uint16_t); i++) {
            if (index[i] >= count)
                continue;
            const FormatEntry &entry = allocator->fTable[index[i]];
            if (entry.format == kFormatArgb8888 &&
                entry.modifier == kModifierLinear)
                allocator->fLinearArgb = true;
        }
    },
    .tranche_flags = [](void *, zwp_linux_dmabuf_feedback_v1 *, uint32_t) {},
};


const zwp_linux_buffer_params_v1_listener DmabufAllocator::kParamsListener = {
    .created = [](void *data, zwp_linux_buffer_params_v1 *,
                  wl_buffer *buffer) {
        auto *allocator = static_cast<DmabufAllocator *>(data);
        allocator->fCreated = buffer;
        allocator->fCreateDone = true;
    },
    .failed = [](void *data, zwp_linux_buffer_params_v1 *) {
        auto *allocator = static_cast<DmabufAllocator *>(data);
        allocator->fCreated = nullptr;
        allocator->fCreateDone = true;
    },
};


/* Binds the global on the private queue and reads which device and
   formats the compositor takes. */
bool DmabufAllocator::Bind(wl_registry *registry, uint32_t name)
{
    fQueue = wl_display_create_queue(fDisplay);
    if (fQueue == nullptr)
        return fail("no event queue");
    auto *wrapper = static_cast<wl_registry *>(wl_proxy_create_wrapper(
        registry));
    if (wrapper == nullptr)
        return fail("no event queue");
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapper), fQueue);
    fDmabuf = static_cast<zwp_linux_dmabuf_v1 *>(wl_registry_bind(
        wrapper, name, &zwp_linux_dmabuf_v1_interface, 4));
    wl_proxy_wrapper_destroy(wrapper);

    zwp_linux_dmabuf_feedback_v1 *feedback =
        zwp_linux_dmabuf_v1_get_default_feedback(fDmabuf);
    zwp_linux_dmabuf_feedback_v1_add_listener(feedback, &kFeedbackListener,
                                              this);
    while (!fFeedbackDone) {
        if (wl_display_roundtrip_queue(fDisplay, fQueue) < 0)
            break;
    }
    zwp_linux_dmabuf_feedback_v1_destroy(feedback);
    if (!fFeedbackDone)
        return fail("no dma-buf feedback from the compositor");
    if (!fHaveMainDevice)
        return fail("the compositor names no GPU");
    if (!fLinearArgb)
        return fail("the compositor takes no linear ARGB8888 dma-bufs");
    return true;
}


static bool has_extension(const std::vector<VkExtensionProperties> &list,
                          const char *name)
{
    for (const VkExtensionProperties &e : list) {
        if (strcmp(e.extensionName, name) == 0)
            return true;
    }
    return false;
}


/* The Vulkan device that is the compositor's GPU. */
bool DmabufAllocator::OpenDevice()
{
    VkApplicationInfo app = {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "TinyEMU";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instance_info = {};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    if (vkCreateInstance(&instance_info, nullptr, &fInstance) != VK_SUCCESS) {
        fInstance = VK_NULL_HANDLE;
        return fail("no Vulkan");
    }

    static const char *const kExtensions[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    };
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(fInstance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(fInstance, &count, devices.data());
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(device, &props);
        if (props.apiVersion < VK_API_VERSION_1_1)
            continue;
        uint32_t ext_count = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &ext_count,
                                             nullptr);
        std::vector<VkExtensionProperties> exts(ext_count);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &ext_count,
                                             exts.data());
        if (!has_extension(exts, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME))
            continue;
        bool usable = true;
        for (const char *name : kExtensions)
            usable = usable && has_extension(exts, name);
        if (!usable)
            continue;

        VkPhysicalDeviceDrmPropertiesEXT drm = {};
        drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 props2 = {};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &drm;
        vkGetPhysicalDeviceProperties2(device, &props2);
        if ((drm.hasRender &&
             makedev(drm.renderMajor, drm.renderMinor) == fMainDevice) ||
            (drm.hasPrimary &&
             makedev(drm.primaryMajor, drm.primaryMinor) == fMainDevice)) {
            fPhysicalDevice = device;
            break;
        }
    }
    if (fPhysicalDevice == VK_NULL_HANDLE)
        return fail("no Vulkan device for the compositor's GPU");

    /* buffers need no queue, but a device has one */
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = 0;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info = {};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = sizeof(kExtensions) /
                                        sizeof(kExtensions[0]);
    device_info.ppEnabledExtensionNames = kExtensions;
    if (vkCreateDevice(fPhysicalDevice, &device_info, nullptr, &fDevice) !=
        VK_SUCCESS) {
        fDevice = VK_NULL_HANDLE;
        return fail("could not open the Vulkan device");
    }
    fGetMemoryFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(fDevice, "vkGetMemoryFdKHR"));
    if (fGetMemoryFd == nullptr)
        return fail("no vkGetMemoryFdKHR");
    vkGetPhysicalDeviceMemoryProperties(fPhysicalDevice, &fMemoryProperties);
    return true;
}


/* Host memory the CPU writes at full speed: cached and coherent rather
   than the GPU's own memory, which the CPU reaches through PCI. */
int DmabufAllocator::MemoryType(uint32_t type_bits) const
{
    static const VkMemoryPropertyFlags kWanted[] = {
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
            VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
    };
    for (VkMemoryPropertyFlags wanted : kWanted) {
        for (uint32_t i = 0; i < fMemoryProperties.memoryTypeCount; i++) {
            VkMemoryPropertyFlags flags =
                fMemoryProperties.memoryTypes[i].propertyFlags;
            if ((type_bits & (1u << i)) && (flags & wanted) == wanted &&
                !(flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                return i;
        }
    }
    return -1;
}


/* A wl_buffer for the dma-buf 'fd', which is closed; nullptr when the
   compositor refuses it. */
wl_buffer *DmabufAllocator::Import(int fd, int width, int height,
                                   uint32_t stride)
{
    zwp_linux_buffer_params_v1 *params =
        zwp_linux_dmabuf_v1_create_params(fDmabuf);
    zwp_linux_buffer_params_v1_add_listener(params, &kParamsListener, this);
    zwp_linux_buffer_params_v1_add(params, fd, 0, 0, stride,
                                   kModifierLinear >> 32,
                                   kModifierLinear & 0xffffffff);
    fCreateDone = false;
    fCreated = nullptr;
    zwp_linux_buffer_params_v1_create(params, width, height, kFormatArgb8888,
                                      0);
    while (!fCreateDone) {
        if (wl_display_roundtrip_queue(fDisplay, fQueue) < 0)
            break;
    }
    zwp_linux_buffer_params_v1_destroy(params);
    close(fd);
    /* its release events go with the window's */
    if (fCreated != nullptr)
        wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(fCreated), nullptr);
    return fCreated;
}


WindowBuffer *DmabufAllocator::Create(int width, int height)
{
    size_t stride = ((size_t)width * 4 + kPitchAlign - 1) & ~(kPitchAlign - 1);
    auto *buffer = new DmabufBuffer;
    buffer->device = fDevice;

    VkExternalMemoryBufferCreateInfo external = {};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkBufferCreateInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.pNext = &external;
    buffer_info.size = stride * height;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(fDevice, &buffer_info, nullptr, &buffer->vk_buffer) !=
        VK_SUCCESS) {
        buffer->vk_buffer = VK_NULL_HANDLE;
        delete buffer;
        fail("could not create a Vulkan buffer");
        return nullptr;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(fDevice, buffer->vk_buffer, &req);
    int type = MemoryType(req.memoryTypeBits);
    if (type < 0) {
        delete buffer;
        fail("no host memory for a dma-buf");
        return nullptr;
    }

    VkMemoryDedicatedAllocateInfo dedicated = {};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.buffer = buffer->vk_buffer;
    VkExportMemoryAllocateInfo export_info = {};
    export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_info.pNext = &dedicated;
    export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkMemoryAllocateInfo alloc = {};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.pNext = &export_info;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    void *pixels;
    if (vkAllocateMemory(fDevice, &alloc, nullptr, &buffer->memory) !=
        VK_SUCCESS) {
        buffer->memory = VK_NULL_HANDLE;
        delete buffer;
        fail("could not allocate memory for a dma-buf");
        return nullptr;
    }
    if (vkBindBufferMemory(fDevice, buffer->vk_buffer, buffer->memory, 0) !=
        VK_SUCCESS ||
        vkMapMemory(fDevice, buffer->memory, 0, VK_WHOLE_SIZE, 0, &pixels) !=
        VK_SUCCESS) {
        delete buffer;
        fail("could not map a dma-buf");
        return nullptr;
    }

    VkMemoryGetFdInfoKHR fd_info = {};
    fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fd_info.memory = buffer->memory;
    fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int fd = -1;
    if (fGetMemoryFd(fDevice, &fd_info, &fd) != VK_SUCCESS) {
        delete buffer;
        fail("could not export a dma-buf");
        return nullptr;
    }
    buffer->buffer = Import(fd, width, height, stride);
    if (buffer->buffer == nullptr) {
        delete buffer;
        fail("the compositor refused a dma-buf");
        return nullptr;
    }
    buffer->pixels = static_cast<uint32_t *>(pixels);
    buffer->pitch = stride / 4;
    buffer->width = width;
    buffer->height = height;
    return buffer;
}


bool DmabufAllocator::Init(wl_registry *registry, uint32_t name,
                           uint32_t version)
{
    if (version < 4)
        return fail("zwp_linux_dmabuf_v1 older than version 4");
    if (!Bind(registry, name) || !OpenDevice())
        return false;
    /* the compositor imports the first buffer when it is made */
    WindowBuffer *buffer = Create(16, 16);
    if (buffer == nullptr)
        return false;
    delete buffer;
    return true;
}

} // namespace


std::unique_ptr<BufferAllocator> dmabuf_allocator_create(
    wl_display *display, wl_registry *registry, uint32_t name,
    uint32_t version)
{
    auto allocator = std::make_unique<DmabufAllocator>(display);
    if (!allocator->Init(registry, name, version))
        return nullptr;
    return allocator;
}
