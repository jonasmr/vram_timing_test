// vramtiming: ground-truth test for GPU memory demotion.
//
// Usage: vramtiming <seconds> <size>[d] [<size>[d] ...] [-flip]
//
// Each <size> (in MB) defines a block of render targets of 16 MiB each; the blocks are named A, B, C, ...
// in order. The blocks are rendered one at a time, each for <seconds>, and every frame draws ALL render
// targets of the active block. When the total exceeds what the OS lets this process keep in video memory,
// the graphics kernel demotes some of them to system memory, and the GPU then has to read and write those
// over PCIe, which is ~25x slower. Timing each render target's draw with GPU timestamps therefore shows
// which render targets live in video memory (fast) and which in system memory (slow). Once per second this
// is printed next to the kernel's own count of demoted bytes, so the two can be compared.
//
// A block is allocated at startup and kept until exit, unless its size ends in 'd' (dynamic): a dynamic
// block exists only while it is the active block. It is allocated when it becomes active and freed when
// the next block becomes active. After the last block it stays on the last block, or with -flip starts
// over at A, until the window is closed.
//
// "MB" in the output means MiB (1024 * 1024 bytes).

#include "d3d_helpers.h"
#include "kernelstats.h"
#include "shaders.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <random>
#include <string>
#include <vector>

