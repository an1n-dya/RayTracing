#include "EnvironmentMap.h"

#include "BSDF.h"

#include "stb_image.h" // implementation is compiled into Walnut

#include <glm/gtc/constants.hpp>

#include <atomic>

namespace {

	// Procedural daylight: blue gradient sky, a warm sun disc with a glow, and a dark ground below the horizon
	void GenerateSunSky(uint32_t width, uint32_t height, std::vector<glm::vec4>& texels) {
		const float sunElevation = glm::radians(35.0f), sunAzimuth = glm::radians(40.0f);
		const glm::vec3 sunDirection(glm::cos(sunElevation) * glm::cos(sunAzimuth), glm::sin(sunElevation), glm::cos(sunElevation) * glm::sin(sunAzimuth));
		const float sunCosRadius = glm::cos(glm::radians(1.5f));
		const glm::vec3 sunRadiance = glm::vec3(1.0f, 0.95f, 0.85f) * 2800.0f;

		const glm::vec3 zenith(0.22f, 0.42f, 0.9f), horizon(0.9f, 0.93f, 1.0f), ground(0.18f, 0.16f, 0.14f);

		texels.resize((size_t)width * height);
		for (uint32_t y = 0; y < height; y++) {
			float theta = glm::pi<float>() * ((float)y + 0.5f) / (float)height;
			for (uint32_t x = 0; x < width; x++) {
				float phi = 2.0f * glm::pi<float>() * (((float)x + 0.5f) / (float)width - 0.5f);
				glm::vec3 direction(glm::sin(theta) * glm::cos(phi), glm::cos(theta), glm::sin(theta) * glm::sin(phi));

				glm::vec3 color;
				if (direction.y >= 0.0f) {
					color = glm::mix(zenith, horizon, glm::pow(1.0f - direction.y, 3.0f));
					float cosToSun = glm::dot(direction, sunDirection);
					color += glm::vec3(1.0f, 0.85f, 0.6f) * 1.5f * glm::exp((cosToSun - 1.0f) * 30.0f); // glow
					if (cosToSun >= sunCosRadius)
						color += sunRadiance;
				}
				else {
					color = glm::mix(horizon * 0.5f, ground, glm::smoothstep(0.0f, 0.15f, -direction.y));
				}
				texels[(size_t)y * width + x] = glm::vec4(color, 1.0f);
			}
		}
	}

}

std::shared_ptr<EnvironmentMap> EnvironmentMap::Load(const std::string& source, std::string* error) {
	auto map = std::make_shared<EnvironmentMap>();
	map->m_Source = source;

	if (source == BuiltinSunSky) {
		map->m_Width = 1024;
		map->m_Height = 512;
		GenerateSunSky(map->m_Width, map->m_Height, map->m_Texels);
	}
	else {
		int width, height, channels;
		float* data = stbi_loadf(source.c_str(), &width, &height, &channels, 4);
		if (!data) {
			if (error)
				*error = "can't load " + source + ": " + stbi_failure_reason();
			return nullptr;
		}
		map->m_Width = (uint32_t)width;
		map->m_Height = (uint32_t)height;
		map->m_Texels.resize((size_t)width * height);
		for (size_t i = 0; i < map->m_Texels.size(); i++)
			map->m_Texels[i] = glm::vec4(data[i * 4 + 0], data[i * 4 + 1], data[i * 4 + 2], 1.0f);
		stbi_image_free(data);
	}

	static std::atomic<uint64_t> s_NextVersion{ 1 };
	map->m_Version = s_NextVersion++;
	map->BuildSamplingTables();
	return map;
}

void EnvironmentMap::BuildSamplingTables() {
	const uint32_t width = m_Width, height = m_Height;
	m_MarginalCDF.assign(height, 0.0f);
	m_ConditionalCDF.assign((size_t)width * height, 0.0f);
	m_Pdf.assign((size_t)width * height, 0.0f);

	// Weight each texel by brightness * sin(theta): rows near the poles cover less solid angle
	std::vector<double> rowSums(height, 0.0);
	double total = 0.0;
	for (uint32_t y = 0; y < height; y++) {
		float sinTheta = glm::sin(glm::pi<float>() * ((float)y + 0.5f) / (float)height);
		double rowSum = 0.0;
		for (uint32_t x = 0; x < width; x++) {
			size_t index = (size_t)y * width + x;
			float weight = glm::max(BSDF::Luminance(glm::vec3(m_Texels[index])), 0.0f) * sinTheta;
			m_Pdf[index] = weight; // normalized below
			rowSum += weight;
			m_ConditionalCDF[index] = (float)rowSum;
		}
		// Normalize the row's CDF (an all-black row gets a uniform one; it's never picked anyway)
		for (uint32_t x = 0; x < width; x++) {
			size_t index = (size_t)y * width + x;
			m_ConditionalCDF[index] = rowSum > 0.0 ? (float)(m_ConditionalCDF[index] / rowSum) : (float)(x + 1) / (float)width;
		}
		m_ConditionalCDF[(size_t)y * width + width - 1] = 1.0f;
		rowSums[y] = rowSum;
		total += rowSum;
	}

	m_Sampleable = total > 0.0;
	double cumulative = 0.0;
	for (uint32_t y = 0; y < height; y++) {
		cumulative += rowSums[y];
		m_MarginalCDF[y] = m_Sampleable ? (float)(cumulative / total) : (float)(y + 1) / (float)height;
	}
	m_MarginalCDF[height - 1] = 1.0f;

	// Density over (u, v): weight / total * texel count
	double scale = m_Sampleable ? (double)width * height / total : 0.0;
	for (float& pdf : m_Pdf)
		pdf = (float)(pdf * scale);
}
