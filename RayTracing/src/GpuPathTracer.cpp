#include "GpuPathTracer.h"

#include "Walnut/Application.h"

#include "backends/imgui_impl_vulkan.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {

	// Descriptor bindings - must match the layout(binding = N) declarations in PathTrace.comp
	namespace Binding {
		enum : uint32_t {
			Frame = 0,             // UBO: camera + environment
			Spheres = 1,           // SSBO
			Materials = 2,         // SSBO
			AccumulationImage = 3, // storage image, rgba32f
			DisplayImage = 4,      // storage image, rgba8
			Planes = 5,            // SSBO
			Instances = 6,         // SSBO: mesh instances
			Vertices = 7,          // SSBO: all meshes' vertices
			Indices = 8,           // SSBO: all meshes' triangles (global vertex indices)
			Count
		};
	}

	const VkDescriptorType BindingTypes[Binding::Count] = {
		VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
	};

	// Mirrors the std430/std140 layouts declared in PathTrace.comp
	struct SphereGPU {
		glm::vec4 PositionAndRadius; // xyz = position, w = radius
		glm::ivec4 MaterialIndex;    // x = material index
	};

	struct MaterialGPU {
		glm::vec4 AlbedoRoughness;  // rgb = albedo, w = roughness
		glm::vec4 EmissionAndPower; // rgb = emission color, w = emission power
		glm::vec4 MetallicTransmissionIOR; // x = metallic, y = transmission, z = IOR
	};

	struct PlaneGPU {
		glm::vec4 Point;
		glm::vec4 Normal;
		glm::ivec4 MaterialIndex; // x
	};

	struct InstanceGPU {
		glm::mat4 ObjectToWorld;
		glm::mat4 WorldToObject;
		glm::vec4 BoundsMin; // object space
		glm::vec4 BoundsMax;
		glm::uvec4 Info;     // x = first triangle, y = triangle count, z = material index
	};

	struct VertexGPU {
		glm::vec4 Position;
		glm::vec4 Normal;
	};

	struct FrameUBOData {
		glm::mat4 InverseProjection;
		glm::mat4 InverseView;
		glm::vec4 Position;
		glm::vec4 Lens; // x = aperture, y = focus distance

		glm::vec4 SkyBottomColor; // rgb = color, w = intensity
		glm::vec4 SkyTopColor;
		int32_t SkyMode;
		int32_t Pad[3];
	};

	struct PushConstants {
		uint32_t Width;
		uint32_t Height;
		uint32_t FirstSampleIndex;
		uint32_t SphereCount;
		uint32_t Bounces;
		uint32_t RussianRoulette;
		uint32_t RussianRouletteStartBounce;
		uint32_t SampleCount;
		float Exposure;
		uint32_t ToneMapper;
		uint32_t SRGBOutput;
		uint32_t AntiAliasing;
		uint32_t ResetAccumulation;
		uint32_t PlaneCount;
		uint32_t InstanceCount;
	};

	uint32_t FindMemoryType(VkMemoryPropertyFlags properties, uint32_t typeBits)
	{
		VkPhysicalDeviceMemoryProperties memProperties;
		vkGetPhysicalDeviceMemoryProperties(Walnut::Application::GetPhysicalDevice(), &memProperties);
		for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
		{
			if ((memProperties.memoryTypes[i].propertyFlags & properties) == properties && (typeBits & (1 << i)))
				return i;
		}
		return 0xffffffff;
	}

	// Host-visible + coherent buffer. Prefers memory that is also device-local (resizable BAR), which the
	// shader can read at full speed, and falls back to plain host memory if there is none (or it's full).
	void CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory, void** mapped)
	{
		VkDevice device = Walnut::Application::GetDevice();

		VkBufferCreateInfo bufferInfo{};
		bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferInfo.size = size;
		bufferInfo.usage = usage;
		bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VkResult err = vkCreateBuffer(device, &bufferInfo, nullptr, &buffer);
		check_vk_result(err);

		VkMemoryRequirements memReq;
		vkGetBufferMemoryRequirements(device, buffer, &memReq);

		VkMemoryAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocInfo.allocationSize = memReq.size;

		const VkMemoryPropertyFlags hostVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		allocInfo.memoryTypeIndex = FindMemoryType(hostVisible | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memReq.memoryTypeBits);
		err = allocInfo.memoryTypeIndex != 0xffffffff ? vkAllocateMemory(device, &allocInfo, nullptr, &memory) : VK_ERROR_OUT_OF_DEVICE_MEMORY;
		if (err != VK_SUCCESS)
		{
			allocInfo.memoryTypeIndex = FindMemoryType(hostVisible, memReq.memoryTypeBits);
			err = vkAllocateMemory(device, &allocInfo, nullptr, &memory);
			check_vk_result(err);
		}

		err = vkBindBufferMemory(device, buffer, memory, 0);
		check_vk_result(err);

		if (mapped)
		{
			err = vkMapMemory(device, memory, 0, size, 0, mapped);
			check_vk_result(err);
		}
	}

	void CreateStorageImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
		VkImage& image, VkDeviceMemory& memory, VkImageView& view)
	{
		VkDevice device = Walnut::Application::GetDevice();

		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.format = format;
		imageInfo.extent = { width, height, 1 };
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.usage = usage;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VkResult err = vkCreateImage(device, &imageInfo, nullptr, &image);
		check_vk_result(err);

		VkMemoryRequirements memReq;
		vkGetImageMemoryRequirements(device, image, &memReq);

		VkMemoryAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocInfo.allocationSize = memReq.size;
		allocInfo.memoryTypeIndex = FindMemoryType(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memReq.memoryTypeBits);
		err = vkAllocateMemory(device, &allocInfo, nullptr, &memory);
		check_vk_result(err);

		err = vkBindImageMemory(device, image, memory, 0);
		check_vk_result(err);

		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = format;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.layerCount = 1;
		err = vkCreateImageView(device, &viewInfo, nullptr, &view);
		check_vk_result(err);
	}

	std::vector<char> ReadFile(const std::string& path)
	{
		std::ifstream file(path, std::ios::ate | std::ios::binary);
		if (!file.is_open())
			throw std::runtime_error("GpuPathTracer: failed to open shader file: " + path);

		size_t fileSize = (size_t)file.tellg();
		std::vector<char> buffer(fileSize);
		file.seekg(0);
		file.read(buffer.data(), fileSize);
		return buffer;
	}

	VkShaderModule CreateShaderModule(const std::vector<char>& code)
	{
		VkShaderModuleCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		createInfo.codeSize = code.size();
		createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

		VkShaderModule shaderModule;
		VkResult err = vkCreateShaderModule(Walnut::Application::GetDevice(), &createInfo, nullptr, &shaderModule);
		check_vk_result(err);
		return shaderModule;
	}

	VkBufferUsageFlags GetBufferUsage(VkDescriptorType type)
	{
		return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	}

}

