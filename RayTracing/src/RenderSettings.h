#pragma once

#include <cstdint>

enum class ToneMapper : int {
	None = 0,     // clamp
	Reinhard = 1,
	ACES = 2,
};

// What a single Render() call should do, computed by Renderer from the settings and accumulation state
struct FrameParams {
	bool ResetAccumulation = true; // discard previously accumulated samples first
	uint32_t FirstSampleIndex = 0; // global index of the first sample traced this frame (seeds the RNG)
	uint32_t SampleCount = 1;      // samples per pixel to trace this frame; 0 = only re-resolve the image
};

// User-facing renderer options, shared by the CPU path (Renderer) and the GPU path (GpuPathTracer).
struct RenderSettings {
	bool Accumulate = true;
	int MaxSamples = 0; // stop accumulating after this many samples per pixel; 0 = never stop
	int SamplesPerFrame = 1;
	bool SlowRandom = false; // CPU only: use Walnut::Random instead of the PCG hash
	bool UseGPU = false;

	bool AntiAliasing = true; // jitter each sample within its pixel (off = always the pixel center)

	int MaxBounces = 5;
	bool RussianRoulette = true;
	int RussianRouletteStartBounce = 3; // paths are never terminated before this bounce

	// Post-processing (applied to the accumulated image, so changing these never restarts accumulation)
	float Exposure = 0.0f; // in stops (EV)
	ToneMapper ToneMapping = ToneMapper::ACES;
	bool SRGBOutput = true; // the swapchain is UNORM, so encode to sRGB ourselves
};
