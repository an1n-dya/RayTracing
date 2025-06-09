#include "BVH.h"
#include <algorithm>
#include <iostream>

void BVH::Build(const Scene& scene) {
    Clear();

    if (scene.Spheres.empty()) {
        return;
    }

    // Initialize sphere indices
    std::vector<uint32_t> sphereIndices(scene.Spheres.size());
    std::iota(sphereIndices.begin(), sphereIndices.end(), 0);

    // Reset statistics
    m_NodeCount = 0;
    m_MaxDepth = 0;
    m_LeafCount = 0;

    // Build the tree
    m_Root = BuildRecursive(sphereIndices, scene, 0);

    std::cout << "BVH built: " << m_NodeCount << " nodes, "
        << m_LeafCount << " leaves, max depth: " << m_MaxDepth << std::endl;
}

void BVH::Clear() {
    m_Root.reset();
    m_NodeCount = 0;
    m_MaxDepth = 0;
    m_LeafCount = 0;
}

std::unique_ptr<BVHNode> BVH::BuildRecursive(
    std::vector<uint32_t>& sphereIndices,
    const Scene& scene,
    uint32_t depth) {

    auto node = std::make_unique<BVHNode>();
    m_NodeCount++;
    m_MaxDepth = std::max(m_MaxDepth, depth);

    // Calculate bounding box for this node
    node->BoundingBox = CalculateBoundingBox(sphereIndices, scene);

    // Create leaf if we have few enough spheres
    if (sphereIndices.size() <= MAX_SPHERES_PER_LEAF) {
        node->SphereIndices = std::move(sphereIndices);
        m_LeafCount++;
        return node;
    }

    // Split the spheres
    SplitResult split = SplitSpheres(sphereIndices, scene, SplitMethod::SAH);

    // If split failed or didn't improve much, create a leaf
    if (split.LeftIndices.empty() || split.RightIndices.empty()) {
        node->SphereIndices = std::move(sphereIndices);
        m_LeafCount++;
        return node;
    }

    // Recursively build children
    node->Left = BuildRecursive(split.LeftIndices, scene, depth + 1);
    node->Right = BuildRecursive(split.RightIndices, scene, depth + 1);

    return node;
}

AABB BVH::CalculateBoundingBox(const std::vector<uint32_t>& sphereIndices, const Scene& scene) const {
    AABB bounds;

    for (uint32_t index : sphereIndices) {
        bounds.Expand(GetSphereBounds(scene.Spheres[index]));
    }

    return bounds;
}

AABB BVH::GetSphereBounds(const Sphere& sphere) const {
    glm::vec3 radius(sphere.Radius);
    return AABB(sphere.Position - radius, sphere.Position + radius);
}

BVH::SplitResult BVH::SplitSpheres(
    const std::vector<uint32_t>& sphereIndices,
    const Scene& scene,
    SplitMethod method) const {

    switch (method) {
    case SplitMethod::Middle:
        return SplitMiddle(sphereIndices, scene);
    case SplitMethod::EqualCounts:
        return SplitEqualCounts(sphereIndices, scene);
    case SplitMethod::SAH:
    default:
        return SplitSAH(sphereIndices, scene);
    }
}

BVH::SplitResult BVH::SplitMiddle(const std::vector<uint32_t>& sphereIndices, const Scene& scene) const {
    AABB bounds = CalculateBoundingBox(sphereIndices, scene);
    glm::vec3 size = bounds.Size();

    // Find the longest axis
    int axis = 0;
    if (size.y > size.x) axis = 1;
    if (size.z > size[axis]) axis = 2;

    float splitPos = bounds.Center()[axis];

    SplitResult result;
    for (uint32_t index : sphereIndices) {
        if (scene.Spheres[index].Position[axis] < splitPos) {
            result.LeftIndices.push_back(index);
        }
        else {
            result.RightIndices.push_back(index);
        }
    }

    return result;
}

BVH::SplitResult BVH::SplitEqualCounts(const std::vector<uint32_t>& sphereIndices, const Scene& scene) const {
    AABB bounds = CalculateBoundingBox(sphereIndices, scene);
    glm::vec3 size = bounds.Size();

    // Find the longest axis
    int axis = 0;
    if (size.y > size.x) axis = 1;
    if (size.z > size[axis]) axis = 2;

    // Sort by center position along the chosen axis
    std::vector<uint32_t> sortedIndices = sphereIndices;
    std::sort(sortedIndices.begin(), sortedIndices.end(),
        [&](uint32_t a, uint32_t b) {
            return scene.Spheres[a].Position[axis] < scene.Spheres[b].Position[axis];
        });

    SplitResult result;
    size_t mid = sortedIndices.size() / 2;

    result.LeftIndices.assign(sortedIndices.begin(), sortedIndices.begin() + mid);
    result.RightIndices.assign(sortedIndices.begin() + mid, sortedIndices.end());

    return result;
}

