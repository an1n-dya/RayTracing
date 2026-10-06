#pragma once

#include "PreparedScene.h"
#include "Scene.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

// Vulkan acceleration structures for hardware ray tracing in the compute shader (VK_KHR_ray_query):
//   - one bottom-level structure per mesh (triangles, read straight from the path tracer's vertex/index buffers)
//   - one for the bounding box of a unit sphere, instanced per sphere (the shader intersects the actual sphere)
//   - a top-level structure over all mesh instances and spheres, rebuilt every frame
// Planes are infinite, so the shader keeps testing those itself.
// Only create this when WalnutExtensions::IsRayQuerySupported().
class RayQueryScene {
public:
	RayQueryScene();
	~RayQueryScene();

	RayQueryScene(const RayQueryScene&) = delete;
	RayQueryScene& operator=(const RayQueryScene&) = delete;

	// (Re)builds the per-mesh structures. vertexBuffer / indexBuffer hold every mesh concatenated, as uploaded by
	// GpuPathTracer: 32-byte vertices starting with the position, global vertex indices, meshFirstTriangle[i] =
	// mesh i's first triangle. Both need device addresses and acceleration structure build input usage.
	void BuildMeshes(const Scene& scene, VkBuffer vertexBuffer, uint32_t vertexCount, VkBuffer indexBuffer,
		const std::vector<uint32_t>& meshFirstTriangle);

	// Records this frame's top-level build into commandBuffer (followed by a barrier for compute shader reads).
	// Instance custom indices are the mesh instance / sphere index. Returns true when the top-level structure was
	// recreated, i.e. descriptors pointing at it must be rewritten (before the command buffer binds them).
	bool BuildTopLevel(VkCommandBuffer commandBuffer, const Scene& scene, const PreparedScene& prepared);

	VkAccelerationStructureKHR GetTopLevel() const { return m_TopLevel.Handle; }

private:
	struct AccelerationStructure {
		VkAccelerationStructureKHR Handle = VK_NULL_HANDLE;
		VkBuffer Buffer = VK_NULL_HANDLE;
		VkDeviceMemory Memory = VK_NULL_HANDLE;
		VkDeviceAddress Address = 0;
	};

	struct HostBuffer {
		VkBuffer Buffer = VK_NULL_HANDLE;
		VkDeviceMemory Memory = VK_NULL_HANDLE;
		void* Mapped = nullptr;
		VkDeviceSize Size = 0;
	};

	AccelerationStructure CreateAccelerationStructure(VkAccelerationStructureTypeKHR type, VkDeviceSize size);
	static void Release(AccelerationStructure& structure);
	static void Release(HostBuffer& buffer);
	void EnsureScratch(VkDeviceSize size);
	void BuildSphereBox();

private:
	std::vector<AccelerationStructure> m_MeshStructures; // per mesh (empty meshes have none)
	AccelerationStructure m_SphereBox;                   // unit cube around a radius 1 sphere
	AccelerationStructure m_TopLevel;
	uint32_t m_TopLevelCapacity = 0;                     // instances m_TopLevel was sized for

	HostBuffer m_InstanceBuffer; // VkAccelerationStructureInstanceKHR[], rewritten every frame
	HostBuffer m_SphereBoxBuffer;
	VkBuffer m_ScratchBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_ScratchMemory = VK_NULL_HANDLE;
	VkDeviceSize m_ScratchSize = 0;
	VkDeviceSize m_ScratchAlignment = 256;
};
