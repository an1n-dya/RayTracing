#include "RayQueryScene.h"

#include "VulkanHelpers.h"

#include "Walnut/Application.h"

#include <algorithm>
#include <cstring>

using namespace VulkanHelpers;

namespace {

	// Extension entry points aren't exported by the loader library, so fetch them from the device
	PFN_vkCreateAccelerationStructureKHR s_CreateAccelerationStructure = nullptr;
	PFN_vkDestroyAccelerationStructureKHR s_DestroyAccelerationStructure = nullptr;
	PFN_vkGetAccelerationStructureBuildSizesKHR s_GetBuildSizes = nullptr;
	PFN_vkCmdBuildAccelerationStructuresKHR s_CmdBuildAccelerationStructures = nullptr;
	PFN_vkGetAccelerationStructureDeviceAddressKHR s_GetAccelerationStructureAddress = nullptr;

	void LoadFunctions() {
		VkDevice device = Walnut::Application::GetDevice();
		s_CreateAccelerationStructure = (PFN_vkCreateAccelerationStructureKHR)vkGetDeviceProcAddr(device, "vkCreateAccelerationStructureKHR");
		s_DestroyAccelerationStructure = (PFN_vkDestroyAccelerationStructureKHR)vkGetDeviceProcAddr(device, "vkDestroyAccelerationStructureKHR");
		s_GetBuildSizes = (PFN_vkGetAccelerationStructureBuildSizesKHR)vkGetDeviceProcAddr(device, "vkGetAccelerationStructureBuildSizesKHR");
		s_CmdBuildAccelerationStructures = (PFN_vkCmdBuildAccelerationStructuresKHR)vkGetDeviceProcAddr(device, "vkCmdBuildAccelerationStructuresKHR");
		s_GetAccelerationStructureAddress = (PFN_vkGetAccelerationStructureDeviceAddressKHR)vkGetDeviceProcAddr(device, "vkGetAccelerationStructureDeviceAddressKHR");
	}

	// Makes one acceleration structure build visible to the next build (they share the scratch buffer) and to shaders
	void BuildBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
		VkMemoryBarrier barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
		barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
		barrier.dstAccessMask = dstAccess;
		vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, dstStage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
	}

}

RayQueryScene::RayQueryScene() {
	LoadFunctions();

	VkPhysicalDeviceAccelerationStructurePropertiesKHR accelerationStructureProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR };
	VkPhysicalDeviceProperties2 properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
	properties.pNext = &accelerationStructureProperties;
	vkGetPhysicalDeviceProperties2(Walnut::Application::GetPhysicalDevice(), &properties);
	m_ScratchAlignment = std::max<VkDeviceSize>(accelerationStructureProperties.minAccelerationStructureScratchOffsetAlignment, 1);

	BuildSphereBox();
}

RayQueryScene::~RayQueryScene() {
	for (AccelerationStructure& structure : m_MeshStructures)
		Release(structure);
	Release(m_SphereBox);
	Release(m_TopLevel);
	Release(m_InstanceBuffer);
	Release(m_SphereBoxBuffer);
	if (m_ScratchBuffer != VK_NULL_HANDLE) {
		Walnut::Application::SubmitResourceFree([buffer = m_ScratchBuffer, memory = m_ScratchMemory]() {
			vkDestroyBuffer(Walnut::Application::GetDevice(), buffer, nullptr);
			vkFreeMemory(Walnut::Application::GetDevice(), memory, nullptr);
		});
	}
}

RayQueryScene::AccelerationStructure RayQueryScene::CreateAccelerationStructure(VkAccelerationStructureTypeKHR type, VkDeviceSize size) {
	VkDevice device = Walnut::Application::GetDevice();

	AccelerationStructure structure;
	CreateDeviceLocalBuffer(size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		structure.Buffer, structure.Memory, true);

	VkAccelerationStructureCreateInfoKHR createInfo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
	createInfo.buffer = structure.Buffer;
	createInfo.size = size;
	createInfo.type = type;
	VkResult err = s_CreateAccelerationStructure(device, &createInfo, nullptr, &structure.Handle);
	check_vk_result(err);

	VkAccelerationStructureDeviceAddressInfoKHR addressInfo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
	addressInfo.accelerationStructure = structure.Handle;
	structure.Address = s_GetAccelerationStructureAddress(device, &addressInfo);
	return structure;
}

