#include "Renderer.h"

#include "Walnut/Random.h"

#include <execution>

namespace Utils {
	static uint32_t ConvertToRGBA(const glm::vec4& color) {
		uint8_t r = static_cast<uint8_t>(color.r * 255.0f);
		uint8_t g = static_cast<uint8_t>(color.g * 255.0f);
		uint8_t b = static_cast<uint8_t>(color.b * 255.0f);
		uint8_t a = static_cast<uint8_t>(color.a * 255.0f);

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

void Renderer::Render(const Scene& scene, const Camera& camera) {
	if (!m_FinalImage || !m_ImageData)
		return;

	m_ActiveScene = &scene;
	m_ActiveCamera = &camera;

	if (m_Settings.UseGPU) {
		m_GpuPathTracer.Render(scene, camera, m_FrameIndex, m_Settings);
	}
	else {
		if (m_FrameIndex == 1)
			memset(m_AccumulationData, 0, m_FinalImage->GetWidth() * m_FinalImage->GetHeight() * sizeof(glm::vec4));

#define MT 1
#if MT
		std::for_each(std::execution::par, m_ImageVerticalIter.begin(), m_ImageVerticalIter.end(),
			[this](uint32_t y) {
				std::for_each(std::execution::par, m_ImageHorizontalIter.begin(), m_ImageHorizontalIter.end(),
					[this, y](uint32_t x) {
						glm::vec4 color = PerPixel(x, y);
						m_AccumulationData[x + y * m_FinalImage->GetWidth()] += color;

						glm::vec4 accumulatedColor = m_AccumulationData[x + y * m_FinalImage->GetWidth()];
						accumulatedColor /= (float)m_FrameIndex;

						accumulatedColor = glm::clamp(accumulatedColor, glm::vec4(0.0f), glm::vec4(1.0f));
						m_ImageData[x + y * m_FinalImage->GetWidth()] = Utils::ConvertToRGBA(accumulatedColor);
					});
			});
#else
		for (uint32_t y = 0; y < m_FinalImage->GetHeight(); y++) {
			for (uint32_t x = 0; x < m_FinalImage->GetWidth(); x++) {
				glm::vec4 color = PerPixel(x, y);
				m_AccumulationData[x + y * m_FinalImage->GetWidth()] += color;

				glm::vec4 accumulatedColor = m_AccumulationData[x + y * m_FinalImage->GetWidth()];
				accumulatedColor /= (float)m_FrameIndex;

				accumulatedColor = glm::clamp(accumulatedColor, glm::vec4(0.0f), glm::vec4(1.0f));
				m_ImageData[x + y * m_FinalImage->GetWidth()] = Utils::ConvertToRGBA(accumulatedColor);
			}
		}
#endif

		m_FinalImage->SetData(m_ImageData);
	}

	if (m_Settings.Accumulate)
		m_FrameIndex++;
	else
		m_FrameIndex = 1;
}

glm::vec4 Renderer::PerPixel(uint32_t x, uint32_t y) {
	Ray ray;
	ray.Origin = m_ActiveCamera->GetPosition();
	ray.Direction = m_ActiveCamera->GetRayDirections()[x + y * m_FinalImage->GetWidth()];

	glm::vec3 light(0.0f);
	glm::vec3 contribution(1.0f);

	uint32_t seed = x + y * m_FinalImage->GetWidth();
	seed *= m_FrameIndex;

	for (int i = 0; i < m_Settings.MaxBounces; i++) {
		seed += i;

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
