#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <limits>
#include <numeric>
#include "Ray.h"
#include "Scene.h"

struct AABB {
    glm::vec3 Min{ std::numeric_limits<float>::max() };
    glm::vec3 Max{ -std::numeric_limits<float>::max() };

    AABB() = default;
    AABB(const glm::vec3& min, const glm::vec3& max) : Min(min), Max(max) {}

    void Expand(const glm::vec3& point) {
        Min = glm::min(Min, point);
        Max = glm::max(Max, point);
    }

    void Expand(const AABB& other) {
        Min = glm::min(Min, other.Min);
        Max = glm::max(Max, other.Max);
    }

    glm::vec3 Center() const {
        return (Min + Max) * 0.5f;
    }

    glm::vec3 Size() const {
        return Max - Min;
    }

    float SurfaceArea() const {
        glm::vec3 size = Size();
        return 2.0f * (size.x * size.y + size.y * size.z + size.z * size.x);
    }

    // Fast ray-AABB intersection test
    bool Intersect(const Ray& ray, float tMin = 0.0f, float tMax = std::numeric_limits<float>::max()) const {
        for (int i = 0; i < 3; i++) {
            float invD = 1.0f / ray.Direction[i];
            float t0 = (Min[i] - ray.Origin[i]) * invD;
            float t1 = (Max[i] - ray.Origin[i]) * invD;

            if (invD < 0.0f) {
                std::swap(t0, t1);
            }

            tMin = std::max(t0, tMin);
            tMax = std::min(t1, tMax);

            if (tMin > tMax) {
                return false;
            }
        }
        return true;
    }
};

struct BVHNode {
    AABB BoundingBox;
    std::unique_ptr<BVHNode> Left;
    std::unique_ptr<BVHNode> Right;

    // For leaf nodes
    std::vector<uint32_t> SphereIndices;

    bool IsLeaf() const {
        return Left == nullptr && Right == nullptr;
    }

    BVHNode() = default;
};

class BVH {
public:
    BVH() = default;
    ~BVH() = default;

    void Build(const Scene& scene);
    void Clear();

    // Ray intersection with BVH
    struct HitInfo {
        bool Hit = false;
        float Distance = std::numeric_limits<float>::max();
        uint32_t SphereIndex = 0;
        glm::vec3 HitPoint;
        glm::vec3 Normal;
    };

    HitInfo Intersect(const Ray& ray, const Scene& scene) const;

    // Statistics
    uint32_t GetNodeCount() const { return m_NodeCount; }
    uint32_t GetMaxDepth() const { return m_MaxDepth; }
    uint32_t GetLeafCount() const { return m_LeafCount; }

private:
    std::unique_ptr<BVHNode> m_Root;
    uint32_t m_NodeCount = 0;
    uint32_t m_MaxDepth = 0;
    uint32_t m_LeafCount = 0;

    // Build helpers
    std::unique_ptr<BVHNode> BuildRecursive(
        std::vector<uint32_t>& sphereIndices,
        const Scene& scene,
        uint32_t depth = 0
    );

    AABB CalculateBoundingBox(const std::vector<uint32_t>& sphereIndices, const Scene& scene) const;
    AABB GetSphereBounds(const Sphere& sphere) const;

    // Split methods
    enum class SplitMethod {
        Middle,     // Split at middle of longest axis
        EqualCounts, // Split to have equal number of objects
        SAH         // Surface Area Heuristic (best quality)
    };

    struct SplitResult {
        std::vector<uint32_t> LeftIndices;
        std::vector<uint32_t> RightIndices;
        float Cost = std::numeric_limits<float>::max();
    };

    SplitResult SplitSpheres(
        const std::vector<uint32_t>& sphereIndices,
        const Scene& scene,
        SplitMethod method = SplitMethod::SAH
    ) const;

    SplitResult SplitMiddle(const std::vector<uint32_t>& sphereIndices, const Scene& scene) const;
    SplitResult SplitEqualCounts(const std::vector<uint32_t>& sphereIndices, const Scene& scene) const;
    SplitResult SplitSAH(const std::vector<uint32_t>& sphereIndices, const Scene& scene) const;

    // Intersection helpers
    HitInfo IntersectNode(const BVHNode* node, const Ray& ray, const Scene& scene) const;
    HitInfo IntersectSphere(const Ray& ray, const Sphere& sphere, uint32_t sphereIndex) const;

    // Constants
    static constexpr uint32_t MAX_SPHERES_PER_LEAF = 4;
    static constexpr uint32_t SAH_BUCKETS = 12;
    static constexpr float TRAVERSAL_COST = 1.0f;
    static constexpr float INTERSECTION_COST = 1.0f;
};
