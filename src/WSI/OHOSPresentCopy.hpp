// QEMU CPU presentation bridge. Apache-2.0.
#ifndef SWIFTSHADER_OHOS_PRESENT_COPY_HPP
#define SWIFTSHADER_OHOS_PRESENT_COPY_HPP
#include <cstddef>
#include <cstdint>
#include <limits>

namespace vk {
// SwiftShader's WSI formats are BGRA8; NativeWindow uses RGBA8. Both row
// pitches are bytes, not pixels. Reject invalid layouts before writing.
inline bool CopyBGRA8ToRGBA8(const uint8_t *source, size_t sourceStride,
                            uint8_t *destination, size_t destinationStride,
                            uint32_t width, uint32_t height)
{
	if(!source || !destination || width == 0 || height == 0 ||
	   static_cast<size_t>(width) > std::numeric_limits<size_t>::max() / 4)
	{
		return false;
	}
	const size_t rowBytes = static_cast<size_t>(width) * 4;
	if(sourceStride < rowBytes || destinationStride < rowBytes ||
	   height > std::numeric_limits<size_t>::max() / sourceStride ||
	   height > std::numeric_limits<size_t>::max() / destinationStride)
	{
		return false;
	}
	for(uint32_t y = 0; y < height; ++y)
	{
		const uint8_t *in = source + y * sourceStride;
		uint8_t *out = destination + y * destinationStride;
		for(uint32_t x = 0; x < width; ++x)
		{
			const size_t offset = static_cast<size_t>(x) * 4;
			out[offset + 0] = in[offset + 2];
			out[offset + 1] = in[offset + 1];
			out[offset + 2] = in[offset + 0];
			out[offset + 3] = in[offset + 3];
		}
	}
	return true;
}
}  // namespace vk
#endif