namespace
{

constexpr UINT kRtSize = 2048; // 2048 x 2048 x 4 bytes = 16 MiB
constexpr DXGI_FORMAT kRtFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr UINT kRtMegabytes = 16;
constexpr UINT kWindowSize = 1024;
// Draws per command list. A draw into system memory takes more than 1 ms, so the frame is split into
// several command lists (all submitted together) to keep every GPU packet well below the TDR limit.
constexpr UINT kDrawsPerCommandList = 32;
constexpr double kSlowFactor = 4.0; // see Classify()
constexpr unsigned long kMaxBlockMegabytes = 1 << 20; // 1 TiB, just to reject nonsense

// ---- Test state ----

// A block of render targets from the command line. Each block owns a fixed range of render target
// indices, in command line order: A = RT_0 .. RT_<a-1>, B the next ones, and so on. A dynamic block
// uses the same range (so the same names, descriptors and queries) every time it is allocated.
struct Block
{
    char name;      // 'A', 'B', ...
    UINT first;     // its render targets are RT_<first> .. RT_<first + count - 1>
    UINT count;     // render targets it should have: its size rounded up to whole 16 MiB
    bool dynamic;   // allocated only while it is the active block
    UINT allocated; // render targets that exist right now: 0 while freed, count, or fewer if out of memory
};

Gpu g_gpu;
std::vector<Block> g_blocks;
UINT g_active = 0;            // index of the block that is rendered and measured
ULONGLONG g_blockSeconds = 0; // how long each block is active
bool g_flip = false;          // after the last block, start over at A (otherwise stay on the last block)

std::vector<ComPtr<ID3D12Resource>> g_rts; // RT i, for all blocks; null while its block is not allocated
DescriptorHeap g_rtvs;                     // RTV i = RT i
DescriptorHeap g_srvs;                     // SRV i = RT i, SRV RtCount() = null (first RT of a block has no predecessor)
ComPtr<ID3D12RootSignature> g_rootSignature;
ComPtr<ID3D12PipelineState> g_chainPipeline;
ComPtr<ID3D12PipelineState> g_showPipeline;
std::vector<CommandList> g_chainLists;
CommandList g_showList;
ComPtr<ID3D12QueryHeap> g_timestamps; // 2 per RT: before and after its draw
ComPtr<ID3D12Resource> g_timestampReadback;
double g_msPerTick = 0;

std::mt19937 g_random{ std::random_device{}() };
std::vector<std::vector<double>> g_drawMs; // per RT: its draw times during the current second
std::vector<double> g_frameMs;             // per frame of the current second: GPU time of the whole chain

Block& Active() { return g_blocks[g_active]; }
UINT ActiveLast() { return Active().first + Active().allocated - 1; }
UINT RtCount() { return g_blocks.back().first + g_blocks.back().count; } // over all blocks

UINT AllocatedMegabytes()
{
    UINT count = 0;
    for (const Block& block : g_blocks)
        count += block.allocated;
    return count * kRtMegabytes;
}

// ---- Command line ----

void PrintUsage()
{
    printf("Usage: vramtiming <seconds> <size>[d] [<size>[d] ...] [-flip]\n"
           "  Each <size> (MB) defines a block of 16 MiB render targets, named A, B, C, ... in order.\n"
           "  The blocks are rendered in order, <seconds> each: every frame draws all render targets of the\n"
           "  active block, and once per second it prints how many are in video memory (fast) and how many\n"
           "  in system memory (slow). Blocks are allocated at startup and kept, except a block whose size\n"
           "  ends in d (dynamic): it is allocated when it becomes active and freed when the next one does.\n"
           "  After the last block it stays there; with -flip it starts over at A. Close the window to exit.\n"
           "  Examples:\n"
           "    vramtiming 15 512 512 512    A, B, C (all allocated at startup) 15 s each, then stays on C\n"
           "    vramtiming 20 32 128d -flip  A; at 20 s allocate B and render it; at 40 s free B, back to A; loop\n"
           "    vramtiming 10 8192           one block of 8192 MB\n"
           "    vramtiming 10 3072d 3072     A and B exist at startup; at 10 s free A and render B forever\n");
}

// Fills g_blockSeconds, g_blocks and g_flip from the command line. Returns false if it is invalid.
bool ParseArguments(int argc, wchar_t** argv)
{
    std::vector<const wchar_t*> numbers; // everything except -flip: <seconds>, then the sizes
    for (int i = 1; i < argc; ++i)
    {
        if (wcscmp(argv[i], L"-flip") == 0 || wcscmp(argv[i], L"--flip") == 0)
            g_flip = true;
        else if (iswdigit(argv[i][0]))
            numbers.push_back(argv[i]);
        else
            return false;
    }
    if (numbers.size() < 2 || numbers.size() > 1 + 26)
        return false;

    wchar_t* end = nullptr;
    g_blockSeconds = wcstoul(numbers[0], &end, 10);
    if (g_blockSeconds == 0 || *end != L'\0')
        return false;

    UINT first = 0;
    for (size_t i = 1; i < numbers.size(); ++i)
    {
        const unsigned long megabytes = wcstoul(numbers[i], &end, 10);
        const bool dynamic = *end == L'd';
        if (megabytes > kMaxBlockMegabytes || end[dynamic ? 1 : 0] != L'\0')
            return false;
        const UINT count = std::max(1u, UINT(megabytes + kRtMegabytes - 1) / kRtMegabytes);
        g_blocks.push_back({ char('A' + i - 1), first, count, dynamic, 0 });
        first += count;
        if (count * kRtMegabytes != megabytes)
            printf("block %c: %lu MB rounded up to %u MB (whole 16 MiB render targets)\n",
                g_blocks.back().name, megabytes, count * kRtMegabytes);
    }
    return true;
}

// ---- Setup ----

// RTV and SRV heaps for the render targets of all blocks, plus the null SRV after them.
void CreateDescriptorHeaps()
{
    g_rtvs = CreateDescriptorHeap(g_gpu.device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RtCount(), false);
    g_srvs = CreateDescriptorHeap(g_gpu.device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, RtCount() + 1, true);
    CreateNullTextureSrv(g_gpu.device.Get(), kRtFormat, g_srvs.Cpu(RtCount()));
}

// Creates the block's render targets: committed 16 MiB render targets in the DEFAULT heap, named
// RT_<index>, with their RTV and SRV. If the device runs out of memory, it stops there and says so
// (and exits if it could not create a single one).
void CreateBlock(Block& block)
{
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kRtSize;
    desc.Height = kRtSize;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = kRtFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    for (block.allocated = 0; block.allocated < block.count; ++block.allocated)
    {
        const UINT index = block.first + block.allocated;
        HRESULT hr = g_gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&g_rts[index]));
        if (FAILED(hr))
        {
            printf("CreateCommittedResource failed (hr=0x%08X) after %u of %u render targets of block %c, "
                   "continuing with %u\n", (unsigned)hr, block.allocated, block.count, block.name, block.allocated);
            if (block.allocated == 0)
                CheckHr(hr, "creating any render target of the block");
            return;
        }
        wchar_t name[32];
        swprintf_s(name, L"RT_%u", index);
        g_rts[index]->SetName(name);
        g_gpu.device->CreateRenderTargetView(g_rts[index].Get(), nullptr, g_rtvs.Cpu(index));
        g_gpu.device->CreateShaderResourceView(g_rts[index].Get(), nullptr, g_srvs.Cpu(index));
    }
}

