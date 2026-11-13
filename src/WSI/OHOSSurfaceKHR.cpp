// QEMU CPU NativeWindow presentation bridge. Apache-2.0.
// Use the SDK public APIs rather than depending on private SurfaceBuffer ABI.
#include "OHOSSurfaceKHR.hpp"
#include "OHOSPresentCopy.hpp"
#include "Vulkan/VkImage.hpp"

#include <native_window/external_window.h>
#include <native_buffer/native_buffer.h>
#include <dlfcn.h>
#include <cerrno>
#include <poll.h>
#include <unistd.h>

namespace {
template<class T>
T symbol(void *library, const char *name)
{
	return library ? reinterpret_cast<T>(dlsym(library, name)) : nullptr;
}

struct NativeAPI
{
	// Kept open for the ICD lifetime, including musl's atexit callbacks.
	void *windowLib = dlopen("libnative_window.so", RTLD_NOW | RTLD_LOCAL);
	void *bufferLib = dlopen("libnative_buffer.so", RTLD_NOW | RTLD_LOCAL);
	decltype(&OH_NativeWindow_NativeObjectReference) reference = symbol<decltype(reference)>(windowLib, "OH_NativeWindow_NativeObjectReference");
	decltype(&OH_NativeWindow_NativeObjectUnreference) unreference = symbol<decltype(unreference)>(windowLib, "OH_NativeWindow_NativeObjectUnreference");
	decltype(&OH_NativeWindow_NativeWindowHandleOpt) options = symbol<decltype(options)>(windowLib, "OH_NativeWindow_NativeWindowHandleOpt");
	decltype(&OH_NativeWindow_NativeWindowRequestBuffer) request = symbol<decltype(request)>(windowLib, "OH_NativeWindow_NativeWindowRequestBuffer");
	decltype(&OH_NativeWindow_NativeWindowFlushBuffer) flush = symbol<decltype(flush)>(windowLib, "OH_NativeWindow_NativeWindowFlushBuffer");
	decltype(&OH_NativeWindow_NativeWindowAbortBuffer) abort = symbol<decltype(abort)>(windowLib, "OH_NativeWindow_NativeWindowAbortBuffer");
	decltype(&OH_NativeBuffer_FromNativeWindowBuffer) fromWindow = symbol<decltype(fromWindow)>(bufferLib, "OH_NativeBuffer_FromNativeWindowBuffer");
	decltype(&OH_NativeBuffer_Map) map = symbol<decltype(map)>(bufferLib, "OH_NativeBuffer_Map");
	decltype(&OH_NativeBuffer_Unmap) unmap = symbol<decltype(unmap)>(bufferLib, "OH_NativeBuffer_Unmap");
	decltype(&OH_NativeBuffer_GetConfig) config = symbol<decltype(config)>(bufferLib, "OH_NativeBuffer_GetConfig");

	bool available() const
	{
		return reference && unreference && options && request && flush && abort &&
		       fromWindow && map && unmap && config;
	}
};

NativeAPI &nativeAPI()
{
	static NativeAPI api;
	return api;
}

bool waitFence(int fd)
{
	if(fd < 0) { return true; }
	pollfd fence = { fd, POLLIN, 0 };
	// A stuck consumer must not hang vkQueuePresentKHR indefinitely. EINTR
	// is a failed present too; the caller can recreate the surface.
	return poll(&fence, 1, 3000) > 0 &&
	       (fence.revents & POLLIN) && !(fence.revents & (POLLERR | POLLNVAL));
}
}  // namespace

