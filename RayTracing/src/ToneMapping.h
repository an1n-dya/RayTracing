#pragma once

#include "RenderSettings.h"

#include <glm/glm.hpp>

// Turns accumulated linear HDR radiance into a displayable [0,1] color.
// GLSL counterpart: ResolveColor() and friends in shaders/PathTrace.comp - keep the two in sync.
namespace ToneMapping {

	// Krzysztof Narkowicz's fit of the ACES filmic curve
	inline glm::vec3 ACESFilm(const glm::vec3& x) {
		constexpr float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
		return glm::clamp((x * (a * x + b)) / (x * (c * x + d) + e), glm::vec3(0.0f), glm::vec3(1.0f));
	}

	inline float LinearToSRGB(float c) {
		if (c <= 0.0031308f)
			return 12.92f * c;
		return 1.055f * glm::pow(c, 1.0f / 2.4f) - 0.055f;
	}

	inline glm::vec3 Apply(glm::vec3 color, float exposure, ToneMapper toneMapper, bool sRGB) {
		color *= glm::exp2(exposure);

		switch (toneMapper) {
		case ToneMapper::Reinhard: color = color / (1.0f + color); break;
		case ToneMapper::ACES:     color = ACESFilm(color); break;
		default: break;
		}

		color = glm::clamp(color, glm::vec3(0.0f), glm::vec3(1.0f));
		if (sRGB)
			color = { LinearToSRGB(color.r), LinearToSRGB(color.g), LinearToSRGB(color.b) };
		return color;
	}

	// accumulated.rgb is a sum of radiance samples and accumulated.a is how many there are
	inline glm::vec4 Resolve(const glm::vec4& accumulated, const RenderSettings& settings) {
		glm::vec3 average = accumulated.a > 0.0f ? glm::vec3(accumulated) / accumulated.a : glm::vec3(0.0f);
		return glm::vec4(Apply(average, settings.Exposure, settings.ToneMapping, settings.SRGBOutput), 1.0f);
	}

}
