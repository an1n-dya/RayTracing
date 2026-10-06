// Equirectangular environment lookups and importance sampling. C++ counterpart: EnvironmentSampling.h.
// Expects these to be declared before inclusion: EnvTexels[] (vec4), EnvMarginalCDF[], EnvConditionalCDF[], EnvPdf[]
// (floats), and EnvSize (uvec2: width, height). rotation is in turns around +Y.

#ifndef ENVIRONMENT_GLSL
#define ENVIRONMENT_GLSL

const float ENV_PI = 3.14159265358979;

vec2 EnvDirectionToUV(vec3 direction, float rotation)
{
	float u = atan(direction.z, direction.x) / (2.0 * ENV_PI) + 0.5 - rotation;
	u -= floor(u);
	float v = acos(clamp(direction.y, -1.0, 1.0)) / ENV_PI;
	return vec2(u, v);
}

vec3 EnvUVToDirection(vec2 uv, float rotation)
{
	float phi = 2.0 * ENV_PI * (uv.x - 0.5 + rotation);
	float theta = ENV_PI * uv.y;
	return vec3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi));
}

vec3 EnvTexel(int x, int y)
{
	return EnvTexels[uint(y) * EnvSize.x + uint(x)].rgb;
}

vec3 EnvLookup(vec3 direction, float rotation)
{
	int width = int(EnvSize.x), height = int(EnvSize.y);
	vec2 uv = EnvDirectionToUV(direction, rotation);
	float px = uv.x * float(width) - 0.5;
	float py = uv.y * float(height) - 0.5;
	float fx = floor(px), fy = floor(py);
	float tx = px - fx, ty = py - fy;
	int x0 = int(fx) < 0 ? width - 1 : int(fx); // u is in [0,1), so fx >= -1 (GLSL % is undefined for negatives)
	int x1 = x0 + 1 < width ? x0 + 1 : 0;
	int y0 = clamp(int(fy), 0, height - 1), y1 = clamp(int(fy) + 1, 0, height - 1);

	vec3 top = mix(EnvTexel(x0, y0), EnvTexel(x1, y0), tx);
	vec3 bottom = mix(EnvTexel(x0, y1), EnvTexel(x1, y1), tx);
	return mix(top, bottom, ty);
}

float EnvPdfForDirection(vec3 direction, float rotation)
{
	float sinTheta = sqrt(max(0.0, 1.0 - direction.y * direction.y));
	if (sinTheta <= 0.0)
		return 0.0;
	vec2 uv = EnvDirectionToUV(direction, rotation);
	uint x = min(uint(uv.x * float(EnvSize.x)), EnvSize.x - 1u);
	uint y = min(uint(uv.y * float(EnvSize.y)), EnvSize.y - 1u);
	return EnvPdf[y * EnvSize.x + x] / (2.0 * ENV_PI * ENV_PI * sinTheta);
}

uint EnvSearchMarginal(float u)
{
	uint low = 0u, high = EnvSize.y - 1u;
	while (low < high)
	{
		uint middle = (low + high) / 2u;
		if (u < EnvMarginalCDF[middle])
			high = middle;
		else
			low = middle + 1u;
	}
	return low;
}

uint EnvSearchConditional(uint row, float u)
{
	uint low = 0u, high = EnvSize.x - 1u;
	while (low < high)
	{
		uint middle = (low + high) / 2u;
		if (u < EnvConditionalCDF[row + middle])
			high = middle;
		else
			low = middle + 1u;
	}
	return low;
}

bool SampleEnvironment(float rotation, float u1, float u2, out vec3 direction, out float pdf)
{
	direction = vec3(0.0, 1.0, 0.0);
	pdf = 0.0;
	uint width = EnvSize.x, height = EnvSize.y;

	uint y = EnvSearchMarginal(u1);
	float rowStart = y > 0u ? EnvMarginalCDF[y - 1u] : 0.0;
	float dv = (u1 - rowStart) / max(EnvMarginalCDF[y] - rowStart, 1e-20);

	uint row = y * width;
	uint x = EnvSearchConditional(row, u2);
	float columnStart = x > 0u ? EnvConditionalCDF[row + x - 1u] : 0.0;
	float du = (u2 - columnStart) / max(EnvConditionalCDF[row + x] - columnStart, 1e-20);

	vec2 uv = vec2((float(x) + clamp(du, 0.0, 1.0)) / float(width), (float(y) + clamp(dv, 0.0, 1.0)) / float(height));
	direction = EnvUVToDirection(uv, rotation);

	float sinTheta = sin(ENV_PI * uv.y);
	if (sinTheta <= 0.0)
		return false;
	pdf = EnvPdf[row + x] / (2.0 * ENV_PI * ENV_PI * sinTheta);
	return pdf > 0.0;
}

#endif
