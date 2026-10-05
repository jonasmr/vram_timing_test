#include "kernelstats.h"

#include <winternl.h> // NTSTATUS, needed by d3dkmthk.h
#include <d3dkmthk.h> // D3DKMTQueryStatistics (gdi32.dll)

bool QueryKernelDemotedBytes(LUID adapterLuid, uint64_t* demotedBytes)
{
    D3DKMT_QUERYSTATISTICS query{};
    query.Type = D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT_GROUP;
    query.AdapterLuid = adapterLuid;
    query.hProcess = GetCurrentProcess();
    query.QueryProcessSegmentGroup = D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL;
    if (D3DKMTQueryStatistics(&query) < 0) // NTSTATUS failure
        return false;

    // Demoted[] is split by allocation priority class (minimum, low, normal, high, maximum).
    uint64_t total = 0;
    for (UINT64 bytes : query.QueryResult.ProcessSegmentGroupInformation.Demoted)
        total += bytes;
    *demotedBytes = total;
    return true;
}
