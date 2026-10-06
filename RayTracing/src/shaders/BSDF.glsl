// Material scattering model. C++ counterpart: BSDF.h - keep the two in sync (see the conventions there).
//
// Lobes: GGX specular (F0 = mix(0.04, albedo, metallic)) + Lambert diffuse scaled by (1 - metallic)(1 - F).

#ifndef BSDF_GLSL
#define BSDF_GLSL

const float BSDF_PI = 3.14159265358979;
const float BSDF_MIN_ALPHA = 1e-3;

struct SurfaceMaterial {
	vec3 Albedo;
	float Roughness;
	float Metallic;
};

struct BSDFSample {
	vec3 Direction; // wi
	vec3 Weight;    // f * |cos(wi)| / pdf
	float Pdf;
};

float Luminance(vec3 color) { return dot(color, vec3(0.2126, 0.7152, 0.0722)); }

float RoughnessToAlpha(float roughness) { return max(roughness * roughness, BSDF_MIN_ALPHA); }

vec3 FresnelSchlick(vec3 f0, float cosTheta)
{
	float m = clamp(1.0 - cosTheta, 0.0, 1.0);
	float m5 = m * m * m * m * m;
	return f0 + (1.0 - f0) * m5;
}

float DistributionGGX(float NoH, float alpha)
{
	float a2 = alpha * alpha;
	float d = NoH * NoH * (a2 - 1.0) + 1.0;
	return a2 / (BSDF_PI * d * d);
}

float SmithG1(float NoX, float alpha)
{
	float a2 = alpha * alpha;
	return 2.0 * NoX / (NoX + sqrt(a2 + (1.0 - a2) * NoX * NoX));
}

void BuildBasis(vec3 n, out vec3 t, out vec3 b)
{
	float s = n.z >= 0.0 ? 1.0 : -1.0;
	float a = -1.0 / (s + n.z);
	float c = n.x * n.y * a;
	t = vec3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
	b = vec3(c, s + n.y * n.y * a, -n.y);
}

vec3 SampleCosineHemisphere(float u1, float u2)
{
	float r = sqrt(u1);
	float phi = 2.0 * BSDF_PI * u2;
	return vec3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - u1)));
}

vec3 SampleGGXVNDF(vec3 v, float alpha, float u1, float u2)
{
	vec3 vh = normalize(vec3(alpha * v.x, alpha * v.y, v.z));
	float lensq = vh.x * vh.x + vh.y * vh.y;
	vec3 t1 = lensq > 0.0 ? vec3(-vh.y, vh.x, 0.0) / sqrt(lensq) : vec3(1.0, 0.0, 0.0);
	vec3 t2 = cross(vh, t1);

	float r = sqrt(u1);
	float phi = 2.0 * BSDF_PI * u2;
	float p1 = r * cos(phi);
	float p2 = r * sin(phi);
	float s = 0.5 * (1.0 + vh.z);
	p2 = (1.0 - s) * sqrt(max(0.0, 1.0 - p1 * p1)) + s * p2;

	vec3 nh = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;
	return normalize(vec3(alpha * nh.x, alpha * nh.y, max(0.0, nh.z)));
}

vec3 SpecularF0(SurfaceMaterial material)
{
	return mix(vec3(0.04), material.Albedo, material.Metallic);
}

float SpecularProbability(SurfaceMaterial material, float NoV)
{
	float specular = Luminance(FresnelSchlick(SpecularF0(material), NoV));
	float diffuse = (1.0 - material.Metallic) * Luminance(material.Albedo);
	if (diffuse <= 0.0)
		return 1.0;
	return clamp(specular / (specular + diffuse), 0.05, 0.95);
}

// f(wo, wi) * cos(wi)
vec3 EvaluateBSDF(SurfaceMaterial material, vec3 n, vec3 wo, vec3 wi)
{
	float NoV = dot(n, wo);
	float NoL = dot(n, wi);
	if (NoV <= 0.0 || NoL <= 0.0)
		return vec3(0.0);

	vec3 h = normalize(wo + wi);
	float NoH = max(dot(n, h), 0.0);
	float VoH = max(dot(wo, h), 0.0);

	float alpha = RoughnessToAlpha(material.Roughness);
	vec3 F = FresnelSchlick(SpecularF0(material), VoH);
	float D = DistributionGGX(NoH, alpha);
	float G = SmithG1(NoV, alpha) * SmithG1(NoL, alpha);

	vec3 specular = F * (D * G / (4.0 * NoV * NoL));
	vec3 diffuse = (1.0 - F) * (1.0 - material.Metallic) * material.Albedo / BSDF_PI;
	return (specular + diffuse) * NoL;
}

float PdfBSDF(SurfaceMaterial material, vec3 n, vec3 wo, vec3 wi)
{
	float NoV = dot(n, wo);
	float NoL = dot(n, wi);
	if (NoV <= 0.0 || NoL <= 0.0)
		return 0.0;

	vec3 h = normalize(wo + wi);
	float NoH = max(dot(n, h), 0.0);
	float alpha = RoughnessToAlpha(material.Roughness);

	float specularPdf = SmithG1(NoV, alpha) * DistributionGGX(NoH, alpha) / (4.0 * NoV);
	float diffusePdf = NoL / BSDF_PI;

	float pSpecular = SpecularProbability(material, NoV);
	return pSpecular * specularPdf + (1.0 - pSpecular) * diffusePdf;
}

bool SampleBSDF(SurfaceMaterial material, vec3 n, vec3 wo, vec3 u, out BSDFSample bsdfSample)
{
	bsdfSample.Direction = vec3(0.0);
	bsdfSample.Weight = vec3(0.0);
	bsdfSample.Pdf = 0.0;

	float NoV = dot(n, wo);
	if (NoV <= 0.0)
		return false;

	vec3 t, b;
	BuildBasis(n, t, b);

	vec3 wi;
	if (u.x < SpecularProbability(material, NoV))
	{
		vec3 woLocal = vec3(dot(wo, t), dot(wo, b), NoV);
		vec3 hLocal = SampleGGXVNDF(woLocal, RoughnessToAlpha(material.Roughness), u.y, u.z);
		vec3 h = hLocal.x * t + hLocal.y * b + hLocal.z * n;
		wi = reflect(-wo, h);
	}
	else
	{
		vec3 local = SampleCosineHemisphere(u.y, u.z);
		wi = local.x * t + local.y * b + local.z * n;
	}

	if (dot(n, wi) <= 0.0)
		return false;

	bsdfSample.Direction = wi;
	bsdfSample.Pdf = PdfBSDF(material, n, wo, wi);
	if (bsdfSample.Pdf <= 0.0)
		return false;
	bsdfSample.Weight = EvaluateBSDF(material, n, wo, wi) / bsdfSample.Pdf;
	return true;
}

#endif