namespace vk {
bool OHOSSurfaceKHR::isSupported()
{
	return nativeAPI().available();
}

OHOSSurfaceKHR::OHOSSurfaceKHR(const VkSurfaceCreateInfoOHOS *info, void *mem)
    : window(info->window)
{
	auto &api = nativeAPI();
	referenced = api.reference(window) == 0;
	if(!referenced) { return; }
	uint64_t usage = 0;
	ready = api.options(window, GET_USAGE, &usage) == 0 &&
	        api.options(window, SET_USAGE, usage | NATIVEBUFFER_USAGE_CPU_READ |
	                    NATIVEBUFFER_USAGE_CPU_WRITE) == 0 &&
	        api.options(window, SET_FORMAT, NATIVEBUFFER_PIXEL_FMT_RGBA_8888) == 0;
}

void OHOSSurfaceKHR::destroySurface(const VkAllocationCallbacks *)
{
	if(referenced)
	{
		nativeAPI().unreference(window);
		referenced = false;
	}
}

bool OHOSSurfaceKHR::getExtent(VkExtent2D *extent) const
{
	int32_t height = 0, width = 0;
	if(!ready || nativeAPI().options(window, GET_BUFFER_GEOMETRY, &height, &width) != 0 ||
	   width <= 0 || height <= 0)
	{
		return false;
	}
	*extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
	return true;
}

VkResult OHOSSurfaceKHR::getSurfaceCapabilities(const void *next, VkSurfaceCapabilitiesKHR *caps, void *outNext) const
{
	std::lock_guard<std::mutex> lock(mutex);
	if(!getExtent(&caps->currentExtent)) { return VK_ERROR_SURFACE_LOST_KHR; }
	caps->minImageExtent = { 1, 1 };
	caps->maxImageExtent = caps->currentExtent;
	SetCommonSurfaceCapabilities(next, caps, outNext);
	return VK_SUCCESS;
}

VkResult OHOSSurfaceKHR::present(PresentImage *image)
{
	std::lock_guard<std::mutex> lock(mutex);
	VkExtent2D extent;
	if(!getExtent(&extent)) { return VK_ERROR_SURFACE_LOST_KHR; }
	const auto *sourceImage = image->getImage();
	const auto &size = sourceImage->getExtent();
	if(extent.width != size.width || extent.height != size.height)
	{
		return VK_ERROR_OUT_OF_DATE_KHR;
	}
	const VkFormat format = sourceImage->getFormat();
	if(format != VK_FORMAT_B8G8R8A8_UNORM && format != VK_FORMAT_B8G8R8A8_SRGB)
	{
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	auto &api = nativeAPI();
	OHNativeWindowBuffer *buffer = nullptr;
	int fd = -1;
	if(api.request(window, &buffer, &fd) != 0 || !buffer)
	{
		if(fd >= 0) { close(fd); }
		return VK_ERROR_SURFACE_LOST_KHR;
	}
	const bool signaled = waitFence(fd);
	if(fd >= 0) { close(fd); }
	if(!signaled)
	{
		api.abort(window, buffer);
		return VK_ERROR_SURFACE_LOST_KHR;
	}
	OH_NativeBuffer *nativeBuffer = nullptr;
	void *address = nullptr;
	if(api.fromWindow(buffer, &nativeBuffer) != 0 || !nativeBuffer ||
	   api.map(nativeBuffer, &address) != 0)
	{
		api.abort(window, buffer);
		return VK_ERROR_SURFACE_LOST_KHR;
	}
	OH_NativeBuffer_Config config = {};
	api.config(nativeBuffer, &config);
	const bool geometryMatches = config.width > 0 && config.height > 0 &&
	                            static_cast<uint32_t>(config.width) == size.width &&
	                            static_cast<uint32_t>(config.height) == size.height;
	const bool copied = geometryMatches && config.stride > 0 &&
	                    config.format == NATIVEBUFFER_PIXEL_FMT_RGBA_8888 &&
	                    CopyBGRA8ToRGBA8(
	                        static_cast<const uint8_t *>(sourceImage->getTexelPointer(
	                            { 0, 0, 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 })),
	                        sourceImage->rowPitchBytes(VK_IMAGE_ASPECT_COLOR_BIT, 0),
	                        static_cast<uint8_t *>(address), static_cast<size_t>(config.stride),
	                        size.width, size.height);
	const int unmapResult = api.unmap(nativeBuffer);
	if(!copied || unmapResult != 0)
	{
		api.abort(window, buffer);
		return geometryMatches ? VK_ERROR_SURFACE_LOST_KHR : VK_ERROR_OUT_OF_DATE_KHR;
	}
	// CPU copy has completed; no release fence is needed. An empty Region
	// asks NativeWindow to mark the entire buffer dirty.
	if(api.flush(window, buffer, -1, { nullptr, 0 }) != 0)
	{
		api.abort(window, buffer);
		return VK_ERROR_SURFACE_LOST_KHR;
	}
	return VK_SUCCESS;
}
}  // namespace vk
