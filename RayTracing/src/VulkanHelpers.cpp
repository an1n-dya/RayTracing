#include "VulkanHelpers.h"

#include "Walnut/Application.h"

#include <fstream>
#include <stdexcept>

namespace VulkanHelpers {

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
void CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory, void** mapped, bool deviceAddress)
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

	VkMemoryAllocateFlagsInfo flagsInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
	flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
	if (deviceAddress)
		allocInfo.pNext = &flagsInfo;

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

void CreateDeviceLocalBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory, bool deviceAddress)
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

	VkMemoryAllocateFlagsInfo flagsInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
	flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
	if (deviceAddress)
		allocInfo.pNext = &flagsInfo;
	allocInfo.memoryTypeIndex = FindMemoryType(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memReq.memoryTypeBits);
	err = vkAllocateMemory(device, &allocInfo, nullptr, &memory);
	check_vk_result(err);

	err = vkBindBufferMemory(device, buffer, memory, 0);
	check_vk_result(err);
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

void CreateComputePipeline(VkShaderModule module, VkDescriptorSetLayout setLayout, uint32_t pushConstantSize,
	VkPipelineLayout& pipelineLayout, VkPipeline& pipeline)
{
	VkDevice device = Walnut::Application::GetDevice();

	VkPushConstantRange pushConstantRange{};
	pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pushConstantRange.offset = 0;
	pushConstantRange.size = pushConstantSize;

	VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &setLayout;
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
	VkResult err = vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout);
	check_vk_result(err);

	VkPipelineShaderStageCreateInfo stageInfo{};
	stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	stageInfo.module = module;
	stageInfo.pName = "main";

	VkComputePipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipelineInfo.stage = stageInfo;
	pipelineInfo.layout = pipelineLayout;
	err = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
	check_vk_result(err);
}

VkDeviceAddress GetBufferAddress(VkBuffer buffer)
{
	VkBufferDeviceAddressInfo info{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
	info.buffer = buffer;
	return vkGetBufferDeviceAddress(Walnut::Application::GetDevice(), &info);
}

}
