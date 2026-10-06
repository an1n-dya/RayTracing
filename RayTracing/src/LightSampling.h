#pragma once

#include "BSDF.h"

#include <glm/glm.hpp>

// Sampling directions towards lights, and the matching solid-angle pdfs (needed for MIS when a BSDF-sampled
// ray happens to hit a light). GLSL counterpart: shaders/LightSampling.glsl - keep the two in sync.
namespace LightSampling {

	// Multiple importance sampling weight for a sample with pdf `pdf`, competing with one with pdf `otherPdf`
	inline float PowerHeuristic(float pdf, float otherPdf) {
		float a = pdf * pdf;
		float b = otherPdf * otherPdf;
		return a + b > 0.0f ? a / (a + b) : 0.0f;
	}

	// 1 - cos(theta_max) of the cone a sphere subtends from `point`; 0 if the point is inside the sphere.
	// Written as sin^2 / (1 + cos) so tiny or distant spheres don't cancel to zero.
	inline float SphereConeOneMinusCos(const glm::vec3& point, const glm::vec3& center, float radius) {
		glm::vec3 toCenter = center - point;
		float distance2 = glm::dot(toCenter, toCenter);
		float radius2 = radius * radius;
		if (distance2 <= radius2)
			return 0.0f;
		float sin2 = radius2 / distance2;
		return sin2 / (1.0f + glm::sqrt(glm::max(0.0f, 1.0f - sin2)));
	}

	// Uniform over the cone of directions that hit the sphere
	inline float SpherePdf(const glm::vec3& point, const glm::vec3& center, float radius) {
		float oneMinusCos = SphereConeOneMinusCos(point, center, radius);
		return oneMinusCos > 0.0f ? 1.0f / (2.0f * BSDF::Pi * oneMinusCos) : 0.0f;
	}

	inline bool SampleSphere(const glm::vec3& point, const glm::vec3& center, float radius, float u1, float u2,
		glm::vec3& direction, float& pdf)
	{
		float oneMinusCos = SphereConeOneMinusCos(point, center, radius);
		if (oneMinusCos <= 0.0f)
			return false; // inside the light

		float cosTheta = 1.0f - u1 * oneMinusCos;
		float sinTheta = glm::sqrt(glm::max(0.0f, 1.0f - cosTheta * cosTheta));
		float phi = 2.0f * BSDF::Pi * u2;

		glm::vec3 w = glm::normalize(center - point);
		glm::vec3 t, b;
		BSDF::BuildBasis(w, t, b);
		direction = glm::normalize(t * (glm::cos(phi) * sinTheta) + b * (glm::sin(phi) * sinTheta) + w * cosTheta);
		pdf = 1.0f / (2.0f * BSDF::Pi * oneMinusCos);
		return true;
	}

	// Solid-angle pdf of uniformly sampling a point on a triangle of `area`, seen at `distance` and angle cosLight
	inline float TrianglePdf(float distance, float cosLight, float area) {
		return cosLight > 1e-6f ? distance * distance / (area * cosLight) : 0.0f;
	}

	// Uniform point on the triangle; fails if the point sees the triangle's back (only the front face, the side
	// `normal` points to, emits)
	inline bool SampleTriangle(const glm::vec3& point, const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
		const glm::vec3& normal, float area, float u1, float u2, glm::vec3& direction, float& distance, float& pdf)
	{
		float su = glm::sqrt(u1);
		float b0 = 1.0f - su;
		float b1 = u2 * su;
		glm::vec3 target = b0 * v0 + b1 * v1 + (1.0f - b0 - b1) * v2;

		glm::vec3 toLight = target - point;
		distance = glm::length(toLight);
		if (distance <= 0.0f)
			return false;
		direction = toLight / distance;

		pdf = TrianglePdf(distance, -glm::dot(normal, direction), area);
		return pdf > 0.0f;
	}

}
