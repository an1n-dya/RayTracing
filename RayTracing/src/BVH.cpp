#include "BVH.h"

#include "Mesh.h"

#include <algorithm>
#include <cfloat>

namespace BVH {

	namespace {

		constexpr int BinCount = 16;
		constexpr uint32_t MaxLeafTriangles = 8;

		struct Bounds {
			glm::vec3 Min{ FLT_MAX }, Max{ -FLT_MAX };

			void Grow(const glm::vec3& point) { Min = glm::min(Min, point); Max = glm::max(Max, point); }
			void Grow(const Bounds& other) { Min = glm::min(Min, other.Min); Max = glm::max(Max, other.Max); }
			float Area() const {
				glm::vec3 extent = Max - Min;
				return extent.x < 0.0f ? 0.0f : 2.0f * (extent.x * extent.y + extent.y * extent.z + extent.z * extent.x);
			}
		};

		class Builder {
		public:
			Builder(const Mesh& mesh, std::vector<BVHNode>& nodes)
				: m_Nodes(nodes)
			{
				uint32_t triangleCount = mesh.GetTriangleCount();
				m_Order.resize(triangleCount);
				m_Bounds.resize(triangleCount);
				m_Centroids.resize(triangleCount);
				for (uint32_t i = 0; i < triangleCount; i++) {
					m_Order[i] = i;
					for (int corner = 0; corner < 3; corner++)
						m_Bounds[i].Grow(mesh.Vertices[mesh.Indices[i * 3 + corner]].Position);
					m_Centroids[i] = 0.5f * (m_Bounds[i].Min + m_Bounds[i].Max);
				}
			}

			void Build() {
				m_Nodes.clear();
				m_Nodes.reserve(m_Order.size() * 2);
				BVHNode& root = m_Nodes.emplace_back();
				root.LeftOrFirst = 0;
				root.TriangleCount = (uint32_t)m_Order.size();
				UpdateBounds(0);
				Subdivide(0, 0);
			}

			const std::vector<uint32_t>& GetOrder() const { return m_Order; }

		private:
			void UpdateBounds(uint32_t nodeIndex) {
				BVHNode& node = m_Nodes[nodeIndex];
				Bounds bounds;
				for (uint32_t i = 0; i < node.TriangleCount; i++)
					bounds.Grow(m_Bounds[m_Order[node.LeftOrFirst + i]]);
				node.BoundsMin = bounds.Min;
				node.BoundsMax = bounds.Max;
			}

			// Best split plane by the surface area heuristic, evaluated at bin boundaries along each axis
			float FindBestSplit(const BVHNode& node, int& bestAxis, float& bestPosition) const {
				float bestCost = FLT_MAX;
				for (int axis = 0; axis < 3; axis++) {
					float centroidMin = FLT_MAX, centroidMax = -FLT_MAX;
					for (uint32_t i = 0; i < node.TriangleCount; i++) {
						float c = m_Centroids[m_Order[node.LeftOrFirst + i]][axis];
						centroidMin = std::min(centroidMin, c);
						centroidMax = std::max(centroidMax, c);
					}
					if (centroidMax <= centroidMin)
						continue;

					Bounds binBounds[BinCount];
					uint32_t binCounts[BinCount] = {};
					float scale = (float)BinCount / (centroidMax - centroidMin);
					for (uint32_t i = 0; i < node.TriangleCount; i++) {
						uint32_t triangle = m_Order[node.LeftOrFirst + i];
						int bin = std::min(BinCount - 1, (int)((m_Centroids[triangle][axis] - centroidMin) * scale));
						binCounts[bin]++;
						binBounds[bin].Grow(m_Bounds[triangle]);
					}

					// Sweep from both sides to get the area/count left and right of each boundary
					float leftArea[BinCount - 1], rightArea[BinCount - 1];
					uint32_t leftCount[BinCount - 1], rightCount[BinCount - 1];
					Bounds leftBounds, rightBounds;
					uint32_t leftSum = 0, rightSum = 0;
					for (int i = 0; i < BinCount - 1; i++) {
						leftSum += binCounts[i];
						leftCount[i] = leftSum;
						leftBounds.Grow(binBounds[i]);
						leftArea[i] = leftBounds.Area();

						rightSum += binCounts[BinCount - 1 - i];
						rightCount[BinCount - 2 - i] = rightSum;
						rightBounds.Grow(binBounds[BinCount - 1 - i]);
						rightArea[BinCount - 2 - i] = rightBounds.Area();
					}

					float binWidth = (centroidMax - centroidMin) / (float)BinCount;
					for (int i = 0; i < BinCount - 1; i++) {
						float cost = leftCount[i] * leftArea[i] + rightCount[i] * rightArea[i];
						if (leftCount[i] > 0 && rightCount[i] > 0 && cost < bestCost) {
							bestCost = cost;
							bestAxis = axis;
							bestPosition = centroidMin + binWidth * (float)(i + 1);
						}
					}
				}
				return bestCost;
			}