void RayQueryScene::Release(AccelerationStructure& structure) {
	if (structure.Handle == VK_NULL_HANDLE)
		return;
	Walnut::Application::SubmitResourceFree([structure]() {
		VkDevice device = Walnut::Application::GetDevice();
		s_DestroyAccelerationStructure(device, structure.Handle, nullptr);
		vkDestroyBuffer(device, structure.Buffer, nullptr);
		vkFreeMemory(device, structure.Memory, nullptr);
	});
	structure = {};
}

void RayQueryScene::Release(HostBuffer& buffer) {
	if (buffer.Buffer == VK_NULL_HANDLE)
		return;
	Walnut::Application::SubmitResourceFree([buffer]() {
		VkDevice device = Walnut::Application::GetDevice();
		vkUnmapMemory(device, buffer.Memory);
		vkDestroyBuffer(device, buffer.Buffer, nullptr);
		vkFreeMemory(device, buffer.Memory, nullptr);
	});
	buffer = {};
}

void RayQueryScene::EnsureScratch(VkDeviceSize size) {
	VkDeviceSize required = size + m_ScratchAlignment; // room to align the start
	if (m_ScratchBuffer != VK_NULL_HANDLE && m_ScratchSize >= required)
		return;

	if (m_ScratchBuffer != VK_NULL_HANDLE) {
		Walnut::Application::SubmitResourceFree([buffer = m_ScratchBuffer, memory = m_ScratchMemory]() {
			vkDestroyBuffer(Walnut::Application::GetDevice(), buffer, nullptr);
			vkFreeMemory(Walnut::Application::GetDevice(), memory, nullptr);
		});
	}
	m_ScratchSize = std::max(required, m_ScratchSize * 2);
	CreateDeviceLocalBuffer(m_ScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		m_ScratchBuffer, m_ScratchMemory, true);
}