GpuPathTracer::~GpuPathTracer()
{
	if (!m_Initialized)
		return;

	if (m_AccumulationImage.Image != VK_NULL_HANDLE)
		ReleaseImages();

	std::vector<Buffer> buffers = { m_FrameBuffer, m_SphereBuffer, m_MaterialBuffer, m_PlaneBuffer, m_InstanceBuffer,
		m_VertexBuffer, m_IndexBuffer };

	Walnut::Application::SubmitResourceFree([shaderModule = m_ShaderModule, pipeline = m_Pipeline,
		pipelineLayout = m_PipelineLayout, descriptorSetLayout = m_DescriptorSetLayout, descriptorPool = m_DescriptorPool,
		sampler = m_DisplaySampler, buffers]()
	{
		VkDevice device = Walnut::Application::GetDevice();

		vkDestroyPipeline(device, pipeline, nullptr);
		vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
		vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
		vkDestroyDescriptorPool(device, descriptorPool, nullptr); // also frees the set allocated from it
		vkDestroyShaderModule(device, shaderModule, nullptr);
		if (sampler != VK_NULL_HANDLE)
			vkDestroySampler(device, sampler, nullptr);

		for (const Buffer& buffer : buffers)
		{
			if (buffer.Handle == VK_NULL_HANDLE)
				continue;
			vkUnmapMemory(device, buffer.Memory);
			vkDestroyBuffer(device, buffer.Handle, nullptr);
			vkFreeMemory(device, buffer.Memory, nullptr);
		}
	});
}

