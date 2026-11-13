// QEMU CPU NativeWindow presentation bridge. Apache-2.0.
#ifndef SWIFTSHADER_OHOS_SURFACE_HPP
#define SWIFTSHADER_OHOS_SURFACE_HPP

#include "VkSurfaceKHR.hpp"
#include <vulkan/vulkan_ohos.h>
#include <mutex>

namespace vk {
class OHOSSurfaceKHR : public SurfaceKHR, public ObjectBase<OHOSSurfaceKHR, VkSurfaceKHR>
{
public:
	static bool isSupported();
	OHOSSurfaceKHR(const VkSurfaceCreateInfoOHOS *info, void *mem);
	static size_t ComputeRequiredAllocationSize(const VkSurfaceCreateInfoOHOS *) { return 0; }
	bool isReady() const { return ready; }
	void destroySurface(const VkAllocationCallbacks *) override;
	VkResult getSurfaceCapabilities(const void *, VkSurfaceCapabilitiesKHR *, void *) const override;
	void attachImage(PresentImage *) override {}
	void detachImage(PresentImage *) override {}
	VkResult present(PresentImage *) override;

private:
	bool getExtent(VkExtent2D *) const;
	OHNativeWindow *window;
	bool referenced = false;
	bool ready = false;
	mutable std::mutex mutex;
};
}  // namespace vk
#endif
