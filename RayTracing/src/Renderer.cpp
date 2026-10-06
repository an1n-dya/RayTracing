#include "Renderer.h"

#include "ImageExport.h"
#include "ToneMapping.h"

#include "Walnut/Random.h"

#include <execution>
#include <filesystem>

namespace Utils {
	static uint32_t ConvertToRGBA(const glm::vec4& color) {
		// Round to nearest, like the GPU's UNORM image stores
		uint8_t r = static_cast<uint8_t>(color.r * 255.0f + 0.5f);
		uint8_t g = static_cast<uint8_t>(color.g * 255.0f + 0.5f);
		uint8_t b = static_cast<uint8_t>(color.b * 255.0f + 0.5f);
		uint8_t a = static_cast<uint8_t>(color.a * 255.0f + 0.5f);

		return (a << 24) | (b << 16) | (g << 8) | r;
	}

	static uint32_t PCG_Hash(uint32_t input) {
		uint32_t state = input * 747796405u + 2891336453u;
		uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
		return (word >> 22u) ^ word;
	}

	static float RandomFloat(uint32_t& seed) {
		seed = PCG_Hash(seed);
		return (float)seed / (float)std::numeric_limits<uint32_t>::max();
	}

	static glm::vec3 InUnitSphere(uint32_t& seed) {
		return glm::normalize(glm::vec3(
			RandomFloat(seed) * 2.0f - 1.0f,
			RandomFloat(seed) * 2.0f - 1.0f,
			RandomFloat(seed) * 2.0f - 1.0f)
		);
	}

#define MT 1
	// Runs fn(x, y) for every pixel, in parallel unless MT is 0
	template<typename Fn>
	static void ForEachPixel(const std::vector<uint32_t>& xs, const std::vector<uint32_t>& ys, Fn&& fn) {
#if MT
		std::for_each(std::execution::par, ys.begin(), ys.end(), [&](uint32_t y) {
			std::for_each(std::execution::par, xs.begin(), xs.end(), [&](uint32_t x) { fn(x, y); });
		});
#else
		for (uint32_t y : ys)
			for (uint32_t x : xs)
				fn(x, y);
#endif
	}
}

void Renderer::OnResize(uint32_t width, uint32_t height) {
	m_GpuPathTracer.OnResize(width, height);

	if (m_FinalImage) {
		if (m_FinalImage->GetWidth() == width && m_FinalImage->GetHeight() == height)
			return;
		m_FinalImage->Resize(width, height);
	}
	else {
		m_FinalImage = std::make_shared<Walnut::Image>(width, height, Walnut::ImageFormat::RGBA);
	}

	// The (re)allocated accumulation buffers hold garbage until frame 1 clears them
	m_FrameIndex = 1;

	delete[] m_ImageData;
	m_ImageData = new uint32_t[width * height];

	delete[] m_AccumulationData;
	m_AccumulationData = new glm::vec4[width * height];
		
	m_ImageHorizontalIter.resize(width);
	m_ImageVerticalIter.resize(height);
	for (uint32_t i = 0; i < width; i++)
		m_ImageHorizontalIter[i] = i;
	for (uint32_t i = 0; i < height; i++)
		m_ImageVerticalIter[i] = i;
}

bool Renderer::IsConverged() const {
	return m_Settings.Accumulate && m_Settings.MaxSamples > 0 && m_FrameIndex > 1
		&& m_AccumulatedSamples >= (uint32_t)m_Settings.MaxSamples;
}

bool Renderer::Render(const Scene& scene, const Camera& camera) {
	if (!m_FinalImage || !m_ImageData)
		return false;

	m_ActiveScene = &scene;
	m_ActiveCamera = &camera;

	if (m_FrameIndex == 1)
		m_AccumulatedSamples = 0;

	FrameParams frame;
	frame.ResetAccumulation = m_FrameIndex == 1;
	frame.FirstSampleIndex = m_AccumulatedSamples;
	frame.SampleCount = (uint32_t)std::max(m_Settings.SamplesPerFrame, 1);
	if (m_Settings.Accumulate && m_Settings.MaxSamples > 0) // don't overshoot the limit
		frame.SampleCount = std::min(frame.SampleCount, (uint32_t)m_Settings.MaxSamples - std::min(m_AccumulatedSamples, (uint32_t)m_Settings.MaxSamples));
	// Once converged SampleCount is 0: only re-resolve the accumulated image (so exposure/tone mapping stay live)

	if (m_Settings.UseGPU) {
		m_GpuPathTracer.Render(scene, camera, m_Settings, frame);
	}
	else {
		uint32_t width = m_FinalImage->GetWidth();
		if (frame.ResetAccumulation)
			memset(m_AccumulationData, 0, width * m_FinalImage->GetHeight() * sizeof(glm::vec4));

		Utils::ForEachPixel(m_ImageHorizontalIter, m_ImageVerticalIter, [this, &frame, width](uint32_t x, uint32_t y) {
			uint32_t index = x + y * width;
			for (uint32_t s = 0; s < frame.SampleCount; s++)
				m_AccumulationData[index] += PerPixel(x, y, frame.FirstSampleIndex + s);
			m_ImageData[index] = Utils::ConvertToRGBA(ToneMapping::Resolve(m_AccumulationData[index], m_Settings));
		});

		m_FinalImage->SetData(m_ImageData);
	}

	if (frame.SampleCount == 0)
		return false;

	m_AccumulatedSamples += frame.SampleCount;

	if (m_Settings.Accumulate)
		m_FrameIndex++;
	else
		m_FrameIndex = 1;

	return true;
}