BVH::SplitResult BVH::SplitSAH(const std::vector<uint32_t>& sphereIndices, const Scene& scene) const {
    AABB bounds = CalculateBoundingBox(sphereIndices, scene);

    SplitResult bestSplit;
    bestSplit.Cost = std::numeric_limits<float>::max();

    // Try each axis
    for (int axis = 0; axis < 3; axis++) {
        // Sort spheres by center position along this axis
        std::vector<uint32_t> sortedIndices = sphereIndices;
        std::sort(sortedIndices.begin(), sortedIndices.end(),
            [&](uint32_t a, uint32_t b) {
                return scene.Spheres[a].Position[axis] < scene.Spheres[b].Position[axis];
            });

        // Try different split positions using buckets
        for (int bucket = 1; bucket < SAH_BUCKETS; bucket++) {
            size_t splitIndex = (sortedIndices.size() * bucket) / SAH_BUCKETS;

            if (splitIndex == 0 || splitIndex >= sortedIndices.size()) {
                continue;
            }

            // Calculate bounds for left and right sides
            std::vector<uint32_t> leftIndices(sortedIndices.begin(), sortedIndices.begin() + splitIndex);
            std::vector<uint32_t> rightIndices(sortedIndices.begin() + splitIndex, sortedIndices.end());

            AABB leftBounds = CalculateBoundingBox(leftIndices, scene);
            AABB rightBounds = CalculateBoundingBox(rightIndices, scene);

            // Calculate SAH cost
            float leftArea = leftBounds.SurfaceArea();
            float rightArea = rightBounds.SurfaceArea();
            float totalArea = bounds.SurfaceArea();

            if (totalArea <= 0.0f) continue; // Avoid division by zero

            float cost = TRAVERSAL_COST +
                (leftArea / totalArea) * leftIndices.size() * INTERSECTION_COST +
                (rightArea / totalArea) * rightIndices.size() * INTERSECTION_COST;

            if (cost < bestSplit.Cost) {
                bestSplit.Cost = cost;
                bestSplit.LeftIndices = std::move(leftIndices);
                bestSplit.RightIndices = std::move(rightIndices);
            }
        }
    }

    // Check if splitting is beneficial (compare with leaf cost)
    float leafCost = sphereIndices.size() * INTERSECTION_COST;
    if (bestSplit.Cost >= leafCost) {
        // Don't split, return empty result
        bestSplit.LeftIndices.clear();
        bestSplit.RightIndices.clear();
    }

    return bestSplit;
}

BVH::HitInfo BVH::Intersect(const Ray& ray, const Scene& scene) const {
    if (!m_Root) {
        return HitInfo{};
    }

    return IntersectNode(m_Root.get(), ray, scene);
}

BVH::HitInfo BVH::IntersectNode(const BVHNode* node, const Ray& ray, const Scene& scene) const {
    if (!node->BoundingBox.Intersect(ray))
        return HitInfo{};

    if (node->IsLeaf()) {
        HitInfo closestHit;
        for (uint32_t sphereIndex : node->SphereIndices) {
            HitInfo hit = IntersectSphere(ray, scene.Spheres[sphereIndex], sphereIndex);
            if (hit.Hit && hit.Distance < closestHit.Distance) {
                closestHit = hit;
            }
        }
        return closestHit;
    }

    // Sort children based on distance to AABB
    HitInfo hit;
    const BVHNode* first = node->Left.get();
    const BVHNode* second = node->Right.get();

    float firstDist = first->BoundingBox.Intersect(ray) ? 0.0f : std::numeric_limits<float>::max();
    float secondDist = second->BoundingBox.Intersect(ray) ? 0.0f : std::numeric_limits<float>::max();

    if (secondDist < firstDist)
        std::swap(first, second);

    hit = IntersectNode(first, ray, scene);
    if (!hit.Hit || hit.Distance > secondDist) {
        HitInfo secondHit = IntersectNode(second, ray, scene);
        if (secondHit.Hit && secondHit.Distance < hit.Distance)
            hit = secondHit;
    }

    return hit;
}

BVH::HitInfo BVH::IntersectSphere(const Ray& ray, const Sphere& sphere, uint32_t sphereIndex) const {
    glm::vec3 origin = ray.Origin - sphere.Position;

    float a = glm::dot(ray.Direction, ray.Direction);
    float b = 2.0f * glm::dot(origin, ray.Direction);
    float c = glm::dot(origin, origin) - sphere.Radius * sphere.Radius;

    float discriminant = b * b - 4.0f * a * c;
    if (discriminant < 0.0f) {
        return HitInfo{};
    }

    float t = (-b - glm::sqrt(discriminant)) / (2.0f * a);
    if (t <= 0.0f) {
        return HitInfo{};
    }

    HitInfo hit;
    hit.Hit = true;
    hit.Distance = t;
    hit.SphereIndex = sphereIndex;
    hit.HitPoint = ray.Origin + ray.Direction * t;
    hit.Normal = glm::normalize((hit.HitPoint - sphere.Position) / sphere.Radius);

    return hit;
}