// Releases the block's render targets, once the GPU no longer uses them.
void FreeBlock(Block& block)
{
    WaitForGpu(g_gpu);
    for (UINT i = 0; i < block.allocated; ++i)
        g_rts[block.first + i].Reset();
    block.allocated = 0;
}

void CreatePipelines()
{
    ID3D12Device* device = g_gpu.device.Get();
    g_rootSignature = CreateRootSignature(device, 2); // 2 root constants: seed, index
    ComPtr<ID3DBlob> vs = CompileShader(kShaders, "FullscreenVS", "vs_5_0");
    g_chainPipeline = CreateFullscreenPipeline(device, g_rootSignature.Get(), vs.Get(),
        CompileShader(kShaders, "ChainPS", "ps_5_0").Get(), kRtFormat);
    g_showPipeline = CreateFullscreenPipeline(device, g_rootSignature.Get(), vs.Get(),
        CompileShader(kShaders, "ShowPS", "ps_5_0").Get(), kBackBufferFormat);

    UINT largestBlock = 0;
    for (const Block& block : g_blocks)
        largestBlock = std::max(largestBlock, block.count);
    g_chainLists.resize((largestBlock + kDrawsPerCommandList - 1) / kDrawsPerCommandList); // enough for any block
    for (CommandList& list : g_chainLists)
        list = CreateCommandList(device);
    g_showList = CreateCommandList(device);
}

void CreateTimestampQueries()
{
    const UINT queryCount = 2 * RtCount();
    D3D12_QUERY_HEAP_DESC desc{ D3D12_QUERY_HEAP_TYPE_TIMESTAMP, queryCount, 0 };
    CHECK(g_gpu.device->CreateQueryHeap(&desc, IID_PPV_ARGS(&g_timestamps)));
    g_timestampReadback = CreateReadbackBuffer(g_gpu.device.Get(), UINT64(queryCount) * sizeof(UINT64));

    UINT64 ticksPerSecond = 0;
    CHECK(g_gpu.queue->GetTimestampFrequency(&ticksPerSecond));
    g_msPerTick = 1000.0 / double(ticksPerSecond);
    g_drawMs.resize(RtCount());
}

// ---- Frame ----