glm::vec4 Renderer::PerPixel(uint32_t x, uint32_t y, uint32_t sampleIndex) {
	// Decorrelate pixels and samples: every (pixel, sample) pair gets its own random stream
	uint32_t seed = Utils::PCG_Hash((x + y * m_FinalImage->GetWidth()) ^ Utils::PCG_Hash(sampleIndex));

	glm::vec2 jitter(0.5f);
	if (m_Settings.AntiAliasing)
		jitter = { RandomFloat(seed), RandomFloat(seed) };

	glm::vec2 ndc = (glm::vec2((float)x, (float)y) + jitter) / glm::vec2((float)m_FinalImage->GetWidth(), (float)m_FinalImage->GetHeight());
	ndc = ndc * 2.0f - 1.0f; // -1 -> 1

	glm::vec2 lensSample(0.0f);
	if (m_ActiveCamera->GetAperture() > 0.0f)
		lensSample = { RandomFloat(seed), RandomFloat(seed) };

	Ray ray = m_ActiveCamera->GenerateRay(ndc, lensSample);

	glm::vec3 light(0.0f);
	glm::vec3 contribution(1.0f);

	for (int i = 0; i < m_Settings.MaxBounces; i++) {
		Renderer::HitPayload payload = TraceRay(ray);
		if (payload.HitDistance < 0.0f) {
			light += m_ActiveScene->Sky.GetRadiance(ray.Direction) * contribution;
			break;
		}

		const Sphere& sphere = m_ActiveScene->Spheres[payload.ObjectIndex];
		const Material& material = m_ActiveScene->Materials[sphere.MaterialIndex];

		// Emission is weighted by the throughput accumulated *before* this surface's albedo is applied
		light += material.GetEmission() * contribution;
		contribution *= material.Albedo;

		ray.Origin = payload.WorldPosition + payload.WorldNormal * 0.0001f;
		//ray.Direction = glm::reflect(ray.Direction,
		//	payload.WorldNormal + material.Roughness * Walnut::Random::Vec3(-0.5f, 0.5f));
		ray.Direction = glm::normalize(payload.WorldNormal + RandomInUnitSphere(seed));

		// Russian roulette: randomly end paths that can't carry much more light, boosting the
		// survivors by 1/p so the estimate stays unbiased.
		if (m_Settings.RussianRoulette && i >= m_Settings.RussianRouletteStartBounce) {
			float p = glm::clamp(glm::max(contribution.r, glm::max(contribution.g, contribution.b)), 0.05f, 1.0f);
			if (RandomFloat(seed) > p)
				break;
			contribution /= p;
		}
	}

	return glm::vec4(light, 1.0f);
}

float Renderer::RandomFloat(uint32_t& seed) const {
	if (m_Settings.SlowRandom)
		return Walnut::Random::Float();
	return Utils::RandomFloat(seed);
}

glm::vec3 Renderer::RandomInUnitSphere(uint32_t& seed) const {
	if (m_Settings.SlowRandom)
		return Walnut::Random::InUnitSphere();
	return Utils::InUnitSphere(seed);
}

