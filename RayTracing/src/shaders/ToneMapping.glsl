// Tone mapping: same as ToneMapping.h - keep the two in sync.

#ifndef TONE_MAPPING_GLSL
#define TONE_MAPPING_GLSL

vec3 ACESFilm(vec3 x)
{
	const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
	return clamp((x * (a * x + b)) / (x * (c * x + d) + e), vec3(0.0), vec3(1.0));
}

vec3 LinearToSRGB(vec3 c)
{
	return mix(12.92 * c, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, greaterThan(c, vec3(0.0031308)));
}

// toneMapper: 0 = none (clamp), 1 = Reinhard, 2 = ACES
vec3 ApplyToneMapping(vec3 color, float exposure, uint toneMapper, uint sRGB)
{
	color *= exp2(exposure);

	if (toneMapper == 1u)
		color = color / (1.0 + color);
	else if (toneMapper == 2u)
		color = ACESFilm(color);

	color = clamp(color, vec3(0.0), vec3(1.0));
	if (sRGB != 0u)
		color = LinearToSRGB(color);
	return color;
}

#endif
