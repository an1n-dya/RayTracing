#pragma once

#include "Camera.h"
#include "RenderSettings.h"
#include "Scene.h"

#include <vulkan/vulkan.h>

#include <cstdint>

// Vulkan compute-shader path tracer. Owns its own storage image (never touches Walnut::Image),
// which it registers directly with ImGui via ImGui_ImplVulkan_AddTexture so it can be drawn
// straight into the viewport with zero CPU readback. Mirrors the math in Renderer.cpp exactly;
// see RayTracing/src/shaders/PathTrace.comp for the shader itself.
class GpuPathTracer {
public:
	GpuPathTracer() = default;
	~GpuPathTracer();

	GpuPathTracer(const GpuPathTracer&) = delete;
	GpuPathTracer& operator=(const GpuPathTracer&) = delete;

	void OnResize(uint32_t width, uint32_t height);
	// trace = false skips path tracing and only re-resolves the accumulated image to the display image
	void Render(const Scene& scene, const Camera& camera, uint32_t frameIndex, const RenderSettings& settings, bool trace);

	VkDescriptorSet GetDescriptorSet() const { return m_DisplayDescriptorSet; }
	uint32_t GetWidth() const { return m_Width; }
	uint32_t GetHeight() const { return m_Height; }

private:
	void Init();
	void CreateImages(uint32_t width, uint32_t height);
	void ReleaseImages();

private:
	static constexpr uint32_t MaxSpheres = 64;
	static constexpr uint32_t MaxMaterials = 64;

	bool m_Initialized = false;

	uint32_t m_Width = 0, m_Height = 0;

	// Persistent pipeline objects (created once, live for the app's lifetime)
	VkShaderModule m_ShaderModule = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_DescriptorSetLayout = VK_NULL_HANDLE;
	VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
	VkPipeline m_Pipeline = VK_NULL_HANDLE;
	VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;
	VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE; // compute shader bindings (UBO/SSBOs/images)

	// Persistent, host-mapped upload buffers, re-written every frame
	VkBuffer m_CameraUBO = VK_NULL_HANDLE;
	VkDeviceMemory m_CameraUBOMemory = VK_NULL_HANDLE;
	void* m_CameraUBOMapped = nullptr;

	VkBuffer m_SphereSSBO = VK_NULL_HANDLE;
	VkDeviceMemory m_SphereSSBOMemory = VK_NULL_HANDLE;
	void* m_SphereSSBOMapped = nullptr;

	VkBuffer m_MaterialSSBO = VK_NULL_HANDLE;
	VkDeviceMemory m_MaterialSSBOMemory = VK_NULL_HANDLE;
	void* m_MaterialSSBOMapped = nullptr;

	// Per-resize resources
	VkImage m_AccumulationImage = VK_NULL_HANDLE;
	VkDeviceMemory m_AccumulationImageMemory = VK_NULL_HANDLE;
	VkImageView m_AccumulationImageView = VK_NULL_HANDLE;

	VkImage m_DisplayImage = VK_NULL_HANDLE;
	VkDeviceMemory m_DisplayImageMemory = VK_NULL_HANDLE;
	VkImageView m_DisplayImageView = VK_NULL_HANDLE;
	VkSampler m_DisplaySampler = VK_NULL_HANDLE;
	VkDescriptorSet m_DisplayDescriptorSet = VK_NULL_HANDLE; // ImGui texture id
};