void GpuPathTracer::Init()
{
	VkDevice device = Walnut::Application::GetDevice();

	auto shaderCode = ReadFile("src/shaders/PathTrace.comp.spv");
	m_ShaderModule = CreateShaderModule(shaderCode);

	VkDescriptorSetLayoutBinding bindings[Binding::Count]{};
	for (uint32_t i = 0; i < Binding::Count; i++)
		bindings[i] = { i, BindingTypes[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

	VkDescriptorSetLayoutCreateInfo layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = Binding::Count;
	layoutInfo.pBindings = bindings;
	VkResult err = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_DescriptorSetLayout);
	check_vk_result(err);

	VkPushConstantRange pushConstantRange{};
	pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pushConstantRange.offset = 0;
	pushConstantRange.size = sizeof(PushConstants);

	VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &m_DescriptorSetLayout;
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
	err = vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &m_PipelineLayout);
	check_vk_result(err);

	VkPipelineShaderStageCreateInfo stageInfo{};
	stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	stageInfo.module = m_ShaderModule;
	stageInfo.pName = "main";

	VkComputePipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipelineInfo.stage = stageInfo;
	pipelineInfo.layout = m_PipelineLayout;
	err = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_Pipeline);
	check_vk_result(err);

	// One pool entry per binding of each type
	std::vector<VkDescriptorPoolSize> poolSizes;
	for (VkDescriptorType type : BindingTypes)
	{
		auto it = std::find_if(poolSizes.begin(), poolSizes.end(), [type](const VkDescriptorPoolSize& size) { return size.type == type; });
		if (it != poolSizes.end())
			it->descriptorCount++;
		else
			poolSizes.push_back({ type, 1 });
	}

	VkDescriptorPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = 1;
	poolInfo.poolSizeCount = (uint32_t)poolSizes.size();
	poolInfo.pPoolSizes = poolSizes.data();
	err = vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_DescriptorPool);
	check_vk_result(err);

	VkDescriptorSetAllocateInfo dsAllocInfo{};
	dsAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dsAllocInfo.descriptorPool = m_DescriptorPool;
	dsAllocInfo.descriptorSetCount = 1;
	dsAllocInfo.pSetLayouts = &m_DescriptorSetLayout;
	err = vkAllocateDescriptorSets(device, &dsAllocInfo, &m_DescriptorSet);
	check_vk_result(err);

	m_Initialized = true;
}

void GpuPathTracer::UploadGeometry(const Scene& scene)
{
	if (m_GeometryUploaded && m_UploadedGeometryVersion == scene.GeometryVersion)
		return;

	std::vector<VertexGPU> vertices;
	std::vector<uint32_t> indices;
	m_MeshFirstTriangle.clear();
	for (const Mesh& mesh : scene.Meshes)
	{
		uint32_t vertexOffset = (uint32_t)vertices.size();
		m_MeshFirstTriangle.push_back((uint32_t)(indices.size() / 3));

		for (const Vertex& vertex : mesh.Vertices)
			vertices.push_back({ glm::vec4(vertex.Position, 1.0f), glm::vec4(vertex.Normal, 0.0f) });
		for (uint32_t index : mesh.Indices)
			indices.push_back(vertexOffset + index);
	}

	Upload(m_VertexBuffer, Binding::Vertices, BindingTypes[Binding::Vertices], vertices.data(), vertices.size() * sizeof(VertexGPU));
	Upload(m_IndexBuffer, Binding::Indices, BindingTypes[Binding::Indices], indices.data(), indices.size() * sizeof(uint32_t));

	m_GeometryUploaded = true;
	m_UploadedGeometryVersion = scene.GeometryVersion;
}

void GpuPathTracer::Upload(Buffer& buffer, uint32_t binding, VkDescriptorType type, const void* data, VkDeviceSize size)
{
	// Zero-sized buffers are invalid, and every binding must stay valid even when e.g. the scene has no spheres
	VkDeviceSize requiredSize = std::max<VkDeviceSize>(size, 256);

	if (buffer.Handle == VK_NULL_HANDLE || buffer.Size < requiredSize)
	{
		if (buffer.Handle != VK_NULL_HANDLE)
		{
			Walnut::Application::SubmitResourceFree([old = buffer]()
			{
				VkDevice device = Walnut::Application::GetDevice();
				vkUnmapMemory(device, old.Memory);
				vkDestroyBuffer(device, old.Handle, nullptr);
				vkFreeMemory(device, old.Memory, nullptr);
			});
		}

		buffer.Size = std::max(requiredSize, buffer.Size * 2); // grow geometrically so adding objects one by one stays cheap
		CreateBuffer(buffer.Size, GetBufferUsage(type), buffer.Handle, buffer.Memory, &buffer.Mapped);
		WriteBufferDescriptor(binding, type, buffer);
	}

	if (size > 0)
		memcpy(buffer.Mapped, data, (size_t)size);
}

