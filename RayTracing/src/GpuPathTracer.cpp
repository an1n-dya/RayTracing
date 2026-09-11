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

	// Mirrors the std430/std140 layouts declared in PathTrace.comp
	struct SphereGPU {
		glm::vec4 PositionAndRadius; // xyz = position, w = radius
		glm::ivec4 MaterialIndex;    // x = material index
	};

	struct MaterialGPU {
		glm::vec4 AlbedoRoughness;  // rgb = albedo, w = roughness
		glm::vec4 EmissionAndPower; // rgb = emission color, w = emission power
		glm::vec4 MetallicPad;      // x = metallic
	};

	struct CameraUBOData {
		glm::mat4 InverseProjection;
		glm::mat4 InverseView;
		glm::vec4 Position;
	};

	struct PushConstants {
		uint32_t Width;
		uint32_t Height;
		uint32_t FrameIndex;
		uint32_t SphereCount;
		uint32_t Bounces;
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
		allocInfo.memoryTypeIndex = FindMemoryType(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, memReq.memoryTypeBits);
		err = vkAllocateMemory(device, &allocInfo, nullptr, &memory);
		check_vk_result(err);

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

}

GpuPathTracer::~GpuPathTracer()
{
	if (!m_Initialized)
		return;

	if (m_AccumulationImage != VK_NULL_HANDLE)
		ReleaseImages();

	Walnut::Application::SubmitResourceFree([shaderModule = m_ShaderModule, pipeline = m_Pipeline,
		pipelineLayout = m_PipelineLayout, descriptorSetLayout = m_DescriptorSetLayout, descriptorPool = m_DescriptorPool,
		sampler = m_DisplaySampler, cameraUBO = m_CameraUBO, cameraUBOMemory = m_CameraUBOMemory,
		sphereSSBO = m_SphereSSBO, sphereSSBOMemory = m_SphereSSBOMemory,
		materialSSBO = m_MaterialSSBO, materialSSBOMemory = m_MaterialSSBOMemory]()
	{
		VkDevice device = Walnut::Application::GetDevice();

		vkDestroyPipeline(device, pipeline, nullptr);
		vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
		vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
		vkDestroyDescriptorPool(device, descriptorPool, nullptr); // also frees the set allocated from it
		vkDestroyShaderModule(device, shaderModule, nullptr);
		if (sampler != VK_NULL_HANDLE)
			vkDestroySampler(device, sampler, nullptr);

		vkUnmapMemory(device, cameraUBOMemory);
		vkDestroyBuffer(device, cameraUBO, nullptr);
		vkFreeMemory(device, cameraUBOMemory, nullptr);

		vkUnmapMemory(device, sphereSSBOMemory);
		vkDestroyBuffer(device, sphereSSBO, nullptr);
		vkFreeMemory(device, sphereSSBOMemory, nullptr);

		vkUnmapMemory(device, materialSSBOMemory);
		vkDestroyBuffer(device, materialSSBO, nullptr);
		vkFreeMemory(device, materialSSBOMemory, nullptr);
	});
}

