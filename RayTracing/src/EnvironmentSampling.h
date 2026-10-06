#pragma once

#include "EnvironmentMap.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

// Equirectangular environment lookups and importance sampling. GLSL counterpart: shaders/Environment.glsl.
// rotation is in turns (1 = 360 degrees) around +Y.
namespace EnvironmentSampling {

	constexpr float Pi = 3.14159265358979f;

	inline glm::vec2 DirectionToUV(const glm::vec3& direction, float rotation) {
		float u = std::atan2(direction.z, direction.x) / (2.0f * Pi) + 0.5f - rotation;
		u -= std::floor(u);
		float v = std::acos(glm::clamp(direction.y, -1.0f, 1.0f)) / Pi;
		return { u, v };
	}

	inline glm::vec3 UVToDirection(const glm::vec2& uv, float rotation) {
		float phi = 2.0f * Pi * (uv.x - 0.5f + rotation);
		float theta = Pi * uv.y;
		return { std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi) };
	}

	// Bilinear, wrapping around the horizon and clamping at the poles
	inline glm::vec3 Lookup(const EnvironmentMap& map, const glm::vec3& direction, float rotation) {
		int width = (int)map.GetWidth(), height = (int)map.GetHeight();
		glm::vec2 uv = DirectionToUV(direction, rotation);
		float px = uv.x * (float)width - 0.5f;
		float py = uv.y * (float)height - 0.5f;
		float fx = std::floor(px), fy = std::floor(py);
		float tx = px - fx, ty = py - fy;
		int x0 = (int)fx < 0 ? width - 1 : (int)fx; // u is in [0,1), so fx >= -1
		int x1 = x0 + 1 < width ? x0 + 1 : 0;
		int y0 = std::clamp((int)fy, 0, height - 1), y1 = std::clamp((int)fy + 1, 0, height - 1);

		const std::vector<glm::vec4>& texels = map.GetTexels();
		glm::vec3 top = glm::mix(glm::vec3(texels[(size_t)y0 * width + x0]), glm::vec3(texels[(size_t)y0 * width + x1]), tx);
		glm::vec3 bottom = glm::mix(glm::vec3(texels[(size_t)y1 * width + x0]), glm::vec3(texels[(size_t)y1 * width + x1]), tx);
		return glm::mix(top, bottom, ty);
	}

	// Solid-angle pdf of Sample() producing direction
	inline float Pdf(const EnvironmentMap& map, const glm::vec3& direction, float rotation) {
		float sinTheta = std::sqrt(std::max(0.0f, 1.0f - direction.y * direction.y));
		if (sinTheta <= 0.0f)
			return 0.0f;
		glm::vec2 uv = DirectionToUV(direction, rotation);
		uint32_t x = std::min((uint32_t)(uv.x * (float)map.GetWidth()), map.GetWidth() - 1);
		uint32_t y = std::min((uint32_t)(uv.y * (float)map.GetHeight()), map.GetHeight() - 1);
		return map.GetPdf()[(size_t)y * map.GetWidth() + x] / (2.0f * Pi * Pi * sinTheta);
	}

	// First index in [first, first + count) whose CDF value exceeds u
	inline uint32_t SearchCDF(const std::vector<float>& cdf, size_t first, uint32_t count, float u) {
		uint32_t low = 0, high = count - 1;
		while (low < high) {
			uint32_t middle = (low + high) / 2;
			if (u < cdf[first + middle])
				high = middle;
			else
				low = middle + 1;
		}
		return low;
	}

	inline bool Sample(const EnvironmentMap& map, float rotation, float u1, float u2, glm::vec3& direction, float& pdf) {
		uint32_t width = map.GetWidth(), height = map.GetHeight();
		const std::vector<float>& marginal = map.GetMarginalCDF();
		const std::vector<float>& conditional = map.GetConditionalCDF();

		// Row, then column within it; the leftover fraction of u places the point inside the texel
		uint32_t y = SearchCDF(marginal, 0, height, u1);
		float rowStart = y > 0 ? marginal[y - 1] : 0.0f;
		float dv = (u1 - rowStart) / std::max(marginal[y] - rowStart, 1e-20f);

		size_t row = (size_t)y * width;
		uint32_t x = SearchCDF(conditional, row, width, u2);
		float columnStart = x > 0 ? conditional[row + x - 1] : 0.0f;
		float du = (u2 - columnStart) / std::max(conditional[row + x] - columnStart, 1e-20f);

		glm::vec2 uv(((float)x + glm::clamp(du, 0.0f, 1.0f)) / (float)width, ((float)y + glm::clamp(dv, 0.0f, 1.0f)) / (float)height);
		direction = UVToDirection(uv, rotation);

		float sinTheta = std::sin(Pi * uv.y);
		if (sinTheta <= 0.0f)
			return false;
		pdf = map.GetPdf()[row + x] / (2.0f * Pi * Pi * sinTheta);
		return pdf > 0.0f;
	}

}
