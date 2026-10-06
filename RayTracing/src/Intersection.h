#pragma once

#include <glm/glm.hpp>

// Ray-primitive intersection tests. GLSL counterpart: shaders/Intersection.glsl - keep the two in sync.
// Directions don't need to be normalized; returned distances are in units of the direction's length.
// A miss is reported as a negative distance.
namespace Intersect {

	// Near hit, or the far one if the ray starts inside the sphere (e.g. after refracting into glass)
	inline float Sphere(const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& center, float radius) {
		// Equation of a sphere:
		// (bx^2 + by^2)t^2 + (2(axbx + ayby))t + (ax^2 + ay^2 - r^2) = 0
		// where a = ray origin (relative to the center), b = ray direction, r = radius, t = hit distance
		glm::vec3 oc = origin - center;
		float a = glm::dot(direction, direction);
		float b = 2.0f * glm::dot(oc, direction);
		float c = glm::dot(oc, oc) - radius * radius;

		// Quadratic formula: t = (-b +- sqrt(b^2 - 4ac)) / 2a
		float discriminant = b * b - 4.0f * a * c;
		if (discriminant < 0.0f)
			return -1.0f;

		float sqrtDiscriminant = glm::sqrt(discriminant);
		float t = (-b - sqrtDiscriminant) / (2.0f * a);
		if (t <= 0.0f)
			t = (-b + sqrtDiscriminant) / (2.0f * a);
		return t;
	}

	// Infinite plane through point with the given (unit) normal; double sided
	inline float Plane(const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& point, const glm::vec3& normal) {
		float denominator = glm::dot(direction, normal);
		if (glm::abs(denominator) < 1e-8f)
			return -1.0f;
		return glm::dot(point - origin, normal) / denominator;
	}

	// Moller-Trumbore, double sided. Returns t and the barycentrics of v1 and v2 in uv.
	inline float Triangle(const glm::vec3& origin, const glm::vec3& direction,
		const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, glm::vec2& uv)
	{
		glm::vec3 edge1 = v1 - v0;
		glm::vec3 edge2 = v2 - v0;
		glm::vec3 p = glm::cross(direction, edge2);
		float determinant = glm::dot(edge1, p);
		if (glm::abs(determinant) < 1e-12f)
			return -1.0f;

		float inverseDeterminant = 1.0f / determinant;
		glm::vec3 s = origin - v0;
		uv.x = glm::dot(s, p) * inverseDeterminant;
		if (uv.x < 0.0f || uv.x > 1.0f)
			return -1.0f;

		glm::vec3 q = glm::cross(s, edge1);
		uv.y = glm::dot(direction, q) * inverseDeterminant;
		if (uv.y < 0.0f || uv.x + uv.y > 1.0f)
			return -1.0f;

		return glm::dot(edge2, q) * inverseDeterminant;
	}

	// Slab test: does the ray hit the box somewhere in [0, maxDistance]? Returns the entry distance (or -1).
	inline float AABB(const glm::vec3& origin, const glm::vec3& inverseDirection, const glm::vec3& boxMin, const glm::vec3& boxMax, float maxDistance) {
		glm::vec3 t0 = (boxMin - origin) * inverseDirection;
		glm::vec3 t1 = (boxMax - origin) * inverseDirection;
		glm::vec3 tNear = glm::min(t0, t1);
		glm::vec3 tFar = glm::max(t0, t1);
		float enter = glm::max(glm::max(tNear.x, tNear.y), glm::max(tNear.z, 0.0f));
		float exit = glm::min(glm::min(tFar.x, tFar.y), glm::min(tFar.z, maxDistance));
		return enter <= exit ? enter : -1.0f;
	}

}