void GpuPathTracer::Init()
{
	VkDevice device = Walnut::Application::GetDevice();

	auto shaderCode = ReadFile("src/shaders/PathTrace.comp.spv");
	m_ShaderModule = CreateShaderModule(shaderCode);

	VkDescriptorSetLayoutBinding bindings[5]{};
	bindings[0] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
	bindings[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
	bindings[2] = { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
	bindings[3] = { 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
	bindings[4] = { 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

	VkDescriptorSetLayoutCreateInfo layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = 5;
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

	VkDescriptorPoolSize poolSizes[3]{};
	poolSizes[0] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 };
	poolSizes[1] = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 };
	poolSizes[2] = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 };

	VkDescriptorPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = 1;
	poolInfo.poolSizeCount = 3;
	poolInfo.pPoolSizes = poolSizes;
	err = vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_DescriptorPool);
	check_vk_result(err);

	VkDescriptorSetAllocateInfo dsAllocInfo{};
	dsAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dsAllocInfo.descriptorPool = m_DescriptorPool;
	dsAllocInfo.descriptorSetCount = 1;
	dsAllocInfo.pSetLayouts = &m_DescriptorSetLayout;
	err = vkAllocateDescriptorSets(device, &dsAllocInfo, &m_DescriptorSet);
	check_vk_result(err);

	CreateBuffer(sizeof(CameraUBOData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, m_CameraUBO, m_CameraUBOMemory, &m_CameraUBOMapped);
	CreateBuffer(MaxSpheres * sizeof(SphereGPU), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_SphereSSBO, m_SphereSSBOMemory, &m_SphereSSBOMapped);
	CreateBuffer(MaxMaterials * sizeof(MaterialGPU), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_MaterialSSBO, m_MaterialSSBOMemory, &m_MaterialSSBOMapped);

	VkDescriptorBufferInfo cameraBufferInfo{ m_CameraUBO, 0, sizeof(CameraUBOData) };
	VkDescriptorBufferInfo sphereBufferInfo{ m_SphereSSBO, 0, MaxSpheres * sizeof(SphereGPU) };
	VkDescriptorBufferInfo materialBufferInfo{ m_MaterialSSBO, 0, MaxMaterials * sizeof(MaterialGPU) };

	VkWriteDescriptorSet writes[3]{};
	writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	writes[0].dstSet = m_DescriptorSet;
	writes[0].dstBinding = 0;
	writes[0].descriptorCount = 1;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	writes[0].pBufferInfo = &cameraBufferInfo;

	writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	writes[1].dstSet = m_DescriptorSet;
	writes[1].dstBinding = 1;
	writes[1].descriptorCount = 1;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[1].pBufferInfo = &sphereBufferInfo;

	writes[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	writes[2].dstSet = m_DescriptorSet;
	writes[2].dstBinding = 2;
	writes[2].descriptorCount = 1;
	writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[2].pBufferInfo = &materialBufferInfo;

	vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

	m_Initialized = true;
}

void GpuPathTracer::ReleaseImages()
{
	Walnut::Application::SubmitResourceFree([accumImage = m_AccumulationImage, accumMemory = m_AccumulationImageMemory,
		accumView = m_AccumulationImageView, displayImage = m_DisplayImage, displayMemory = m_DisplayImageMemory,
		displayView = m_DisplayImageView]()
	{
		VkDevice device = Walnut::Application::GetDevice();
		vkDestroyImageView(device, accumView, nullptr);
		vkDestroyImage(device, accumImage, nullptr);
		vkFreeMemory(device, accumMemory, nullptr);
		vkDestroyImageView(device, displayView, nullptr);
		vkDestroyImage(device, displayImage, nullptr);
		vkFreeMemory(device, displayMemory, nullptr);
	});

	m_AccumulationImage = VK_NULL_HANDLE;
	m_AccumulationImageMemory = VK_NULL_HANDLE;
	m_AccumulationImageView = VK_NULL_HANDLE;
	m_DisplayImage = VK_NULL_HANDLE;
	m_DisplayImageMemory = VK_NULL_HANDLE;
	m_DisplayImageView = VK_NULL_HANDLE;
}

void GpuPathTracer::CreateImages(uint32_t width, uint32_t height)
{
	if (m_AccumulationImage != VK_NULL_HANDLE)
		ReleaseImages();

	CreateStorageImage(width, height, VK_FORMAT_R32G32B32A32_SFLOAT,
		VK_IMAGE_USAGE_STORAGE_BIT, m_AccumulationImage, m_AccumulationImageMemory, m_AccumulationImageView);

	CreateStorageImage(width, height, VK_FORMAT_R8G8B8A8_UNORM,
		VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, m_DisplayImage, m_DisplayImageMemory, m_DisplayImageView);

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
	barriers[0].image = m_AccumulationImage;
	barriers[1].image = m_DisplayImage;

	vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 0, nullptr, 0, nullptr, 2, barriers);

	Walnut::Application::FlushCommandBuffer(commandBuffer);

	// Register the display image directly with ImGui - no CPU readback, no Walnut::Image involved.
	m_DisplayDescriptorSet = (VkDescriptorSet)ImGui_ImplVulkan_AddTexture(m_DisplaySampler, m_DisplayImageView, VK_IMAGE_LAYOUT_GENERAL);

	VkDescriptorImageInfo accumImageInfo{ VK_NULL_HANDLE, m_AccumulationImageView, VK_IMAGE_LAYOUT_GENERAL };
	VkDescriptorImageInfo displayImageInfo{ VK_NULL_HANDLE, m_DisplayImageView, VK_IMAGE_LAYOUT_GENERAL };

	VkWriteDescriptorSet writes[2]{};
	writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	writes[0].dstSet = m_DescriptorSet;
	writes[0].dstBinding = 3;
	writes[0].descriptorCount = 1;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	writes[0].pImageInfo = &accumImageInfo;

	writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	writes[1].dstSet = m_DescriptorSet;
	writes[1].dstBinding = 4;
	writes[1].descriptorCount = 1;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	writes[1].pImageInfo = &displayImageInfo;

	vkUpdateDescriptorSets(Walnut::Application::GetDevice(), 2, writes, 0, nullptr);
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

void GpuPathTracer::Render(const Scene& scene, const Camera& camera, uint32_t frameIndex)
{
	if (!m_Initialized || m_Width == 0 || m_Height == 0)
		return;

	CameraUBOData cameraData{};
	cameraData.InverseProjection = camera.GetInverseProjection();
	cameraData.InverseView = camera.GetInverseView();
	cameraData.Position = glm::vec4(camera.GetPosition(), 1.0f);
	memcpy(m_CameraUBOMapped, &cameraData, sizeof(cameraData));

	uint32_t sphereCount = std::min((uint32_t)scene.Spheres.size(), MaxSpheres);
	if (sphereCount > 0)
	{
		std::vector<SphereGPU> spheres(sphereCount);
		for (uint32_t i = 0; i < sphereCount; i++)
		{
			spheres[i].PositionAndRadius = glm::vec4(scene.Spheres[i].Position, scene.Spheres[i].Radius);
			spheres[i].MaterialIndex = glm::ivec4(scene.Spheres[i].MaterialIndex, 0, 0, 0);
		}
		memcpy(m_SphereSSBOMapped, spheres.data(), sphereCount * sizeof(SphereGPU));
	}

	uint32_t materialCount = std::min((uint32_t)scene.Materials.size(), MaxMaterials);
	if (materialCount > 0)
	{
		std::vector<MaterialGPU> materials(materialCount);
		for (uint32_t i = 0; i < materialCount; i++)
		{
			const Material& material = scene.Materials[i];
			materials[i].AlbedoRoughness = glm::vec4(material.Albedo, material.Roughness);
			materials[i].EmissionAndPower = glm::vec4(material.EmissionColor, material.EmissionPower);
			materials[i].MetallicPad = glm::vec4(material.Metallic, 0.0f, 0.0f, 0.0f);
		}
		memcpy(m_MaterialSSBOMapped, materials.data(), materialCount * sizeof(MaterialGPU));
	}

	PushConstants pushConstants{};
	pushConstants.Width = m_Width;
	pushConstants.Height = m_Height;
	pushConstants.FrameIndex = frameIndex;
	pushConstants.SphereCount = sphereCount;
	pushConstants.Bounces = Bounces;

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
	barrier.image = m_DisplayImage;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		0, 0, nullptr, 0, nullptr, 1, &barrier);

	Walnut::Application::FlushCommandBuffer(commandBuffer);
}