Renderer::HitPayload Renderer::TraceRay(const Ray& ray) {
	// Equation of a sphere:
	// (bx^2 + by^2)t^2 + (2(axbx + ayby))t + (ax^2 + ay^2 - r^2) = 0
	// where:
	// a = ray origin	
	// b = ray direction
	// r = radius
	// t = hit distance	

	int closestSphere = -1;
	float hitDistance = std::numeric_limits<float>::max();

	for (size_t i = 0; i < m_ActiveScene->Spheres.size(); i++) {
		const Sphere& sphere = m_ActiveScene->Spheres[i];
		glm::vec3 origin = ray.Origin - sphere.Position;

		float a = glm::dot(ray.Direction, ray.Direction);
		float b = 2.0f * glm::dot(origin, ray.Direction);
		float c = glm::dot(origin, origin) - sphere.Radius * sphere.Radius;

		// Quadratic formula discriminant:
		// discriminant = b^2 - 4ac
		float discriminant = b * b - 4.0f * a * c;
		if (discriminant < 0.0f)
			continue;

		// Solve for t using the quadratic formula:
		// t = (-b +- sqrt(discriminant)) / 2a
		
		// float t0 = (-b + glm::sqrt(discriminant)) / (2.0f * a); // Second hit distance (currently unused)
		float closestT = (-b - glm::sqrt(discriminant)) / (2.0f * a);
		if (closestT > 0.0f && closestT < hitDistance) {
			hitDistance = closestT;
			closestSphere = (int)i;
		}
	}

	if (closestSphere < 0)
		return Miss(ray);

	return ClosestHit(ray, hitDistance,	closestSphere);
}

Renderer::HitPayload Renderer::ClosestHit(const Ray& ray, float hitDistance, int objectIndex) {
	Renderer::HitPayload payload;
	payload.HitDistance = hitDistance;
	payload.ObjectIndex = objectIndex;

	const Sphere& closestSphere = m_ActiveScene->Spheres[objectIndex];

	glm::vec3 origin = ray.Origin - closestSphere.Position;
	payload.WorldPosition = origin + ray.Direction * hitDistance;
	payload.WorldNormal = glm::normalize(payload.WorldPosition);

	payload.WorldPosition += closestSphere.Position;

	return payload;
}

Renderer::HitPayload Renderer::Miss(const Ray& ray) {
	Renderer::HitPayload payload;
	payload.HitDistance = -0.1f;
	return payload;
}

ObjectRef Renderer::Pick(const Scene& scene, const Camera& camera, const glm::vec2& ndc, float* outDistance) {
	m_ActiveScene = &scene;

	Ray ray;
	ray.Origin = camera.GetPosition();
	ray.Direction = camera.GetRayDirection(ndc);

	HitPayload payload = TraceRay(ray);
	if (payload.HitDistance < 0.0f)
		return {};

	if (outDistance)
		*outDistance = payload.HitDistance;
	return { ObjectType::Sphere, payload.ObjectIndex };
}

bool Renderer::SaveImage(const std::string& path) {
	uint32_t width = GetFinalImageWidth();
	uint32_t height = GetFinalImageHeight();
	if (width == 0 || height == 0)
		return false;
	size_t pixelCount = (size_t)width * height;

	std::string extension = std::filesystem::path(path).extension().string();
	for (char& c : extension)
		c = (char)tolower(c);

	if (extension == ".hdr") {
		std::vector<glm::vec4> accumulation;
		if (m_Settings.UseGPU)
			m_GpuPathTracer.ReadAccumulationImage(accumulation);
		else
			accumulation.assign(m_AccumulationData, m_AccumulationData + pixelCount);

		std::vector<float> rgb(pixelCount * 3);
		for (size_t i = 0; i < pixelCount; i++) {
			glm::vec3 average = accumulation[i].a > 0.0f ? glm::vec3(accumulation[i]) / accumulation[i].a : glm::vec3(0.0f);
			rgb[i * 3 + 0] = average.r;
			rgb[i * 3 + 1] = average.g;
			rgb[i * 3 + 2] = average.b;
		}
		return ImageExport::SaveHDR(path, width, height, rgb.data());
	}

	std::vector<uint32_t> pixels;
	if (m_Settings.UseGPU)
		m_GpuPathTracer.ReadDisplayImage(pixels);
	else
		pixels.assign(m_ImageData, m_ImageData + pixelCount);
	return ImageExport::SavePNG(path, width, height, pixels.data());
}

VkDescriptorSet Renderer::GetFinalImageDescriptorSet() const {
	if (m_Settings.UseGPU)
		return m_GpuPathTracer.GetDescriptorSet();
	return m_FinalImage ? m_FinalImage->GetDescriptorSet() : nullptr;
}

uint32_t Renderer::GetFinalImageWidth() const {
	if (m_Settings.UseGPU)
		return m_GpuPathTracer.GetWidth();
	return m_FinalImage ? m_FinalImage->GetWidth() : 0;
}

uint32_t Renderer::GetFinalImageHeight() const {
	if (m_Settings.UseGPU)
		return m_GpuPathTracer.GetHeight();
	return m_FinalImage ? m_FinalImage->GetHeight() : 0;
}