void GpuPathTracer::WriteBufferDescriptor(uint32_t binding, VkDescriptorType type, const Buffer& buffer)
{
	VkDescriptorBufferInfo bufferInfo{ buffer.Handle, 0, buffer.Size };

	VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	write.dstSet = m_DescriptorSet;
	write.dstBinding = binding;
	write.descriptorCount = 1;
	write.descriptorType = type;
	write.pBufferInfo = &bufferInfo;
	vkUpdateDescriptorSets(Walnut::Application::GetDevice(), 1, &write, 0, nullptr);
}

void GpuPathTracer::WriteImageDescriptor(uint32_t binding, VkImageView view)
{
	VkDescriptorImageInfo imageInfo{ VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL };

	VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	write.dstSet = m_DescriptorSet;
	write.dstBinding = binding;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	write.pImageInfo = &imageInfo;
	vkUpdateDescriptorSets(Walnut::Application::GetDevice(), 1, &write, 0, nullptr);
}

void GpuPathTracer::ReleaseImages()
{
	Walnut::Application::SubmitResourceFree([accumulation = m_AccumulationImage, display = m_DisplayImage]()
	{
		VkDevice device = Walnut::Application::GetDevice();
		for (const StorageImage& image : { accumulation, display })
		{
			vkDestroyImageView(device, image.View, nullptr);
			vkDestroyImage(device, image.Image, nullptr);
			vkFreeMemory(device, image.Memory, nullptr);
		}
	});

	m_AccumulationImage = {};
	m_DisplayImage = {};
}

void GpuPathTracer::CreateImages(uint32_t width, uint32_t height)
{
	if (m_AccumulationImage.Image != VK_NULL_HANDLE)
		ReleaseImages();

	CreateStorageImage(width, height, VK_FORMAT_R32G32B32A32_SFLOAT,
		VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		m_AccumulationImage.Image, m_AccumulationImage.Memory, m_AccumulationImage.View);

	CreateStorageImage(width, height, VK_FORMAT_R8G8B8A8_UNORM,
		VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		m_DisplayImage.Image, m_DisplayImage.Memory, m_DisplayImage.View);

	if (m_DisplaySampler == VK_NULL_HANDLE)
	{
		VkSamplerCreateInfo samplerInfo{};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.minLod = -1000;
		samplerInfo.maxLod = 1000;
		samplerInfo.maxAnisotropy = 1.0f;
		VkResult err = vkCreateSampler(Walnut::Application::GetDevice(), &samplerInfo, nullptr, &m_DisplaySampler);
		check_vk_result(err);
	}

	// Transition both images UNDEFINED -> GENERAL: GENERAL supports both compute shader read/write
	// and being sampled by ImGui's fragment shader, so it never needs to change again.
	VkCommandBuffer commandBuffer = Walnut::Application::GetCommandBuffer(true);

	VkImageMemoryBarrier barriers[2]{};
	for (auto& b : barriers)
	{
		b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		b.subresourceRange.levelCount = 1;
		b.subresourceRange.layerCount = 1;
	}
	barriers[0].image = m_AccumulationImage.Image;
	barriers[1].image = m_DisplayImage.Image;

	vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 0, nullptr, 0, nullptr, 2, barriers);

	Walnut::Application::FlushCommandBuffer(commandBuffer);

	// Register the display image directly with ImGui - no CPU readback, no Walnut::Image involved.
	m_DisplayDescriptorSet = (VkDescriptorSet)ImGui_ImplVulkan_AddTexture(m_DisplaySampler, m_DisplayImage.View, VK_IMAGE_LAYOUT_GENERAL);

	WriteImageDescriptor(Binding::AccumulationImage, m_AccumulationImage.View);
	WriteImageDescriptor(Binding::DisplayImage, m_DisplayImage.View);
}

