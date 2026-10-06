#pragma once

#include "RenderSettings.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

// Image-space denoising of the accumulated render. Inputs are the renderer's per-pixel running *sums*.
struct DenoiserInput {
	uint32_t Width = 0, Height = 0;
	const glm::vec4* Color = nullptr;        // rgb = radiance sum, a = sample count
	const glm::vec4* AlbedoDepth = nullptr;  // rgb = first-surface albedo sum, a = first-hit distance sum
	const glm::vec4* NormalMoment = nullptr; // xyz = first-surface normal sum, w = sum of radiance luminance^2
};

// Interface, so another denoiser (e.g. Intel Open Image Denoise) can be dropped in later
class Denoiser {
public:
	virtual ~Denoiser() = default;

	// Writes Width * Height linear RGB colors
	virtual void Denoise(const DenoiserInput& input, const RenderSettings& settings, glm::vec3* output) = 0;
};

// Edge-avoiding a-trous wavelet filter (Dammertz et al. 2010): a few passes of a 5x5 B3-spline kernel with
// doubling spacing, run on demodulated irradiance (color / albedo). Taps are weighted down across normal and depth
// discontinuities, and across luminance differences larger than the pixel's noise (its variance of the mean, as
// in SVGF), so edges stay sharp and the filter backs off as the image converges.
// GLSL counterpart: shaders/Denoise.comp - keep the two in sync.
class ATrousDenoiser : public Denoiser {
public:
	void Denoise(const DenoiserInput& input, const RenderSettings& settings, glm::vec3* output) override;

private:
	struct Features {
		glm::vec3 Normal; // normalized average, or zero where the camera saw the sky
		float Depth;
		glm::vec3 Albedo; // clamped away from zero for demodulation
	};

	std::vector<Features> m_Features;
	std::vector<glm::vec4> m_Ping, m_Pong; // rgb = irradiance, a = its variance
	std::vector<uint32_t> m_Rows;
};
