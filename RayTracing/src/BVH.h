#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

struct Mesh;

// Bounding volume hierarchy node, 32 bytes - the same layout the compute shader reads (shaders/PathTrace.comp)
struct BVHNode {
	glm::vec3 BoundsMin;
	uint32_t LeftOrFirst;   // interior node: index of the left child (the right one follows it); leaf: first triangle
	glm::vec3 BoundsMax;
	uint32_t TriangleCount; // 0 for interior nodes

	bool IsLeaf() const { return TriangleCount > 0; }
};

namespace BVH {

	// Traversal keeps a fixed-size stack (64 entries on the GPU), so trees are never built deeper than this
	constexpr uint32_t MaxDepth = 48;

	// Builds mesh.BVHNodes over the mesh's triangles with a binned surface area heuristic, reordering mesh.Indices
	// so that every leaf covers a contiguous range of triangles. Node 0 is the root.
	void Build(Mesh& mesh);

}