			void Subdivide(uint32_t nodeIndex, uint32_t depth) {
				// Note: m_Nodes may reallocate below, so re-fetch nodes by index after growing it
				BVHNode node = m_Nodes[nodeIndex];
				if (node.TriangleCount <= 2 || depth >= MaxDepth)
					return;

				int axis = -1;
				float position = 0.0f;
				float splitCost = FindBestSplit(node, axis, position);

				Bounds nodeBounds;
				nodeBounds.Min = node.BoundsMin;
				nodeBounds.Max = node.BoundsMax;
				float leafCost = (float)node.TriangleCount * nodeBounds.Area();
				if (axis < 0 || (splitCost >= leafCost && node.TriangleCount <= MaxLeafTriangles))
					return; // splitting doesn't pay off

				// Partition the node's triangles around the split position
				uint32_t i = node.LeftOrFirst;
				uint32_t j = node.LeftOrFirst + node.TriangleCount - 1;
				while (i <= j && j != UINT32_MAX) {
					if (m_Centroids[m_Order[i]][axis] < position)
						i++;
					else
						std::swap(m_Order[i], m_Order[j--]);
				}
				uint32_t leftCount = i - node.LeftOrFirst;
				if (leftCount == 0 || leftCount == node.TriangleCount)
					return;

				uint32_t leftIndex = (uint32_t)m_Nodes.size();
				m_Nodes.emplace_back();
				m_Nodes.emplace_back();
				m_Nodes[leftIndex].LeftOrFirst = node.LeftOrFirst;
				m_Nodes[leftIndex].TriangleCount = leftCount;
				m_Nodes[leftIndex + 1].LeftOrFirst = i;
				m_Nodes[leftIndex + 1].TriangleCount = node.TriangleCount - leftCount;
				m_Nodes[nodeIndex].LeftOrFirst = leftIndex;
				m_Nodes[nodeIndex].TriangleCount = 0;

				UpdateBounds(leftIndex);
				UpdateBounds(leftIndex + 1);
				Subdivide(leftIndex, depth + 1);
				Subdivide(leftIndex + 1, depth + 1);
			}

			std::vector<BVHNode>& m_Nodes;
			std::vector<uint32_t> m_Order; // triangle indices, rearranged so leaves are contiguous
			std::vector<Bounds> m_Bounds;
			std::vector<glm::vec3> m_Centroids;
		};

	}

	void Build(Mesh& mesh) {
		mesh.BVHNodes.clear();
		if (mesh.GetTriangleCount() == 0)
			return;

		Builder builder(mesh, mesh.BVHNodes);
		builder.Build();

		// Put the triangles in BVH order
		const std::vector<uint32_t>& order = builder.GetOrder();
		std::vector<uint32_t> indices(mesh.Indices.size());
		for (size_t i = 0; i < order.size(); i++)
			for (int corner = 0; corner < 3; corner++)
				indices[i * 3 + corner] = mesh.Indices[order[i] * 3 + corner];
		mesh.Indices = std::move(indices);
	}

}
