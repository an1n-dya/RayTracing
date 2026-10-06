#pragma once

// User-facing renderer options, shared by the CPU path (Renderer) and the GPU path (GpuPathTracer).
struct RenderSettings {
	bool Accumulate = true;
	int MaxSamples = 0; // stop accumulating after this many samples per pixel; 0 = never stop
	bool SlowRandom = false; // CPU only: use Walnut::Random instead of the PCG hash
	bool UseGPU = false;

	int MaxBounces = 5;
	bool RussianRoulette = true;
	int RussianRouletteStartBounce = 3; // paths are never terminated before this bounce
};
