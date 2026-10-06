#pragma once

#include <cstdint>
#include <string>

// Image file writers (stb_image_write, vendored with GLFW). Rows are expected bottom-up, i.e. row 0 is
// the bottom of the picture - the order both render paths produce - and are flipped on write.
namespace ImageExport {

	// rgba: width * height packed RGBA8 pixels (R in the lowest byte), already tone mapped
	bool SavePNG(const std::string& path, uint32_t width, uint32_t height, const uint32_t* rgba);

	// rgb: width * height * 3 linear (not tone mapped) floats, written as Radiance RGBE
	bool SaveHDR(const std::string& path, uint32_t width, uint32_t height, const float* rgb);

}
