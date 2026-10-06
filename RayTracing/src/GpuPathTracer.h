#pragma once

#include "Camera.h"
#include "PreparedScene.h"
#include "RenderSettings.h"
#include "Scene.h"

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

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
	// frame.SampleCount = 0 skips path tracing and only re-resolves the accumulated image to the display image
	void Render(const Scene& scene, const PreparedScene& prepared, const Camera& camera, const RenderSettings& settings, const FrameParams& frame);

	// Synchronous GPU -> CPU copies of the current images (rows bottom-up, like the CPU path's buffers)
	void ReadDisplayImage(std::vector<uint32_t>& pixels);        // tone mapped RGBA8
	void ReadAccumulationImage(std::vector<glm::vec4>& pixels);  // rgb = radiance sum, a = sample count
	void ReadDenoisedImage(std::vector<glm::vec4>& pixels);      // rgb = linear denoised color, a = 1

	VkDescriptorSet GetDescriptorSet() const { return m_DisplayDescriptorSet; }
	uint32_t GetWidth() const { return m_Width; }
	uint32_t GetHeight() const { return m_Height; }

private:
	// Host-visible, persistently mapped buffer bound to one descriptor binding; grown on demand
	struct Buffer {
		VkBuffer Handle = VK_NULL_HANDLE;
		VkDeviceMemory Memory = VK_NULL_HANDLE;
		void* Mapped = nullptr;
		VkDeviceSize Size = 0;
	};

	struct StorageImage {
		VkImage Image = VK_NULL_HANDLE;
		VkDeviceMemory Memory = VK_NULL_HANDLE;
		VkImageView View = VK_NULL_HANDLE;
	};

	void Init();
	void InitDenoiser();
	void Denoise(VkCommandBuffer commandBuffer, const RenderSettings& settings);
	void UploadGeometry(const Scene& scene);
	void UploadEnvironment(const SkySettings& sky);
	void CreateImages(uint32_t width, uint32_t height);
	void ReleaseImages();

	// Makes sure buffer holds at least size bytes (reallocating and re-binding it if not), then copies data in
	void Upload(Buffer& buffer, uint32_t binding, VkDescriptorType type, const void* data, VkDeviceSize size);
	// Replaces buffer with a device-local storage buffer filled through a staging copy, for big data that rarely
	// changes (mesh geometry, environment maps) - fast to read even without resizable BAR
	void UploadStatic(Buffer& buffer, uint32_t binding, const void* data, VkDeviceSize size);
	static void ReleaseBuffer(Buffer& buffer); // deferred until the GPU is done with it
	void WriteBufferDescriptor(uint32_t binding, VkDescriptorType type, const Buffer& buffer);
	void WriteImageDescriptor(VkDescriptorSet set, uint32_t binding, VkImageView view);

	void ReadImage(VkImage image, VkDeviceSize bytesPerPixel, void* destination);

private:
	bool m_Initialized = false;

	uint32_t m_Width = 0, m_Height = 0;

	// Persistent pipeline objects (created once, live for the app's lifetime)
	VkShaderModule m_ShaderModule = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_DescriptorSetLayout = VK_NULL_HANDLE;
	VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
	VkPipeline m_Pipeline = VK_NULL_HANDLE;
	VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;
	VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE; // compute shader bindings (UBO/SSBOs/images)

	// Scene/frame data, re-written every frame
	Buffer m_FrameBuffer; // UBO: camera + environment
	Buffer m_SphereBuffer;
	Buffer m_MaterialBuffer;
	Buffer m_PlaneBuffer;
	Buffer m_InstanceBuffer;
	Buffer m_LightBuffer;
	Buffer m_LightCDFBuffer;

	// Mesh geometry, only re-uploaded when Scene::GeometryVersion changes
	Buffer m_VertexBuffer;
	Buffer m_IndexBuffer;
	Buffer m_BVHBuffer;
	bool m_GeometryUploaded = false;
	uint64_t m_UploadedGeometryVersion = 0;
	std::vector<uint32_t> m_MeshFirstTriangle; // per mesh, into the concatenated index buffer
	std::vector<uint32_t> m_MeshFirstNode;     // per mesh, into the concatenated BVH node buffer

	// Environment map + sampling tables, only re-uploaded when the map changes
	Buffer m_EnvironmentTexelBuffer;
	Buffer m_EnvironmentMarginalBuffer;
	Buffer m_EnvironmentConditionalBuffer;
	Buffer m_EnvironmentPdfBuffer;
	bool m_EnvironmentUploaded = false;
	uint64_t m_UploadedEnvironmentVersion = 0; // 0 = none (placeholder buffers)

	// Denoiser pipeline (shaders/Denoise.comp), run as several passes after path tracing
	VkShaderModule m_DenoiseShaderModule = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_DenoiseDescriptorSetLayout = VK_NULL_HANDLE;
	VkPipelineLayout m_DenoisePipelineLayout = VK_NULL_HANDLE;
	VkPipeline m_DenoisePipeline = VK_NULL_HANDLE;
	VkDescriptorPool m_DenoiseDescriptorPool = VK_NULL_HANDLE;
	VkDescriptorSet m_DenoiseDescriptorSet = VK_NULL_HANDLE;
	uint32_t m_LastDenoiseIterations = 0;

	// Per-resize resources
	StorageImage m_AccumulationImage;  // rgba32f: rgb = radiance sum, a = sample count
	StorageImage m_DisplayImage;       // rgba8: tone mapped, sampled by ImGui
	StorageImage m_AlbedoDepthImage;   // rgba32f: denoiser features, accumulated like the color
	StorageImage m_NormalMomentImage;  // rgba32f
	StorageImage m_PingImage;          // rgba32f: denoiser passes alternate between these two
	StorageImage m_PongImage;
	VkSampler m_DisplaySampler = VK_NULL_HANDLE;
	VkDescriptorSet m_DisplayDescriptorSet = VK_NULL_HANDLE; // ImGui texture id
};
