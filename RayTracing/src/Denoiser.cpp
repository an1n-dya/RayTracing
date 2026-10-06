#include "Denoiser.h"

#include "BSDF.h"

#include <algorithm>
#include <execution>
#include <numeric>

namespace {

	constexpr float Kernel[5] = { 1.0f / 16.0f, 1.0f / 4.0f, 3.0f / 8.0f, 1.0f / 4.0f, 1.0f / 16.0f };
	constexpr float MinAlbedo = 0.01f;

}

void ATrousDenoiser::Denoise(const DenoiserInput& input, const RenderSettings& settings, glm::vec3* output) {
	const int width = (int)input.Width, height = (int)input.Height;
	const size_t pixelCount = (size_t)width * height;
	m_Features.resize(pixelCount);
	m_Ping.resize(pixelCount);
	m_Pong.resize(pixelCount);
	m_Rows.resize(height);
	std::iota(m_Rows.begin(), m_Rows.end(), 0u);

	// Averages, demodulation and the variance of the mean luminance
	std::for_each(std::execution::par, m_Rows.begin(), m_Rows.end(), [&](uint32_t y) {
		for (int x = 0; x < width; x++) {
			size_t i = (size_t)y * width + x;
			float sampleCount = input.Color[i].a;
			if (sampleCount <= 0.0f) {
				m_Features[i] = { glm::vec3(0.0f), 0.0f, glm::vec3(1.0f) };
				m_Ping[i] = glm::vec4(0.0f);
				continue;
			}

			glm::vec3 color = glm::vec3(input.Color[i]) / sampleCount;
			glm::vec3 albedo = glm::max(glm::vec3(input.AlbedoDepth[i]) / sampleCount, glm::vec3(MinAlbedo));
			glm::vec3 normal = glm::vec3(input.NormalMoment[i]) / sampleCount;
			float normalLength = glm::length(normal);

			Features& features = m_Features[i];
			features.Normal = normalLength > 1e-3f ? normal / normalLength : glm::vec3(0.0f);
			features.Depth = input.AlbedoDepth[i].a / sampleCount;
			features.Albedo = albedo;

			float luminance = BSDF::Luminance(color);
			float variance = glm::max(0.0f, input.NormalMoment[i].w / sampleCount - luminance * luminance) / sampleCount;
			float albedoLuminance = glm::max(BSDF::Luminance(albedo), MinAlbedo);
			m_Ping[i] = glm::vec4(color / albedo, variance / (albedoLuminance * albedoLuminance));
		}
	});

	int iterations = std::clamp(settings.DenoiseIterations, 1, 8);
	std::vector<glm::vec4>* source = &m_Ping;
	std::vector<glm::vec4>* destination = &m_Pong;
	for (int iteration = 0; iteration < iterations; iteration++) {
		int step = 1 << iteration;
		std::for_each(std::execution::par, m_Rows.begin(), m_Rows.end(), [&](uint32_t y) {
			for (int x = 0; x < width; x++) {
				size_t p = (size_t)y * width + x;
				const glm::vec4& center = (*source)[p];
				const Features& centerFeatures = m_Features[p];
				float centerLuminance = BSDF::Luminance(glm::vec3(center));
				float luminanceSigma = settings.DenoiseColorSigma * glm::sqrt(glm::max(center.a, 0.0f)) + 1e-4f;
				float depthSigma = settings.DenoiseDepthSigma * centerFeatures.Depth * (float)step * 0.01f + 1e-4f;
				bool centerHasNormal = glm::dot(centerFeatures.Normal, centerFeatures.Normal) > 0.0f;

				glm::vec3 sum(0.0f);
				float weightSum = 0.0f, varianceSum = 0.0f;
				for (int dy = -2; dy <= 2; dy++) {
					int qy = (int)y + dy * step;
					if (qy < 0 || qy >= height)
						continue;
					for (int dx = -2; dx <= 2; dx++) {
						int qx = x + dx * step;
						if (qx < 0 || qx >= width)
							continue;
						size_t q = (size_t)qy * width + qx;
						const glm::vec4& tap = (*source)[q];
						const Features& tapFeatures = m_Features[q];

						float weight = Kernel[dx + 2] * Kernel[dy + 2];
						if (q != p) {
							bool tapHasNormal = glm::dot(tapFeatures.Normal, tapFeatures.Normal) > 0.0f;
							float normalWeight = centerHasNormal && tapHasNormal
								? glm::pow(glm::max(0.0f, glm::dot(centerFeatures.Normal, tapFeatures.Normal)), settings.DenoiseNormalSigma)
								: (centerHasNormal == tapHasNormal ? 1.0f : 0.0f);
							float depthWeight = glm::exp(-glm::abs(centerFeatures.Depth - tapFeatures.Depth) / depthSigma);
							float luminanceWeight = glm::exp(-glm::abs(centerLuminance - BSDF::Luminance(glm::vec3(tap))) / luminanceSigma);
							weight *= normalWeight * depthWeight * luminanceWeight;
						}

						sum += weight * glm::vec3(tap);
						weightSum += weight;
						varianceSum += weight * weight * tap.a;
					}
				}

				// The center tap always contributes, so weightSum > 0
				(*destination)[p] = glm::vec4(sum / weightSum, varianceSum / (weightSum * weightSum));
			}
		});
		std::swap(source, destination);
	}

	// Remodulate
	std::for_each(std::execution::par, m_Rows.begin(), m_Rows.end(), [&](uint32_t y) {
		for (int x = 0; x < width; x++) {
			size_t i = (size_t)y * width + x;
			output[i] = glm::vec3((*source)[i]) * m_Features[i].Albedo;
		}
	});
}
