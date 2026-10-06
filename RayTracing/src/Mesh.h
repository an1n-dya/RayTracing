#pragma once

#include "BVH.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

struct Vertex {
	glm::vec3 Position{ 0.0f };
	glm::vec3 Normal{ 0.0f, 1.0f, 0.0f };
};

// Triangle mesh in object space. Placed in the scene (any number of times) by MeshInstances.
struct Mesh {
	std::string Name;
	std::string Source; // "builtin:<name>" or the file it was loaded from - what scene files store
	std::vector<Vertex> Vertices;
	std::vector<uint32_t> Indices; // three per triangle, counter-clockwise when seen from the front, in BVH order
	glm::vec3 BoundsMin{ 0.0f }, BoundsMax{ 0.0f };
	std::vector<BVHNode> BVHNodes; // built by MeshLoader::Load (BVH::Build); node 0 is the root

	uint32_t GetTriangleCount() const { return (uint32_t)(Indices.size() / 3); }
	void ComputeBounds();
};

// Procedural meshes, all centered on the origin and fitting in a unit cube
namespace BuiltinMeshes {

	constexpr const char* Box = "builtin:box";       // unit cube, flat faces
	constexpr const char* Quad = "builtin:quad";     // unit square in the XZ plane, facing +Y
	constexpr const char* Sphere = "builtin:sphere"; // UV sphere of radius 0.5, smooth normals

	bool IsBuiltin(const std::string& source);
	bool Create(const std::string& source, Mesh& mesh);

}