void RayQueryScene::BuildSphereBox() {
	VkDevice device = Walnut::Application::GetDevice();

	VkAabbPositionsKHR box{ -1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
	m_SphereBoxBuffer.Size = sizeof(box);
	CreateBuffer(m_SphereBoxBuffer.Size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		m_SphereBoxBuffer.Buffer, m_SphereBoxBuffer.Memory, &m_SphereBoxBuffer.Mapped, true);
	memcpy(m_SphereBoxBuffer.Mapped, &box, sizeof(box));

	VkAccelerationStructureGeometryKHR geometry{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
	geometry.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
	geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	geometry.geometry.aabbs.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
	geometry.geometry.aabbs.data.deviceAddress = GetBufferAddress(m_SphereBoxBuffer.Buffer);
	geometry.geometry.aabbs.stride = sizeof(VkAabbPositionsKHR);

	VkAccelerationStructureBuildGeometryInfoKHR buildInfo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
	buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	buildInfo.geometryCount = 1;
	buildInfo.pGeometries = &geometry;

	uint32_t primitiveCount = 1;
	VkAccelerationStructureBuildSizesInfoKHR sizes{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
	s_GetBuildSizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &primitiveCount, &sizes);

	m_SphereBox = CreateAccelerationStructure(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, sizes.accelerationStructureSize);
	EnsureScratch(sizes.buildScratchSize);
	buildInfo.dstAccelerationStructure = m_SphereBox.Handle;
	buildInfo.scratchData.deviceAddress = (GetBufferAddress(m_ScratchBuffer) + m_ScratchAlignment - 1) & ~(m_ScratchAlignment - 1);

	VkAccelerationStructureBuildRangeInfoKHR range{ primitiveCount, 0, 0, 0 };
	const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
	VkCommandBuffer commandBuffer = Walnut::Application::GetCommandBuffer(true);
	s_CmdBuildAccelerationStructures(commandBuffer, 1, &buildInfo, &ranges);
	BuildBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
	Walnut::Application::FlushCommandBuffer(commandBuffer);
}

void RayQueryScene::BuildMeshes(const Scene& scene, VkBuffer vertexBuffer, uint32_t vertexCount, VkBuffer indexBuffer,
	const std::vector<uint32_t>& meshFirstTriangle)
{
	VkDevice device = Walnut::Application::GetDevice();

	for (AccelerationStructure& structure : m_MeshStructures)
		Release(structure);
	m_MeshStructures.assign(scene.Meshes.size(), {});
	if (vertexCount == 0)
		return;

	VkDeviceAddress vertexAddress = GetBufferAddress(vertexBuffer);
	VkDeviceAddress indexAddress = GetBufferAddress(indexBuffer);

	// Set up every build first (the geometry descriptions must stay alive until they're recorded)
	std::vector<VkAccelerationStructureGeometryKHR> geometries(scene.Meshes.size());
	std::vector<VkAccelerationStructureBuildGeometryInfoKHR> buildInfos(scene.Meshes.size());
	std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(scene.Meshes.size());
	VkDeviceSize scratchSize = 0;
	for (size_t i = 0; i < scene.Meshes.size(); i++) {
		uint32_t triangleCount = scene.Meshes[i].GetTriangleCount();
		if (triangleCount == 0)
			continue;

		VkAccelerationStructureGeometryKHR& geometry = geometries[i];
		geometry = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
		geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
		geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
		VkAccelerationStructureGeometryTrianglesDataKHR& triangles = geometry.geometry.triangles;
		triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
		triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
		triangles.vertexData.deviceAddress = vertexAddress;
		triangles.vertexStride = 32; // VertexGPU: vec4 position, vec4 normal
		triangles.maxVertex = vertexCount - 1;
		triangles.indexType = VK_INDEX_TYPE_UINT32;
		triangles.indexData.deviceAddress = indexAddress + (VkDeviceAddress)meshFirstTriangle[i] * 3 * sizeof(uint32_t);

		VkAccelerationStructureBuildGeometryInfoKHR& buildInfo = buildInfos[i];
		buildInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
		buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
		buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
		buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
		buildInfo.geometryCount = 1;
		buildInfo.pGeometries = &geometry;

		VkAccelerationStructureBuildSizesInfoKHR sizes{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
		s_GetBuildSizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &triangleCount, &sizes);

		m_MeshStructures[i] = CreateAccelerationStructure(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, sizes.accelerationStructureSize);
		buildInfo.dstAccelerationStructure = m_MeshStructures[i].Handle;
		ranges[i] = { triangleCount, 0, 0, 0 };
		scratchSize = std::max(scratchSize, sizes.buildScratchSize);
	}

	EnsureScratch(scratchSize);
	VkDeviceAddress scratchAddress = (GetBufferAddress(m_ScratchBuffer) + m_ScratchAlignment - 1) & ~(m_ScratchAlignment - 1);

	VkCommandBuffer commandBuffer = Walnut::Application::GetCommandBuffer(true);
	for (size_t i = 0; i < scene.Meshes.size(); i++) {
		if (m_MeshStructures[i].Handle == VK_NULL_HANDLE)
			continue;
		buildInfos[i].scratchData.deviceAddress = scratchAddress;
		const VkAccelerationStructureBuildRangeInfoKHR* range = &ranges[i];
		s_CmdBuildAccelerationStructures(commandBuffer, 1, &buildInfos[i], &range);
		// The builds share the scratch buffer, so each must finish before the next starts
		BuildBarrier(commandBuffer, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_SHADER_READ_BIT);
	}
	Walnut::Application::FlushCommandBuffer(commandBuffer);
}

bool RayQueryScene::BuildTopLevel(VkCommandBuffer commandBuffer, const Scene& scene, const PreparedScene& prepared) {
	VkDevice device = Walnut::Application::GetDevice();

	// Mesh instances (custom index = mesh instance index) then spheres (custom index = sphere index). The shader tells
	// them apart by the hit type: meshes only produce triangle hits, spheres only bounding box candidates.
	std::vector<VkAccelerationStructureInstanceKHR> instances;
	instances.reserve(scene.MeshInstances.size() + scene.Spheres.size());
	for (size_t i = 0; i < scene.MeshInstances.size(); i++) {
		const AccelerationStructure& structure = m_MeshStructures[scene.MeshInstances[i].MeshIndex];
		if (structure.Handle == VK_NULL_HANDLE)
			continue;

		VkAccelerationStructureInstanceKHR instance{};
		const glm::mat4& transform = prepared.Instances[i].ObjectToWorld;
		for (int row = 0; row < 3; row++)
			for (int column = 0; column < 4; column++)
				instance.transform.matrix[row][column] = transform[column][row]; // glm is column-major
		instance.instanceCustomIndex = (uint32_t)i;
		instance.mask = 0xFF;
		instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
		instance.accelerationStructureReference = structure.Address;
		instances.push_back(instance);
	}
	for (size_t i = 0; i < scene.Spheres.size(); i++) {
		const Sphere& sphere = scene.Spheres[i];
		VkAccelerationStructureInstanceKHR instance{};
		instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = instance.transform.matrix[2][2] = sphere.Radius;
		instance.transform.matrix[0][3] = sphere.Position.x;
		instance.transform.matrix[1][3] = sphere.Position.y;
		instance.transform.matrix[2][3] = sphere.Position.z;
		instance.instanceCustomIndex = (uint32_t)i;
		instance.mask = 0xFF;
		instance.accelerationStructureReference = m_SphereBox.Address;
		instances.push_back(instance);
	}
	uint32_t instanceCount = (uint32_t)instances.size();

	VkDeviceSize instanceBytes = std::max<VkDeviceSize>(instanceCount, 1) * sizeof(VkAccelerationStructureInstanceKHR);
	if (m_InstanceBuffer.Size < instanceBytes) {
		Release(m_InstanceBuffer);
		m_InstanceBuffer.Size = instanceBytes * 2;
		CreateBuffer(m_InstanceBuffer.Size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
			m_InstanceBuffer.Buffer, m_InstanceBuffer.Memory, &m_InstanceBuffer.Mapped, true);
	}
	if (instanceCount > 0)
		memcpy(m_InstanceBuffer.Mapped, instances.data(), instanceCount * sizeof(VkAccelerationStructureInstanceKHR));

	VkAccelerationStructureGeometryKHR geometry{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
	geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	geometry.geometry.instances.data.deviceAddress = GetBufferAddress(m_InstanceBuffer.Buffer);

	VkAccelerationStructureBuildGeometryInfoKHR buildInfo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
	buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	buildInfo.geometryCount = 1;
	buildInfo.pGeometries = &geometry;

	// Size the structure (and scratch) for some headroom, so adding objects doesn't recreate it every time
	bool recreated = false;
	if (m_TopLevel.Handle == VK_NULL_HANDLE || instanceCount > m_TopLevelCapacity) {
		uint32_t capacity = std::max(instanceCount, std::max(16u, m_TopLevelCapacity * 2));
		VkAccelerationStructureBuildSizesInfoKHR sizes{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
		s_GetBuildSizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &capacity, &sizes);

		Release(m_TopLevel);
		m_TopLevel = CreateAccelerationStructure(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, sizes.accelerationStructureSize);
		m_TopLevelCapacity = capacity;
		EnsureScratch(sizes.buildScratchSize);
		recreated = true;
	}

	buildInfo.dstAccelerationStructure = m_TopLevel.Handle;
	buildInfo.scratchData.deviceAddress = (GetBufferAddress(m_ScratchBuffer) + m_ScratchAlignment - 1) & ~(m_ScratchAlignment - 1);

	VkAccelerationStructureBuildRangeInfoKHR range{ instanceCount, 0, 0, 0 };
	const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
	s_CmdBuildAccelerationStructures(commandBuffer, 1, &buildInfo, &ranges);
	BuildBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);

	return recreated;
}
