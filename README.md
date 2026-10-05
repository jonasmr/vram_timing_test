# vramtiming

A small D3D12 test that shows, from GPU timing alone, which of its render targets live in video
memory and which have been demoted to system memory. It also prints the graphics kernel's own count of
demoted bytes next to that, so the two can be compared.

## What it does

1. Allocates `<megabytes> / 16` render targets of exactly 16 MiB each (2048x2048 `R8G8B8A8_UNORM`,
   committed resources in the DEFAULT heap), named `RT_0`, `RT_1`, ... with `SetName`.
   If creation fails (out of memory) it prints how many it got and continues with those.
2. Every frame it draws all of them in order, as a chain: `RT[0]` is noise from a new random seed,
   `RT[i]` is noise mixed with `RT[i-1]`. So every render target is read and written every frame.
3. Each render target's draw is bracketed by GPU timestamps. A render target in video memory takes
   roughly 0.05-0.1 ms, one in system memory (accessed over PCIe) more than 1 ms.
4. The window (1024x1024) shows the last render target of the chain, so you can see it is running.
5. Once per second it prints one line to the console (see below).

It runs until the window is closed.

## Build

Visual Studio 2022, x64. Only inbox `d3d12`, `dxgi` and `d3dcompiler` are used (no NuGet, no Agility
SDK). From a VS 2022 developer command prompt:

```
msbuild vram-timing.sln /p:Configuration=Release /p:Platform=x64
```

The executable is written to `build\bin\Release\vramtiming.exe`.

## Run

```
vramtiming.exe <megabytes>
```

For example `vramtiming.exe 512` (fits in video memory) or `vramtiming.exe 8192` (more than a 6 GB GPU
can hold).

## Reading the output

```
t=5s  allocated 8192 MB | VRAM (fast) 3728 MB [233 RTs] | system memory (slow) 4464 MB [279 RTs] | evicted (kernel) 4448 MB | frame 613.4 ms
  slow: 231-334, 337-511
```

MB means MiB here.

| Field | Meaning |
|---|---|
| `t=5s` | seconds since rendering started |
| `allocated` | total size of the render targets that were created |
| `VRAM (fast)` | render targets whose draws were fast during the last second, i.e. in video memory |
| `system memory (slow)` | render targets whose draws were slow during the last second, i.e. in system memory |
| `evicted (kernel)` | bytes of this process that the graphics kernel reports as demoted out of video memory (`D3DKMTQueryStatistics`, `D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT_GROUP`, Local group, sum of `Demoted[]`) |
| `frame` | GPU time for the whole chain (median over the last second) |
| `slow: ...` | printed whenever the set of slow render targets changes: their indices (`RT_<index>`), as ranges |

How a render target is classified as slow: for each render target take the median of its draw times
over the last second; the fast reference is the 10th percentile of those medians; a render target is
slow if its median is more than 4x the fast reference. This assumes at least 10% of the render targets
are in video memory, which holds unless the allocation is many times the GPU's memory.

If the timing and the kernel agree, `system memory (slow)` and `evicted (kernel)` are about the same.

## Files

| File | Content |
|---|---|
| `main.cpp` | the test: render targets, frame loop, timing, classification, report |
| `shaders.h` | the HLSL (fullscreen triangle, chain pass, show pass), compiled at startup |
| `d3d_helpers.h/.cpp` | boilerplate: window, device, swap chain, descriptor heaps, shaders, pipelines |
| `kernelstats.h/.cpp` | the kernel's demoted-bytes query |
"# vram_timing_test" 
