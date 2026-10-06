#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

// Small Vulkan helpers shared by the GPU path tracer and its acceleration structures. All use Walnut's device.
namespace VulkanHelpers {

	uint32_t FindMemoryType(VkMemoryPropertyFlags properties, uint32_t typeBits);

	// Host-visible + coherent buffer, preferring memory that is also device-local (resizable BAR). mapped (optional)
	// receives a persistent mapping. deviceAddress: allocate so vkGetBufferDeviceAddress works (the usage must
	// include VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT).
	void CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory, void** mapped,
		bool deviceAddress = false);

	// Device-local only buffer (fill it with a transfer)
	void CreateDeviceLocalBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory,
		bool deviceAddress = false);

	VkDeviceAddress GetBufferAddress(VkBuffer buffer);

	void CreateStorageImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
		VkImage& image, VkDeviceMemory& memory, VkImageView& view);

	std::vector<char> ReadFile(const std::string& path);
	VkShaderModule CreateShaderModule(const std::vector<char>& code);
	void CreateComputePipeline(VkShaderModule module, VkDescriptorSetLayout setLayout, uint32_t pushConstantSize,
		VkPipelineLayout& pipelineLayout, VkPipeline& pipeline);

}
