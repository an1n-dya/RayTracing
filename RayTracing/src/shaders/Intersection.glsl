// Ray-primitive intersection tests. C++ counterpart: Intersection.h - keep the two in sync.
// Directions don't need to be normalized; a miss is reported as a negative distance.

#ifndef INTERSECTION_GLSL
#define INTERSECTION_GLSL

// Near hit, or the far one if the ray starts inside the sphere
float IntersectSphere(vec3 origin, vec3 direction, vec3 center, float radius)
{
	vec3 oc = origin - center;
	float a = dot(direction, direction);
	float b = 2.0 * dot(oc, direction);
	float c = dot(oc, oc) - radius * radius;

	float discriminant = b * b - 4.0 * a * c;
	if (discriminant < 0.0)
		return -1.0;

	float sqrtDiscriminant = sqrt(discriminant);
	float t = (-b - sqrtDiscriminant) / (2.0 * a);
	if (t <= 0.0)
		t = (-b + sqrtDiscriminant) / (2.0 * a);
	return t;
}

float IntersectPlane(vec3 origin, vec3 direction, vec3 point, vec3 normal)
{
	float denominator = dot(direction, normal);
	if (abs(denominator) < 1e-8)
		return -1.0;
	return dot(point - origin, normal) / denominator;
}

// Moller-Trumbore, double sided. Returns t and the barycentrics of v1 and v2 in uv.
float IntersectTriangle(vec3 origin, vec3 direction, vec3 v0, vec3 v1, vec3 v2, out vec2 uv)
{
	uv = vec2(0.0);
	vec3 edge1 = v1 - v0;
	vec3 edge2 = v2 - v0;
	vec3 p = cross(direction, edge2);
	float determinant = dot(edge1, p);
	if (abs(determinant) < 1e-12)
		return -1.0;

	float inverseDeterminant = 1.0 / determinant;
	vec3 s = origin - v0;
	uv.x = dot(s, p) * inverseDeterminant;
	if (uv.x < 0.0 || uv.x > 1.0)
		return -1.0;

	vec3 q = cross(s, edge1);
	uv.y = dot(direction, q) * inverseDeterminant;
	if (uv.y < 0.0 || uv.x + uv.y > 1.0)
		return -1.0;

	return dot(edge2, q) * inverseDeterminant;
}

float IntersectAABB(vec3 origin, vec3 inverseDirection, vec3 boxMin, vec3 boxMax, float maxDistance)
{
	vec3 t0 = (boxMin - origin) * inverseDirection;
	vec3 t1 = (boxMax - origin) * inverseDirection;
	vec3 tNear = min(t0, t1);
	vec3 tFar = max(t0, t1);
	float enter = max(max(tNear.x, tNear.y), max(tNear.z, 0.0));
	float exit = min(min(tFar.x, tFar.y), min(tFar.z, maxDistance));
	return enter <= exit ? enter : -1.0;
}

#endif
