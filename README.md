# vramtiming

A small D3D12 test that shows, from GPU timing alone, which of its render targets live in video
memory and which have been demoted to system memory. It also prints the graphics kernel's own count of
demoted bytes next to that, so the two can be compared.

## What it does

1. The command line defines one or more **blocks** of render targets, named `A`, `B`, `C`, ... Each
   render target is exactly 16 MiB (2048x2048 `R8G8B8A8_UNORM`, a committed resource in the DEFAULT
   heap), named `RT_<index>` with `SetName`. The indices run on over all blocks: `A` is `RT_0` ..
   `RT_<a-1>`, `B` the next ones, and so on.
2. The blocks are rendered one at a time, in order, each for the given number of seconds. Every frame
   it draws all render targets of the active block, as a chain: the first one is noise from a new
   random seed, each next one is noise mixed with the previous one. So every render target of the
   active block is read and written every frame.
3. Each render target's draw is bracketed by GPU timestamps. A render target in video memory takes
   roughly 0.05-0.1 ms, one in system memory (accessed over PCIe) more than 1 ms.
4. The window (1024x1024) shows the last render target of the active block, so you can see it is
   running.
5. Once per second it prints exactly one line to the console (see below), plus one line whenever the
   active block changes.

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
vramtiming.exe <seconds> <size>[d] [<size>[d] ...] [-flip]
```

- `<seconds>`: how long each block is active.
- Each `<size>` (in MB) defines a block. Sizes are rounded up to whole 16 MiB render targets (at least
  one); if a size is rounded, the actual size is printed.
- A block is allocated at startup (in order) and kept until exit, unless its size ends in `d`
  (**dynamic**): a dynamic block exists only while it is active. It is allocated when it becomes active
  and freed (after waiting until the GPU is idle) when the next block becomes active. A dynamic block
  gets the same render target indices, so the same names, every time it is allocated. Block `A` is
  active first, so it is allocated at startup even if it is dynamic.
- After the last block it stays on the last block. With `-flip` (anywhere on the command line) it
  starts over at `A` instead, forever.
- If allocating runs out of memory, it continues with the render targets that were created and says so.

Examples:

| Command | What happens |
|---|---|
| `vramtiming 15 512 512 512` | A, B, C (all allocated at startup), 15 s each, then stays on C |
| `vramtiming 20 32 128d -flip` | A (32 MB) is allocated at startup; at 20 s B (128 MB) is allocated and rendered; at 40 s B is freed and A rendered again; and so on |
| `vramtiming 10 8192` | one block, rendered until the window is closed |
| `vramtiming 10 3072d 3072` | the promotion experiment, see [Promotion](#promotion) |

At startup it waits 2 seconds before creating the D3D12 device, so that an ETW monitor started
separately (such as `dxtcl_monitor`) has its session running before any D3D12 object is created.

## Reading the output

```
blocks: A 32 MB, B 128 MB dynamic | 3 s each | -flip: loop
allocated 32 MB at startup, rendering block A; close the window to exit
t=1s  block A RT_0..1 | alloc 32 MB | vram(fast) 32MB/2 | sys(slow) 0MB/0 | evicted(kernel) 0MB | frame 0.2 ms
...
t=3s  block A RT_0..1 | alloc 32 MB | vram(fast) 32MB/2 | sys(slow) 0MB/0 | evicted(kernel) 0MB | frame 0.2 ms
t=3s  -> block B RT_2..9 (128 MB, allocated now)
t=4s  block B RT_2..9 | alloc 160 MB | vram(fast) 128MB/8 | sys(slow) 0MB/0 | evicted(kernel) 0MB | frame 0.8 ms
...
t=6s  freed block B RT_2..9 (128 MB) -> block A RT_0..1 (32 MB)
t=7s  block A RT_0..1 | alloc 32 MB | vram(fast) 32MB/2 | sys(slow) 0MB/0 | evicted(kernel) 0MB | frame 0.2 ms
```

MB means MiB here.

| Field | Meaning |
|---|---|
| `t=4s` | seconds since rendering started |
| `block B RT_2..9` | the active block, rendered and measured during the last second, and its render targets |
| `alloc 160 MB` | total size of all render targets allocated right now, over all blocks |
| `vram(fast) 128MB/8` | render targets of the active block whose draws were fast during the last second, i.e. in video memory: their total size / their count |
| `sys(slow) 0MB/0` | render targets of the active block whose draws were slow during the last second, i.e. in system memory: their total size / their count |
| `evicted(kernel)` | bytes of this process that the graphics kernel reports as demoted out of video memory (`D3DKMTQueryStatistics`, `D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT_GROUP`, Local group, sum of `Demoted[]`). This is the whole process, so it includes idle blocks |
| `frame` | GPU time for the active block's chain (median over the last second) |
| `-> block B ...`, `freed block B ... -> block A ...` | a transition, printed right after the report for that second: which block was freed (if it is dynamic), and which block is active from now on (`allocated now` if it is dynamic) |

Only the active block is drawn, so only it is measured: render targets of idle blocks are never
counted in `vram(fast)` / `sys(slow)`, wherever they are. On each transition the timing samples are
discarded, so the first report after it only contains draws of the new active block.

How a render target is classified as slow: for each render target take the median of its draw times
over the last second; the fast reference is the 10th percentile of those medians; a render target is
slow if its median is more than 4x the fast reference. This assumes at least 10% of the render targets
are in video memory, which holds unless the allocation is many times the GPU's memory.

If the timing and the kernel agree, `sys(slow)` and `evicted(kernel)` are about the same (while only
the active block exists).

## Promotion

```
vramtiming.exe 10 3072d 3072
```

Provokes the kernel to promote demoted render targets back into video memory. Both blocks are
allocated at startup, A first, so under memory pressure much of B lands in system memory. A is rendered
for 10 s; then A is freed and B is rendered until the window is closed. Once A is gone there is room,
and the timing shows whether, and how fast, the kernel promotes B back into video memory.

The sizes have to add up to more than the GPU can hold: `10 3072d 3072` is meant for a 6 GB GPU. From a
run of `vramtiming 10 6144d 6144` on an RTX 3060 (12 GB):

```
blocks: A 6144 MB dynamic, B 6144 MB | 10 s each | then stays on the last block
allocated 12288 MB at startup, rendering block A; close the window to exit
t=10s  block A RT_0..383 | alloc 12288 MB | vram(fast) 5936MB/371 | sys(slow) 208MB/13 | evicted(kernel) 3712MB | frame 71.5 ms
t=10s  freed block A RT_0..383 (6144 MB) -> block B RT_384..767 (6144 MB)
t=11s  block B RT_384..767 | alloc 6144 MB | vram(fast) 2240MB/140 | sys(slow) 3904MB/244 | evicted(kernel) 3104MB | frame 1296.2 ms
t=13s  block B RT_384..767 | alloc 6144 MB | vram(fast) 2896MB/181 | sys(slow) 3248MB/203 | evicted(kernel) 2464MB | frame 512.3 ms
t=16s  block B RT_384..767 | alloc 6144 MB | vram(fast) 4064MB/254 | sys(slow) 2080MB/130 | evicted(kernel) 1216MB | frame 355.0 ms
t=19s  block B RT_384..767 | alloc 6144 MB | vram(fast) 5696MB/356 | sys(slow) 448MB/28 | evicted(kernel) 96MB | frame 93.2 ms
t=22s  block B RT_384..767 | alloc 6144 MB | vram(fast) 6144MB/384 | sys(slow) 0MB/0 | evicted(kernel) 0MB | frame 40.5 ms
```

In this run the kernel promoted block B gradually, about 400 MB per second, starting right after block
A was freed; after 12 s all of B was in video memory, and the timing agrees with `evicted(kernel)`.

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
| `main.cpp` | the test: command line, blocks, render targets, frame loop, timing, classification, report |
| `shaders.h` | the HLSL (fullscreen triangle, chain pass, show pass), compiled at startup |
| `d3d_helpers.h/.cpp` | boilerplate: window, device, swap chain, descriptor heaps, shaders, pipelines |
| `kernelstats.h/.cpp` | the kernel's demoted-bytes query |
"# vram_timing_test" 
