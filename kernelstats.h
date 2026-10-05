// kernelstats: what the graphics kernel (VidMm) itself says about this process's video memory.
//
// Kept in its own translation unit so the kernel-thunk header (d3dkmthk.h) stays out of the test code.

#pragma once

#include <windows.h>
#include <cstdint>

// Bytes of the current process's allocations on the given adapter that the kernel has demoted out of
// local (video) memory, i.e. that currently live in system memory although they would prefer video
// memory. This is D3DKMTQueryStatistics(D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT_GROUP, Local).Demoted[],
// summed over all priority classes. Returns false if the query fails.
bool QueryKernelDemotedBytes(LUID adapterLuid, uint64_t* demotedBytes);