void GpuPathTracer::ReadImage(VkImage image, VkDeviceSize bytesPerPixel, void* destination)
{
	VkDevice device = Walnut::Application::GetDevice();
	VkDeviceSize size = (VkDeviceSize)m_Width * m_Height * bytesPerPixel;

	VkBuffer buffer;
	VkDeviceMemory memory;
	void* mapped = nullptr;
	CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, buffer, memory, &mapped);

	VkCommandBuffer commandBuffer = Walnut::Application::GetCommandBuffer(true);

	// Compute writes -> transfer read (the image stays in GENERAL)
	VkImageMemoryBarrier imageBarrier{};
	imageBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	imageBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	imageBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	imageBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	imageBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	imageBarrier.image = image;
	imageBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	imageBarrier.subresourceRange.levelCount = 1;
	imageBarrier.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, nullptr, 0, nullptr, 1, &imageBarrier);

	VkBufferImageCopy region{};
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageExtent = { m_Width, m_Height, 1 };
	vkCmdCopyImageToBuffer(commandBuffer, image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &region);

	VkBufferMemoryBarrier bufferBarrier{};
	bufferBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	bufferBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	bufferBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	bufferBarrier.buffer = buffer;
	bufferBarrier.size = VK_WHOLE_SIZE;
	vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
		0, 0, nullptr, 1, &bufferBarrier, 0, nullptr);

	Walnut::Application::FlushCommandBuffer(commandBuffer); // waits for completion

	memcpy(destination, mapped, (size_t)size);

	vkUnmapMemory(device, memory);
	vkDestroyBuffer(device, buffer, nullptr);
	vkFreeMemory(device, memory, nullptr);
}

void GpuPathTracer::ReadDisplayImage(std::vector<uint32_t>& pixels)
{
	pixels.resize((size_t)m_Width * m_Height);
	if (m_DisplayImage.Image != VK_NULL_HANDLE)
		ReadImage(m_DisplayImage.Image, sizeof(uint32_t), pixels.data());
}

void GpuPathTracer::ReadAccumulationImage(std::vector<glm::vec4>& pixels)
{
	pixels.resize((size_t)m_Width * m_Height);
	if (m_AccumulationImage.Image != VK_NULL_HANDLE)
		ReadImage(m_AccumulationImage.Image, sizeof(glm::vec4), pixels.data());
}

void GpuPathTracer::OnResize(uint32_t width, uint32_t height)
{
	if (width == 0 || height == 0)
		return;

	if (!m_Initialized)
		Init();

	if (m_Width == width && m_Height == height)
		return;

	CreateImages(width, height);
	m_Width = width;
	m_Height = height;
}

