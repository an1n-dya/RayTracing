// Sampling directions towards lights and the matching solid-angle pdfs.
// C++ counterpart: LightSampling.h - keep the two in sync. Needs BSDF.glsl (BuildBasis, BSDF_PI).

#ifndef LIGHT_SAMPLING_GLSL
#define LIGHT_SAMPLING_GLSL

float PowerHeuristic(float pdf, float otherPdf)
{
	float a = pdf * pdf;
	float b = otherPdf * otherPdf;
	return a + b > 0.0 ? a / (a + b) : 0.0;
}

float SphereConeOneMinusCos(vec3 point, vec3 center, float radius)
{
	vec3 toCenter = center - point;
	float distance2 = dot(toCenter, toCenter);
	float radius2 = radius * radius;
	if (distance2 <= radius2)
		return 0.0;
	float sin2 = radius2 / distance2;
	return sin2 / (1.0 + sqrt(max(0.0, 1.0 - sin2)));
}

float SphereLightPdf(vec3 point, vec3 center, float radius)
{
	float oneMinusCos = SphereConeOneMinusCos(point, center, radius);
	return oneMinusCos > 0.0 ? 1.0 / (2.0 * BSDF_PI * oneMinusCos) : 0.0;
}

bool SampleSphereLight(vec3 point, vec3 center, float radius, float u1, float u2, out vec3 direction, out float pdf)
{
	direction = vec3(0.0);
	pdf = 0.0;
	float oneMinusCos = SphereConeOneMinusCos(point, center, radius);
	if (oneMinusCos <= 0.0)
		return false;

	float cosTheta = 1.0 - u1 * oneMinusCos;
	float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
	float phi = 2.0 * BSDF_PI * u2;

	vec3 w = normalize(center - point);
	vec3 t, b;
	BuildBasis(w, t, b);
	direction = normalize(t * (cos(phi) * sinTheta) + b * (sin(phi) * sinTheta) + w * cosTheta);
	pdf = 1.0 / (2.0 * BSDF_PI * oneMinusCos);
	return true;
}

float TriangleLightPdf(float distance, float cosLight, float area)
{
	return cosLight > 1e-6 ? distance * distance / (area * cosLight) : 0.0;
}

bool SampleTriangleLight(vec3 point, vec3 v0, vec3 v1, vec3 v2, vec3 normal, float area, float u1, float u2,
	out vec3 direction, out float distance, out float pdf)
{
	direction = vec3(0.0);
	pdf = 0.0;

	float su = sqrt(u1);
	float b0 = 1.0 - su;
	float b1 = u2 * su;
	vec3 target = b0 * v0 + b1 * v1 + (1.0 - b0 - b1) * v2;

	vec3 toLight = target - point;
	distance = length(toLight);
	if (distance <= 0.0)
		return false;
	direction = toLight / distance;

	pdf = TriangleLightPdf(distance, -dot(normal, direction), area);
	return pdf > 0.0;
}

#endif
