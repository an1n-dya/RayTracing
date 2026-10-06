#pragma once

// User-facing renderer options, shared by the CPU path (Renderer) and the GPU path (GpuPathTracer).
struct RenderSettings {
	bool Accumulate = true;
	bool SlowRandom = false; // CPU only: use Walnut::Random instead of the PCG hash
	bool UseGPU = false;

	int MaxBounces = 5;
	bool RussianRoulette = true;
	int RussianRouletteStartBounce = 3; // paths are never terminated before this bounce
};