// Sets the state shared by all fullscreen passes into a size x size render target.
void BeginPass(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* pipeline, UINT size)
{
    ID3D12DescriptorHeap* heaps[] = { g_srvs.heap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetGraphicsRootSignature(g_rootSignature.Get());
    cl->SetPipelineState(pipeline);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT viewport{ 0, 0, (float)size, (float)size, 0, 1 };
    const D3D12_RECT scissor{ 0, 0, (LONG)size, (LONG)size };
    cl->RSSetViewports(1, &viewport);
    cl->RSSetScissorRects(1, &scissor);
}

// One link of the chain: RT[i] = noise(seed, RT[i-1]), bracketed by timestamps. The timestamps enclose
// the barriers too, so the measured time covers the complete read of RT[i-1] and write of RT[i].
// The first RT of the active block has no predecessor and reads the null SRV.
void DrawChainLink(ID3D12GraphicsCommandList* cl, UINT i, UINT seed)
{
    ID3D12Resource* rt = g_rts[i].Get();
    const UINT constants[2] = { seed, i };
    cl->SetGraphicsRoot32BitConstants(0, 2, constants, 0);
    cl->SetGraphicsRootDescriptorTable(1, g_srvs.Gpu(i > Active().first ? i - 1 : RtCount()));

    cl->EndQuery(g_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * i);
    const D3D12_RESOURCE_BARRIER toTarget = Transition(rt, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl->ResourceBarrier(1, &toTarget);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvs.Cpu(i);
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cl->DrawInstanced(3, 1, 0, 0);
    const D3D12_RESOURCE_BARRIER toShaderResource = Transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cl->ResourceBarrier(1, &toShaderResource);
    cl->EndQuery(g_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * i + 1);
}

// Draws every render target of the active block once, in order: RT[first] = noise(seed),
// RT[i] = noise(seed, RT[i-1]). Because each RT reads its predecessor, every active RT is read and
// written every frame, so all of them are in use all the time and none can be left idle in system
// memory without it showing in the timing.
void RenderChain()
{
    const UINT begin = Active().first, end = begin + Active().allocated;
    const UINT seed = g_random();
    std::vector<ID3D12CommandList*> lists;
    for (UINT first = begin; first < end; first += kDrawsPerCommandList)
    {
        ID3D12GraphicsCommandList* cl = BeginCommandList(g_chainLists[(first - begin) / kDrawsPerCommandList]);
        BeginPass(cl, g_chainPipeline.Get(), kRtSize);
        const UINT last = std::min(end, first + kDrawsPerCommandList);
        for (UINT i = first; i < last; ++i)
            DrawChainLink(cl, i, seed);
        if (last == end)
            cl->ResolveQueryData(g_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * begin, 2 * (end - begin),
                g_timestampReadback.Get(), 2 * begin * sizeof(UINT64));
        CHECK(cl->Close());
        lists.push_back(cl);
    }
    g_gpu.queue->ExecuteCommandLists((UINT)lists.size(), lists.data());
}

// Draws the last render target of the active block into the window, so you can see the test is alive.
void ShowLastRT()
{
    ID3D12Resource* backBuffer = g_gpu.backBuffers[g_gpu.swapChain->GetCurrentBackBufferIndex()].Get();
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_gpu.backBufferRtvs.Cpu(g_gpu.swapChain->GetCurrentBackBufferIndex());

    ID3D12GraphicsCommandList* cl = BeginCommandList(g_showList);
    BeginPass(cl, g_showPipeline.Get(), kWindowSize);
    const UINT unusedConstants[2] = {};
    cl->SetGraphicsRoot32BitConstants(0, 2, unusedConstants, 0);
    cl->SetGraphicsRootDescriptorTable(1, g_srvs.Gpu(ActiveLast()));
    const D3D12_RESOURCE_BARRIER toTarget = Transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl->ResourceBarrier(1, &toTarget);
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cl->DrawInstanced(3, 1, 0, 0);
    const D3D12_RESOURCE_BARRIER toPresent = Transition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    cl->ResourceBarrier(1, &toPresent);
    CHECK(cl->Close());

    ID3D12CommandList* lists[] = { cl };
    g_gpu.queue->ExecuteCommandLists(1, lists);
    CHECK(g_gpu.swapChain->Present(0, 0));
}

// Waits until the GPU has finished the frame, then records every active RT's draw time. Waiting for the
// whole frame is the simplest correct way to read the timestamps; throughput does not matter for this test.
void ReadTimings()
{
    WaitForGpu(g_gpu);

    const UINT first = Active().first, last = ActiveLast();
    const D3D12_RANGE readRange{ SIZE_T(2 * first) * sizeof(UINT64), SIZE_T(2 * last + 2) * sizeof(UINT64) };
    UINT64* ticks = nullptr;
    CHECK(g_timestampReadback->Map(0, &readRange, (void**)&ticks));
    for (UINT i = first; i <= last; ++i)
        g_drawMs[i].push_back(double(ticks[2 * i + 1] - ticks[2 * i]) * g_msPerTick);
    g_frameMs.push_back(double(ticks[2 * last + 1] - ticks[2 * first]) * g_msPerTick);
    const D3D12_RANGE nothingWritten{ 0, 0 };
    g_timestampReadback->Unmap(0, &nothingWritten);
}

// ---- Report ----

double Median(std::vector<double> values)
{
    if (values.empty())
        return 0;
    std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
    return values[values.size() / 2];
}

// Classifies every render target of the active block from its draw times during the last second.
// Returns how many are slow. Only the active block is drawn, so only it is measured.
//  - Per RT we take the MEDIAN draw time, so a single slow frame (e.g. while the OS is paging) does not
//    change the result.
//  - The fast reference is the 10th percentile of those medians: the draw time of an RT in video memory,
//    as long as at least 10% of the RTs are in video memory (true unless the allocation is many times
//    the size of the GPU's memory).
//  - An RT is slow if its median is more than kSlowFactor (4x) the fast reference. On an RTX 2060 an RT
//    in video memory takes ~0.05 ms and one in system memory ~1.3 ms, so the exact factor hardly matters.
UINT Classify()
{
    const UINT first = Active().first, n = Active().allocated;
    std::vector<double> medians(n);
    for (UINT i = 0; i < n; ++i)
        medians[i] = Median(g_drawMs[first + i]);

    std::vector<double> sorted = medians;
    std::sort(sorted.begin(), sorted.end());
    const double fastReference = sorted[n / 10];

    return (UINT)std::count_if(medians.begin(), medians.end(),
        [&](double median) { return median > kSlowFactor * fastReference; });
}

void ClearSamples()
{
    for (std::vector<double>& times : g_drawMs)
        times.clear();
    g_frameMs.clear();
}

void PrintReport(ULONGLONG seconds)
{
    const UINT slowCount = Classify();
    const UINT fastCount = Active().allocated - slowCount;

    // Process-wide: includes the idle blocks.
    char evicted[32] = "n/a";
    uint64_t demotedBytes = 0;
    if (QueryKernelDemotedBytes(g_gpu.adapterDesc.AdapterLuid, &demotedBytes))
        sprintf_s(evicted, "%lluMB", (unsigned long long)(demotedBytes >> 20));

    // Worst case (5-digit seconds, RT indices and MB, 6-digit alloc MB, 4-digit frame ms) is ~150 chars.
    printf("t=%llus  block %c RT_%u..%u | alloc %u MB | vram(fast) %uMB/%u | sys(slow) %uMB/%u | evicted(kernel) %s | frame %.1f ms\n",
        seconds, Active().name, Active().first, ActiveLast(), AllocatedMegabytes(), fastCount * kRtMegabytes, fastCount,
        slowCount * kRtMegabytes, slowCount, evicted, Median(g_frameMs));
    ClearSamples();
}

// ---- Block transitions ----

// Makes the next block active (after the last one: block A with -flip, otherwise nothing changes).
// Frees the block that was active if it is dynamic, then allocates the new one if it is dynamic.
// Drops the timing samples, so none of the previous block end up in the next report.
void NextBlock(ULONGLONG seconds)
{
    UINT next = g_active + 1;
    if (next == g_blocks.size())
    {
        if (!g_flip)
            return;
        next = 0;
    }

    char freed[64] = "";
    if (Active().dynamic)
    {
        sprintf_s(freed, "freed block %c RT_%u..%u (%u MB) ", Active().name, Active().first, ActiveLast(),
            Active().allocated * kRtMegabytes);
        FreeBlock(Active());
    }
    g_active = next;
    if (Active().dynamic)
        CreateBlock(Active());
    ClearSamples();
    printf("t=%llus  %s-> block %c RT_%u..%u (%u MB%s)\n", seconds, freed, Active().name, Active().first, ActiveLast(),
        Active().allocated * kRtMegabytes, Active().dynamic ? ", allocated now" : "");
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0); // every line appears immediately, also when redirected to a file
    if (!ParseArguments(argc, argv))
    {
        PrintUsage();
        return 1;
    }

    // Give a separately started ETW monitor (e.g. dxtcl_monitor) time to start its session before the device and resources exist.
    printf("waiting 2 s so an ETW monitor can attach...\n");
    Sleep(2000);

    std::wstring title = L"vramtiming";
    for (int i = 1; i < argc; ++i)
        title += std::wstring(L" ") + argv[i];
    HWND window = CreateAppWindow(title.c_str(), kWindowSize, kWindowSize);
    CreateGpu(g_gpu, window, kWindowSize, kWindowSize);
    printf("vramtiming pid %lu | adapter %ls, %llu MB dedicated video memory\n", GetCurrentProcessId(),
        g_gpu.adapterDesc.Description, (unsigned long long)(g_gpu.adapterDesc.DedicatedVideoMemory >> 20));

    printf("blocks:");
    for (const Block& block : g_blocks)
        printf("%s %c %u MB%s", &block == &g_blocks[0] ? "" : ",", block.name, block.count * kRtMegabytes,
            block.dynamic ? " dynamic" : "");
    printf(" | %llu s each | %s\n", g_blockSeconds, g_flip ? "-flip: loop" : "then stays on the last block");

    // Allocate the persistent blocks, in order, and block A, which is active first, even if it is dynamic.
    g_rts.resize(RtCount());
    CreateDescriptorHeaps();
    for (Block& block : g_blocks)
        if (!block.dynamic || &block == &Active())
            CreateBlock(block);
    CreatePipelines();
    CreateTimestampQueries();
    printf("allocated %u MB at startup, rendering block A; close the window to exit\n", AllocatedMegabytes());

    const ULONGLONG start = GetTickCount64();
    ULONGLONG lastReport = 0;
    ULONGLONG nextBlockAt = g_blockSeconds;
    while (PumpMessages())
    {
        RenderChain();
        ShowLastRT();
        ReadTimings();

        const ULONGLONG seconds = (GetTickCount64() - start) / 1000;
        if (seconds > lastReport)
        {
            lastReport = seconds;
            PrintReport(seconds);
            if (seconds >= nextBlockAt)
            {
                NextBlock(seconds);
                nextBlockAt = seconds + g_blockSeconds;
            }
        }
    }

    WaitForGpu(g_gpu);
    printf("window closed, exiting\n");
    return 0;
}
