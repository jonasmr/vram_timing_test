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
5. Once per second it prints exactly one line to the console (see below).

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
vramtiming.exe <megabytes> [-flip | -promote]
```

For example `vramtiming.exe 512` (fits in video memory) or `vramtiming.exe 8192` (more than a 6 GB GPU
can hold). `-flip` (or `--flip`, before or after the size) adds a second set of render targets for a
while, see [-flip](#-flip) below. `-promote` (or `--promote`) adds a second set and then releases the
first one, see [-promote](#-promote) below. `-flip` and `-promote` cannot be combined.

At startup it waits 2 seconds before creating the D3D12 device, so that an ETW monitor started
separately (such as `dxtcl_monitor`) has its session running before any D3D12 object is created.

## Reading the output

```
t=5s  8192 MB | vram(fast) 3728MB/233 | sys(slow) 4464MB/279 | evicted(kernel) 4448MB | frame 613.4 ms
```

MB means MiB here.

| Field | Meaning |
|---|---|
| `t=5s` | seconds since rendering started |
| `8192 MB` | total size of the render targets that were created |
| `vram(fast) 3728MB/233` | render targets whose draws were fast during the last second, i.e. in video memory: their total size / their count |
| `sys(slow) 4464MB/279` | render targets whose draws were slow during the last second, i.e. in system memory: their total size / their count |
| `evicted(kernel)` | bytes of this process that the graphics kernel reports as demoted out of video memory (`D3DKMTQueryStatistics`, `D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT_GROUP`, Local group, sum of `Demoted[]`) |
| `frame` | GPU time for the whole chain (median over the last second) |

How a render target is classified as slow: for each render target take the median of its draw times
over the last second; the fast reference is the 10th percentile of those medians; a render target is
slow if its median is more than 4x the fast reference. This assumes at least 10% of the render targets
are in video memory, which holds unless the allocation is many times the GPU's memory.

If the timing and the kernel agree, `sys(slow)` and `evicted(kernel)` are about the same.

## -flip

```
vramtiming.exe 3072 -flip
```

Runs in three phases, 60 seconds each except the last:

1. **t = 0..60 s, set A.** Only set A exists: the `<megabytes> / 16` render targets from above,
   `RT_0` .. `RT_<N-1>`, rendered every frame as usual.
2. **t = 60..120 s, set B.** At 60 s it allocates set B, another N render targets of the same kind,
   named `RT_<N>` .. `RT_<2N-1>` (the names keep one global index, so tools that parse `RT_<index>`
   work unchanged), and from then on renders only set B. Set A stays allocated but is idle. If
   allocating set B runs out of memory, it continues with the render targets that were created and
   says so.
3. **t = 120 s until the window is closed, set A.** At 120 s it renders set A again, waits until the
   GPU is idle and releases all of set B. There are no further transitions.

Each frame only the active set is rendered, with the same chain as above (the first render target of
the set is noise from the seed, each next one reads the previous one). The window shows the last
render target of the active set. This shows whether the kernel moves the set that becomes active into
video memory (and the idle one out), and what happens when the extra memory is released again.

```
t=60s  set A RT_0..191 | 3072 MB | vram(fast) 3072MB/192 | sys(slow) 0MB/0 | evicted(kernel) 0MB | frame 22.9 ms
t=60s  allocated set B (RT_192..RT_383), rendering set B
t=61s  set B RT_192..383 | 2x3072 MB | vram(fast) 576MB/36 | sys(slow) 2496MB/156 | evicted(kernel) 2480MB | frame 729.5 ms
...
t=120s  set B RT_192..383 | 2x3072 MB | vram(fast) 592MB/37 | sys(slow) 2480MB/155 | evicted(kernel) 2480MB | frame 335.2 ms
t=120s  rendering set A, released set B (RT_192..RT_383)
t=121s  set A RT_0..191 | 3072 MB | vram(fast) 3072MB/192 | sys(slow) 0MB/0 | evicted(kernel) 0MB | frame 22.9 ms
```

| Field | Meaning |
|---|---|
| `allocated set B (...)`, `rendering set A, released set B (...)` | the transitions; printed right after the report for that second, and from this point on only the named set is rendered |
| `set B RT_192..383` | the set that was rendered (and measured) during the last second (`RT_192` .. `RT_383`) |
| `3072 MB`, `2x3072 MB` | what is allocated right now: set A alone, or sets A and B of 3072 MB each (`A+B MB` if set B came out smaller) |
| `vram(fast)`, `sys(slow)` | as above, but for the active set only. The idle set A is not drawn, so it is not measured: its render targets are never counted, wherever they are |
| `evicted(kernel)` | unchanged: the whole process, so while set B exists it includes the idle set A |
| `frame` | GPU time for the active set's chain |

On each transition the timing samples are discarded, so the first report after it only contains
draws of the new active set.

## -promote

```
vramtiming.exe 3072 -promote
```

Provokes the kernel to promote demoted render targets back into video memory: set B is created while
set A still occupies video memory, so part of B lands in system memory; once A is released there is
room, and B can move back into video memory. Runs in three phases, 10 seconds each except the last:

1. **t = 0..10 s, set A.** Only set A exists (`RT_0` .. `RT_<N-1>`) and is rendered, as with `-flip`.
2. **t = 10..20 s, set B.** At 10 s it allocates set B (`RT_<N>` .. `RT_<2N-1>`) and from then on
   renders only set B. Set A stays allocated but is idle.
3. **t = 20 s until the window is closed, set B alone.** At 20 s it waits until the GPU is idle and
   releases all of set A, and keeps rendering set B. There are no further transitions.

The output has the same fields as with `-flip`; the allocated size is `3072 MB` (A), `2x3072 MB`
(A and B), then `3072 MB` again (B alone). Promotion shows as `sys(slow)` and `evicted(kernel)`
dropping after the release of set A. On each transition the timing samples are discarded.

From a run on an RTX 2060 (6 GB):

```
t=10s  set A RT_0..191 | 3072 MB | vram(fast) 3072MB/192 | sys(slow) 0MB/0 | evicted(kernel) 0MB | frame 22.9 ms
t=10s  allocated set B (RT_192..RT_383), rendering set B
t=11s  set B RT_192..383 | 2x3072 MB | vram(fast) 3072MB/192 | sys(slow) 0MB/0 | evicted(kernel) 2944MB | frame 831.8 ms
...
t=20s  set B RT_192..383 | 2x3072 MB | vram(fast) 3072MB/192 | sys(slow) 0MB/0 | evicted(kernel) 2944MB | frame 394.6 ms
t=20s  released set A (RT_0..RT_191), still rendering set B
t=21s  set B RT_192..383 | 3072 MB | vram(fast) 3072MB/192 | sys(slow) 0MB/0 | evicted(kernel) 2912MB | frame 410.4 ms
...
t=29s  set B RT_192..383 | 3072 MB | vram(fast) 336MB/21 | sys(slow) 2736MB/171 | evicted(kernel) 2656MB | frame 388.6 ms
...
t=42s  set B RT_192..383 | 3072 MB | vram(fast) 1776MB/111 | sys(slow) 1296MB/81 | evicted(kernel) 1184MB | frame 180.3 ms
```

In this run the kernel promoted set B gradually, about 100 MB per second, starting a few seconds after
set A was released. Note that from 11 s to 28 s every render target was classified fast while frames
took ~400 ms: that is the batch-paging case described under [Validity](#validity), so the timing is
not ground truth there; from 29 s on it is, and it agrees with `evicted(kernel)`.

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
