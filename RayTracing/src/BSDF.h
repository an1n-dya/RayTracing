#pragma once

#include "Scene.h"

#include <glm/glm.hpp>

// Material scattering model. GLSL counterpart: shaders/BSDF.glsl - keep the two in sync.
//
// Conventions: n is the shading normal, flipped to face wo. wo points back along the incoming ray (towards
// the viewer), wi towards where light arrives from (the next bounce). All are unit length.
//
// A simplified metallic/roughness model, as a blend of three parts:
//   - glass (weight (1 - metallic) * transmission): rough dielectric, GGX microfacets, exact Fresnel,
//     refracted light tinted by the albedo on the way in
//   - specular: GGX microfacet reflection with Smith shadowing, F0 = mix(F0 from IOR, albedo, metallic)
//   - diffuse:  Lambert, scaled by (1 - metallic) and by the light the specular layer didn't reflect (1 - F)
// Evaluate()/Pdf() only cover specular + diffuse; the glass part is treated like a delta lobe (it can only be
// sampled), which is what light sampling with MIS needs to know (Sample::Delta).
namespace BSDF {

	constexpr float Pi = 3.14159265358979f;
	constexpr float MinAlpha = 1e-3f; // keeps "perfect" mirrors (roughness 0) numerically well behaved

	struct Sample {
		glm::vec3 Direction{ 0.0f }; // wi
		glm::vec3 Weight{ 0.0f };    // f * |cos(wi)| / pdf: what to multiply the path throughput by
		float Pdf = 0.0f;            // solid-angle pdf of Direction (0 for delta lobes)
		bool Delta = false;          // sampled from the glass part, which Evaluate()/Pdf() don't cover
	};

	inline float Luminance(const glm::vec3& color) { return glm::dot(color, glm::vec3(0.2126f, 0.7152f, 0.0722f)); }

	inline float RoughnessToAlpha(float roughness) { return glm::max(roughness * roughness, MinAlpha); }

	inline glm::vec3 FresnelSchlick(const glm::vec3& f0, float cosTheta) {
		float m = glm::clamp(1.0f - cosTheta, 0.0f, 1.0f);
		float m5 = m * m * m * m * m;
		return f0 + (1.0f - f0) * m5;
	}

	// Unpolarized Fresnel reflectance of a dielectric interface. eta = IOR on the incident side / IOR on the other side.
	inline float FresnelDielectric(float cosI, float eta) {
		cosI = glm::clamp(cosI, 0.0f, 1.0f);
		float sin2T = eta * eta * (1.0f - cosI * cosI);
		if (sin2T >= 1.0f)
			return 1.0f; // total internal reflection
		float cosT = glm::sqrt(1.0f - sin2T);
		float rs = (eta * cosI - cosT) / (eta * cosI + cosT);
		float rp = (cosI - eta * cosT) / (cosI + eta * cosT);
		return 0.5f * (rs * rs + rp * rp);
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

	inline float GlassWeight(const Material& material) {
		return (1.0f - material.Metallic) * material.Transmission;
	}

	inline glm::vec3 SpecularF0(const Material& material) {
		float f0 = (material.IOR - 1.0f) / (material.IOR + 1.0f);
		return glm::mix(glm::vec3(f0 * f0), material.Albedo, material.Metallic);
	}

	// Probability of sampling the specular lobe rather than the diffuse one, based on their rough energy
	inline float SpecularProbability(const Material& material, float NoV) {
		float specular = Luminance(FresnelSchlick(SpecularF0(material), NoV));
		float diffuse = (1.0f - material.Metallic) * Luminance(material.Albedo);
		if (diffuse <= 0.0f)
			return 1.0f;
		return glm::clamp(specular / (specular + diffuse), 0.05f, 0.95f);
	}

	// f(wo, wi) * cos(wi), excluding the glass part
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
		return (1.0f - GlassWeight(material)) * (specular + diffuse) * NoL;
	}

	// Solid-angle pdf of Sample() producing wi through the specular/diffuse lobes
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
		return (1.0f - GlassWeight(material)) * (pSpecular * specularPdf + (1.0f - pSpecular) * diffusePdf);
	}

	// frontFace: the ray hit the surface from outside (its geometric normal faced the ray).
	// u: three uniform random numbers in [0,1). Returns false if the path should end.
	inline bool SampleDirection(const Material& material, const glm::vec3& n, const glm::vec3& wo, bool frontFace,
		const glm::vec3& u, Sample& sample)
	{
		float NoV = glm::dot(n, wo);
		if (NoV <= 0.0f)
			return false;

		glm::vec3 t, b;
		BuildBasis(n, t, b);
		glm::vec3 woLocal(glm::dot(wo, t), glm::dot(wo, b), NoV);
		float alpha = RoughnessToAlpha(material.Roughness);

		// u.x picks the part; rescaled, it is reused as a fresh uniform number for the choice within the part
		float pGlass = GlassWeight(material);
		if (u.x < pGlass) {
			float choice = u.x / pGlass;

			glm::vec3 mLocal = SampleGGXVNDF(woLocal, alpha, u.y, u.z);
			glm::vec3 m = mLocal.x * t + mLocal.y * b + mLocal.z * n;

			float eta = frontFace ? 1.0f / material.IOR : material.IOR;
			float F = FresnelDielectric(glm::dot(wo, m), eta);

			glm::vec3 wi;
			glm::vec3 weight(1.0f);
			if (choice < F) {
				wi = glm::reflect(-wo, m);
				if (glm::dot(n, wi) <= 0.0f)
					return false;
			}
			else {
				wi = glm::refract(-wo, m, eta);
				if (glm::dot(n, wi) >= 0.0f) // also catches the zero vector refract() returns on total internal reflection
					return false;
				if (frontFace)
					weight = material.Albedo; // tint light entering the material
			}

			// Picking reflection/refraction by Fresnel cancels F; VNDF sampling leaves G2 / G1(wo) = G1(wi)
			sample.Direction = wi;
			sample.Weight = weight * SmithG1(glm::abs(glm::dot(n, wi)), alpha);
			sample.Pdf = 0.0f;
			sample.Delta = true;
			return true;
		}

		float choice = (u.x - pGlass) / (1.0f - pGlass);
		glm::vec3 wi;
		if (choice < SpecularProbability(material, NoV)) {
			glm::vec3 hLocal = SampleGGXVNDF(woLocal, alpha, u.y, u.z);
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
		sample.Delta = false;
		return true;
	}

}