void GpuPathTracer::Render(const Scene& scene, const Camera& camera, const RenderSettings& settings, const FrameParams& frame)
{
	if (!m_Initialized || m_Width == 0 || m_Height == 0)
		return;

	FrameUBOData frameData{};
	frameData.InverseProjection = camera.GetInverseProjection();
	frameData.InverseView = camera.GetInverseView();
	frameData.Position = glm::vec4(camera.GetPosition(), 1.0f);
	frameData.Lens = glm::vec4(camera.GetAperture(), camera.GetFocusDistance(), 0.0f, 0.0f);
	frameData.SkyBottomColor = glm::vec4(scene.Sky.BottomColor, scene.Sky.Intensity);
	frameData.SkyTopColor = glm::vec4(scene.Sky.TopColor, 0.0f);
	frameData.SkyMode = (int32_t)scene.Sky.Mode;
	Upload(m_FrameBuffer, Binding::Frame, BindingTypes[Binding::Frame], &frameData, sizeof(frameData));

	std::vector<SphereGPU> spheres(scene.Spheres.size());
	for (size_t i = 0; i < spheres.size(); i++)
	{
		spheres[i].PositionAndRadius = glm::vec4(scene.Spheres[i].Position, scene.Spheres[i].Radius);
		spheres[i].MaterialIndex = glm::ivec4(scene.Spheres[i].MaterialIndex, 0, 0, 0);
	}
	Upload(m_SphereBuffer, Binding::Spheres, BindingTypes[Binding::Spheres], spheres.data(), spheres.size() * sizeof(SphereGPU));

	std::vector<MaterialGPU> materials(scene.Materials.size());
	for (size_t i = 0; i < materials.size(); i++)
	{
		const Material& material = scene.Materials[i];
		materials[i].AlbedoRoughness = glm::vec4(material.Albedo, material.Roughness);
		materials[i].EmissionAndPower = glm::vec4(material.EmissionColor, material.EmissionPower);
		materials[i].MetallicTransmissionIOR = glm::vec4(material.Metallic, material.Transmission, material.IOR, 0.0f);
	}
	Upload(m_MaterialBuffer, Binding::Materials, BindingTypes[Binding::Materials], materials.data(), materials.size() * sizeof(MaterialGPU));

	std::vector<PlaneGPU> planes(scene.Planes.size());
	for (size_t i = 0; i < planes.size(); i++)
	{
		planes[i].Point = glm::vec4(scene.Planes[i].Point, 1.0f);
		planes[i].Normal = glm::vec4(scene.Planes[i].Normal, 0.0f);
		planes[i].MaterialIndex = glm::ivec4(scene.Planes[i].MaterialIndex, 0, 0, 0);
	}
	Upload(m_PlaneBuffer, Binding::Planes, BindingTypes[Binding::Planes], planes.data(), planes.size() * sizeof(PlaneGPU));

	UploadGeometry(scene);

	std::vector<InstanceGPU> instances(scene.MeshInstances.size());
	for (size_t i = 0; i < instances.size(); i++)
	{
		const MeshInstance& instance = scene.MeshInstances[i];
		const Mesh& mesh = scene.Meshes[instance.MeshIndex];
		instances[i].ObjectToWorld = instance.Transform.GetMatrix();
		instances[i].WorldToObject = glm::inverse(instances[i].ObjectToWorld);
		instances[i].BoundsMin = glm::vec4(mesh.BoundsMin, 0.0f);
		instances[i].BoundsMax = glm::vec4(mesh.BoundsMax, 0.0f);
		instances[i].Info = glm::uvec4(m_MeshFirstTriangle[instance.MeshIndex], mesh.GetTriangleCount(), (uint32_t)instance.MaterialIndex, 0u);
	}
	Upload(m_InstanceBuffer, Binding::Instances, BindingTypes[Binding::Instances], instances.data(), instances.size() * sizeof(InstanceGPU));

	PushConstants pushConstants{};
	pushConstants.Width = m_Width;
	pushConstants.Height = m_Height;
	pushConstants.FirstSampleIndex = frame.FirstSampleIndex;
	pushConstants.SphereCount = (uint32_t)spheres.size();
	pushConstants.Bounces = (uint32_t)std::max(settings.MaxBounces, 1);
	pushConstants.RussianRoulette = settings.RussianRoulette ? 1u : 0u;
	pushConstants.RussianRouletteStartBounce = (uint32_t)std::max(settings.RussianRouletteStartBounce, 0);
	pushConstants.SampleCount = frame.SampleCount;
	pushConstants.ResetAccumulation = frame.ResetAccumulation ? 1u : 0u;
	pushConstants.Exposure = settings.Exposure;
	pushConstants.ToneMapper = (uint32_t)settings.ToneMapping;
	pushConstants.SRGBOutput = settings.SRGBOutput ? 1u : 0u;
	pushConstants.AntiAliasing = settings.AntiAliasing ? 1u : 0u;
	pushConstants.PlaneCount = (uint32_t)planes.size();
	pushConstants.InstanceCount = (uint32_t)instances.size();

	VkCommandBuffer commandBuffer = Walnut::Application::GetCommandBuffer(true);

	vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_Pipeline);
	vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout, 0, 1, &m_DescriptorSet, 0, nullptr);
	vkCmdPushConstants(commandBuffer, m_PipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants), &pushConstants);

	uint32_t groupsX = (m_Width + 7) / 8;
	uint32_t groupsY = (m_Height + 7) / 8;
	vkCmdDispatch(commandBuffer, groupsX, groupsY, 1);

	// Make the compute writes to DisplayImage visible to the fragment shader ImGui uses to sample it
	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = m_DisplayImage.Image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		0, 0, nullptr, 0, nullptr, 1, &barrier);

	Walnut::Application::FlushCommandBuffer(commandBuffer);
}
