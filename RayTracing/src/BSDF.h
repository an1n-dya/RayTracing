#pragma once

#include "Scene.h"

#include <glm/glm.hpp>

// Material scattering model. GLSL counterpart: shaders/BSDF.glsl - keep the two in sync.
//
// Conventions: n is the shading normal, flipped to face wo. wo points back along the incoming ray (towards
// the viewer), wi towards where light arrives from (the next bounce). All are unit length.
//
// A simplified metallic/roughness model with two lobes:
//   - specular: GGX microfacet reflection with Smith shadowing, F0 = mix(0.04, albedo, metallic)
//   - diffuse:  Lambert, scaled by (1 - metallic) and by the light the specular layer didn't reflect (1 - F)
namespace BSDF {

	constexpr float Pi = 3.14159265358979f;
	constexpr float MinAlpha = 1e-3f; // keeps "perfect" mirrors (roughness 0) numerically well behaved

	struct Sample {
		glm::vec3 Direction{ 0.0f }; // wi
		glm::vec3 Weight{ 0.0f };    // f * |cos(wi)| / pdf: what to multiply the path throughput by
		float Pdf = 0.0f;            // solid-angle pdf of Direction
	};

	inline float Luminance(const glm::vec3& color) { return glm::dot(color, glm::vec3(0.2126f, 0.7152f, 0.0722f)); }

	inline float RoughnessToAlpha(float roughness) { return glm::max(roughness * roughness, MinAlpha); }

	inline glm::vec3 FresnelSchlick(const glm::vec3& f0, float cosTheta) {
		float m = glm::clamp(1.0f - cosTheta, 0.0f, 1.0f);
		float m5 = m * m * m * m * m;
		return f0 + (1.0f - f0) * m5;
	}

	// GGX / Trowbridge-Reitz normal distribution
	inline float DistributionGGX(float NoH, float alpha) {
		float a2 = alpha * alpha;
		float d = NoH * NoH * (a2 - 1.0f) + 1.0f;
		return a2 / (Pi * d * d);
	}

	// Smith masking for GGX, one direction
	inline float SmithG1(float NoX, float alpha) {
		float a2 = alpha * alpha;
		return 2.0f * NoX / (NoX + glm::sqrt(a2 + (1.0f - a2) * NoX * NoX));
	}

	// Orthonormal basis around n (Duff et al. 2017)
	inline void BuildBasis(const glm::vec3& n, glm::vec3& t, glm::vec3& b) {
		float sign = n.z >= 0.0f ? 1.0f : -1.0f;
		float a = -1.0f / (sign + n.z);
		float c = n.x * n.y * a;
		t = glm::vec3(1.0f + sign * n.x * n.x * a, sign * c, -sign * n.x);
		b = glm::vec3(c, sign + n.y * n.y * a, -n.y);
	}

	inline glm::vec3 SampleCosineHemisphere(float u1, float u2) {
		float r = glm::sqrt(u1);
		float phi = 2.0f * Pi * u2;
		return glm::vec3(r * glm::cos(phi), r * glm::sin(phi), glm::sqrt(glm::max(0.0f, 1.0f - u1)));
	}

	// Samples a GGX microfacet normal from the distribution of normals visible from v (Heitz 2018).
	// Everything is in the local frame where the surface normal is +Z.
	inline glm::vec3 SampleGGXVNDF(const glm::vec3& v, float alpha, float u1, float u2) {
		glm::vec3 vh = glm::normalize(glm::vec3(alpha * v.x, alpha * v.y, v.z));
		float lensq = vh.x * vh.x + vh.y * vh.y;
		glm::vec3 t1 = lensq > 0.0f ? glm::vec3(-vh.y, vh.x, 0.0f) / glm::sqrt(lensq) : glm::vec3(1.0f, 0.0f, 0.0f);
		glm::vec3 t2 = glm::cross(vh, t1);

		float r = glm::sqrt(u1);
		float phi = 2.0f * Pi * u2;
		float p1 = r * glm::cos(phi);
		float p2 = r * glm::sin(phi);
		float s = 0.5f * (1.0f + vh.z);
		p2 = (1.0f - s) * glm::sqrt(glm::max(0.0f, 1.0f - p1 * p1)) + s * p2;

		glm::vec3 nh = p1 * t1 + p2 * t2 + glm::sqrt(glm::max(0.0f, 1.0f - p1 * p1 - p2 * p2)) * vh;
		return glm::normalize(glm::vec3(alpha * nh.x, alpha * nh.y, glm::max(0.0f, nh.z)));
	}

