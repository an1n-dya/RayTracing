#include "ImageExport.h"

#define _CRT_SECURE_NO_WARNINGS
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <vector>

namespace ImageExport {

	bool SavePNG(const std::string& path, uint32_t width, uint32_t height, const uint32_t* rgba) {
		std::vector<uint32_t> flipped((size_t)width * height);
		for (uint32_t y = 0; y < height; y++)
			std::copy_n(rgba + (size_t)(height - 1 - y) * width, width, flipped.data() + (size_t)y * width);

		return stbi_write_png(path.c_str(), (int)width, (int)height, 4, flipped.data(), (int)width * 4) != 0;
	}

	bool SaveHDR(const std::string& path, uint32_t width, uint32_t height, const float* rgb) {
		std::vector<float> flipped((size_t)width * height * 3);
		for (uint32_t y = 0; y < height; y++)
			std::copy_n(rgb + (size_t)(height - 1 - y) * width * 3, width * 3, flipped.data() + (size_t)y * width * 3);

		return stbi_write_hdr(path.c_str(), (int)width, (int)height, 3, flipped.data()) != 0;
	}

}
