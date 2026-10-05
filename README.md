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
vramtiming.exe <megabytes> [-flip]
```

For example `vramtiming.exe 512` (fits in video memory) or `vramtiming.exe 8192` (more than a 6 GB GPU
can hold). `-flip` (or `--flip`, before or after the size) switches between two sets of render
targets, see [-flip](#-flip) below.

At startup it waits 2 seconds before creating the D3D12 device, so that an ETW monitor started
separately (such as `dxtcl_monitor`) has its session running before any D3D12 object is created.

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

## -flip

```
vramtiming.exe 3072 -flip
```

Allocates twice the requested amount, as two equally sized sets of 16 MiB render targets:
set A = `RT_0` .. `RT_<N-1>` and set B = `RT_<N>` .. `RT_<2N-1>` (N = `<megabytes> / 16`; the names
keep one global index, so tools that parse `RT_<index>` work unchanged). If creation runs out of
memory, the render targets that were created are split into two halves and a line says so.

Each frame only the active set is rendered, with the same chain as above (the first render target of
the set is noise from the seed, each next one reads the previous one). It starts with set A and switches
to the other set every 10 seconds, so one set is in use while the other sits idle. The window shows the
last render target of the active set. This shows whether the kernel moves the set that becomes active
back into video memory (and the idle one out).

```
t=10s  [set A: RT_0..RT_191]  allocated 2 x 3072 MB | VRAM (fast) 2944 MB [184 RTs] | system memory (slow) 128 MB [8 RTs] | evicted (kernel) 3056 MB | frame 34.3 ms
t=10s  flip -> rendering set B (RT_192..RT_383)
t=11s  [set B: RT_192..RT_383]  allocated 2 x 3072 MB | VRAM (fast) 528 MB [33 RTs] | system memory (slow) 2544 MB [159 RTs] | evicted (kernel) 3056 MB | frame 728.0 ms
  slow: 224-315, 317-383
```

| Field | Meaning |
|---|---|
| `flip -> rendering set B (...)` | from this point on only set B is rendered; printed right after the report for that second |
| `[set B: RT_192..RT_383]` | the set that was rendered (and measured) during the last second |
| `allocated 2 x 3072 MB` | two sets of 3072 MB each |
| `VRAM (fast)`, `system memory (slow)`, `slow:` | as above, but for the active set only. The idle set is not drawn, so it is not measured: its render targets are never counted or listed, wherever they are. The `slow:` ranges use the global `RT_<index>` |
| `evicted (kernel)` | unchanged: the whole process, so it includes the idle set |
| `frame` | GPU time for the active set's chain |

On a flip the timing samples are discarded, so the first report after a flip only contains draws of
the new set, and its `slow:` line is always printed.

## Validity

The per-render-target timing only detects demotion when demoted render targets are actually accessed
in system memory. If video memory is extremely constrained, the kernel may instead page whole batches
of allocations in and out between command lists. Then every render target looks fast during its own
draw, and the cost shows up only as a huge frame time. On an RTX 2060 a normal 8192 MB run shows frames
of about 600 ms; we once saw frames of 1-19 s with every render target classified fast. Treat runs with
such frame times as invalid ground truth.

## Files

| File | Content |
|---|---|
| `main.cpp` | the test: render targets, frame loop, timing, classification, report |
| `shaders.h` | the HLSL (fullscreen triangle, chain pass, show pass), compiled at startup |
| `d3d_helpers.h/.cpp` | boilerplate: window, device, swap chain, descriptor heaps, shaders, pipelines |
| `kernelstats.h/.cpp` | the kernel's demoted-bytes query |
"# vram_timing_test" 
