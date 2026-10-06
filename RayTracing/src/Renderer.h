#pragma once

#include "Walnut/Image.h"

#include "Camera.h"
#include "GpuPathTracer.h"
#include "Ray.h"
#include "RenderSettings.h"
#include "Scene.h"

#include <memory>
#include <string>
#include <glm/glm.hpp>

class Renderer {
public:
	using Settings = RenderSettings;
public:
	Renderer() = default;

	void OnResize(uint32_t width, uint32_t height);
	// Returns false once Settings.MaxSamples is reached; the accumulated image is still re-resolved
	// (tone mapped) every call so post-processing settings stay live
	bool Render(const Scene& scene, const Camera& camera);

	// Editor picking: casts a pinhole ray through ndc (each axis -1 -> 1) on the CPU, whichever path is rendering.
	// Returns the closest object hit (invalid if none) and optionally its distance along the ray.
	ObjectRef Pick(const Scene& scene, const Camera& camera, const glm::vec2& ndc, float* outDistance = nullptr);

	// Writes the current image from whichever path is active: .hdr = linear radiance, anything else = tone mapped PNG
	bool SaveImage(const std::string& path);

	VkDescriptorSet GetFinalImageDescriptorSet() const;
	uint32_t GetFinalImageWidth() const;
	uint32_t GetFinalImageHeight() const;

	void ResetFrameIndex() { m_FrameIndex = 1; }
	uint32_t GetSampleCount() const { return m_AccumulatedSamples; }
	bool IsConverged() const;
	Settings& GetSettings() { return m_Settings; }
private:
	struct HitPayload {
		float HitDistance;
		glm::vec3 WorldPosition;
		glm::vec3 WorldNormal;

		int ObjectIndex;
	};

	glm::vec4 PerPixel(uint32_t x, uint32_t y, uint32_t sampleIndex); // RayGen

	// Dispatch to the fast PCG hash or Walnut::Random depending on Settings.SlowRandom
	float RandomFloat(uint32_t& seed) const;

	HitPayload TraceRay(const Ray& ray);
	HitPayload ClosestHit(const Ray& ray, float hitDistance, int objectIndex);
	HitPayload Miss(const Ray& ray);
private:
	std::shared_ptr<Walnut::Image> m_FinalImage;
	GpuPathTracer m_GpuPathTracer;
	Settings m_Settings;

	std::vector<uint32_t> m_ImageHorizontalIter, m_ImageVerticalIter;

	const Scene* m_ActiveScene = nullptr;
	const Camera* m_ActiveCamera = nullptr;

	uint32_t* m_ImageData = nullptr;
	glm::vec4* m_AccumulationData = nullptr;

	uint32_t m_FrameIndex = 1;         // 1 = the next frame starts a fresh accumulation
	uint32_t m_AccumulatedSamples = 0; // samples per pixel in the image currently displayed
};