	inline glm::vec3 SpecularF0(const Material& material) {
		return glm::mix(glm::vec3(0.04f), material.Albedo, material.Metallic);
	}

	// Probability of sampling the specular lobe rather than the diffuse one, based on their rough energy
	inline float SpecularProbability(const Material& material, float NoV) {
		float specular = Luminance(FresnelSchlick(SpecularF0(material), NoV));
		float diffuse = (1.0f - material.Metallic) * Luminance(material.Albedo);
		if (diffuse <= 0.0f)
			return 1.0f;
		return glm::clamp(specular / (specular + diffuse), 0.05f, 0.95f);
	}

	// f(wo, wi) * cos(wi)
	inline glm::vec3 Evaluate(const Material& material, const glm::vec3& n, const glm::vec3& wo, const glm::vec3& wi) {
		float NoV = glm::dot(n, wo);
		float NoL = glm::dot(n, wi);
		if (NoV <= 0.0f || NoL <= 0.0f)
			return glm::vec3(0.0f);

		glm::vec3 h = glm::normalize(wo + wi);
		float NoH = glm::max(glm::dot(n, h), 0.0f);
		float VoH = glm::max(glm::dot(wo, h), 0.0f);

		float alpha = RoughnessToAlpha(material.Roughness);
		glm::vec3 F = FresnelSchlick(SpecularF0(material), VoH);
		float D = DistributionGGX(NoH, alpha);
		float G = SmithG1(NoV, alpha) * SmithG1(NoL, alpha);

		glm::vec3 specular = F * (D * G / (4.0f * NoV * NoL));
		glm::vec3 diffuse = (1.0f - F) * (1.0f - material.Metallic) * material.Albedo / Pi;
		return (specular + diffuse) * NoL;
	}

	// Solid-angle pdf of Sample() producing wi
	inline float Pdf(const Material& material, const glm::vec3& n, const glm::vec3& wo, const glm::vec3& wi) {
		float NoV = glm::dot(n, wo);
		float NoL = glm::dot(n, wi);
		if (NoV <= 0.0f || NoL <= 0.0f)
			return 0.0f;

		glm::vec3 h = glm::normalize(wo + wi);
		float NoH = glm::max(glm::dot(n, h), 0.0f);
		float alpha = RoughnessToAlpha(material.Roughness);

		// VNDF reflection pdf: D_v(h) / (4 VoH) = G1(V) D(h) / (4 NoV)
		float specularPdf = SmithG1(NoV, alpha) * DistributionGGX(NoH, alpha) / (4.0f * NoV);
		float diffusePdf = NoL / Pi;

		float pSpecular = SpecularProbability(material, NoV);
		return pSpecular * specularPdf + (1.0f - pSpecular) * diffusePdf;
	}

	// u: three uniform random numbers in [0,1). Returns false if the path should end (sample below the surface).
	inline bool SampleDirection(const Material& material, const glm::vec3& n, const glm::vec3& wo, const glm::vec3& u, Sample& sample) {
		float NoV = glm::dot(n, wo);
		if (NoV <= 0.0f)
			return false;

		glm::vec3 t, b;
		BuildBasis(n, t, b);

		glm::vec3 wi;
		if (u.x < SpecularProbability(material, NoV)) {
			glm::vec3 woLocal(glm::dot(wo, t), glm::dot(wo, b), NoV);
			glm::vec3 hLocal = SampleGGXVNDF(woLocal, RoughnessToAlpha(material.Roughness), u.y, u.z);
			glm::vec3 h = hLocal.x * t + hLocal.y * b + hLocal.z * n;
			wi = glm::reflect(-wo, h);
		}
		else {
			glm::vec3 local = SampleCosineHemisphere(u.y, u.z);
			wi = local.x * t + local.y * b + local.z * n;
		}

		if (glm::dot(n, wi) <= 0.0f)
			return false;

		// One-sample MIS over the two lobes: weight by the combined pdf rather than the chosen lobe's
		sample.Direction = wi;
		sample.Pdf = Pdf(material, n, wo, wi);
		if (sample.Pdf <= 0.0f)
			return false;
		sample.Weight = Evaluate(material, n, wo, wi) / sample.Pdf;
		return true;
	}

}
