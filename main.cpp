// vramtiming: ground-truth test for GPU memory demotion.
//
// Usage: vramtiming <seconds> <size>[d] [<size>[d] ...] [-flip] [-fullscreen]
//        vramtiming memorylimittest
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
// over at A, until the window is closed (or Esc is pressed).
//
// "memorylimittest" instead finds how much render target memory the process can use before render targets
// stay in system memory: block A starts empty and grows every second until they do (see RunMemoryLimitTest()).
//
// It is a Windows (GUI) program, not a console program, so that Windows treats it like a game. All output
// goes to vramtiming.log in the current directory and, unless -fullscreen, to a console window it opens.
// With -fullscreen the window is borderless and covers the monitor of the adapter's first output.
//
// "MB" in the output means MiB (1024 * 1024 bytes).

#include "d3d_helpers.h"
#include "kernelstats.h"
#include "shaders.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
// indices, in command line order: A = A_0 .. A_<a-1>, B = B_<a> .., and so on. The render targets are
// named <block letter>_<index>. A dynamic block uses the same range (so the same names, descriptors
// and queries) every time it is allocated.
struct Block
{
    char name;      // 'A', 'B', ...
    UINT megabytes; // size from the command line, before rounding up
    UINT first;     // its render targets are <name>_<first> .. <name>_<first + count - 1>
    UINT count;     // render targets it should have: its size rounded up to whole 16 MiB
    bool dynamic;   // allocated only while it is the active block
    UINT allocated; // render targets that exist right now: 0 while freed, count, or fewer if out of memory
};

Gpu g_gpu;
std::vector<Block> g_blocks;
UINT g_active = 0;            // index of the block that is rendered and measured
ULONGLONG g_blockSeconds = 0; // how long each block is active
bool g_flip = false;          // after the last block, start over at A (otherwise stay on the last block)
bool g_fullscreen = false;    // borderless window over the whole monitor, log only to the file
bool g_memoryLimitTest = false; // "memorylimittest" instead of a block list, see RunMemoryLimitTest()

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

const char* kUsage =
    "Usage: vramtiming <seconds> <size>[d] [<size>[d] ...] [-flip] [-fullscreen]\n"
    "       vramtiming memorylimittest\n"
    "  Each <size> (MB) defines a block of 16 MiB render targets, named A, B, C, ... in order.\n"
    "  The blocks are rendered in order, <seconds> each: every frame draws all render targets of the\n"
    "  active block, and once per second it logs how many are in video memory (fast) and how many\n"
    "  in system memory (slow). Blocks are allocated at startup and kept, except a block whose size\n"
    "  ends in d (dynamic): it is allocated when it becomes active and freed when the next one does.\n"
    "  After the last block it stays there; with -flip it starts over at A.\n"
    "  -fullscreen: borderless window covering the whole monitor (otherwise a 1024x1024 window).\n"
    "  The log goes to vramtiming.log in the current directory, and without -fullscreen also to a\n"
    "  console window. Close the window or press Esc to exit.\n"
    "  memorylimittest: block A starts empty and grows by 128 MB per second until render targets stay\n"
    "  in system memory, then logs how much fit in video memory and shows it in a message box.\n"
    "  Always fullscreen. No other arguments.\n"
    "  Examples:\n"
    "    vramtiming 15 512 512 512    A, B, C (all allocated at startup) 15 s each, then stays on C\n"
    "    vramtiming 20 32 128d -flip  A; at 20 s allocate B and render it; at 40 s free B, back to A; loop\n"
    "    vramtiming 10 8192           one block of 8192 MB\n"
    "    vramtiming 10 3072d 3072     A and B exist at startup; at 10 s free A and render B forever";

// Fills g_blockSeconds, g_blocks, g_flip and g_fullscreen from the command line, or sets g_memoryLimitTest
// (and g_fullscreen) for "memorylimittest". Returns false if it is invalid.
bool ParseArguments(int argc, wchar_t** argv)
{
    if (argc >= 2 && wcscmp(argv[1], L"memorylimittest") == 0)
    {
        g_memoryLimitTest = true;
        g_fullscreen = true; // always, so it runs like a game
        return argc == 2;
    }

    std::vector<const wchar_t*> numbers; // everything except the flags: <seconds>, then the sizes
    for (int i = 1; i < argc; ++i)
    {
        if (wcscmp(argv[i], L"-flip") == 0 || wcscmp(argv[i], L"--flip") == 0)
            g_flip = true;
        else if (wcscmp(argv[i], L"-fullscreen") == 0 || wcscmp(argv[i], L"--fullscreen") == 0)
            g_fullscreen = true;
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
        g_blocks.push_back({ char('A' + i - 1), UINT(megabytes), first, count, dynamic, 0 });
        first += count;
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

// Creates render target <index> of the block: a committed 16 MiB render target in the DEFAULT heap, named
// <block letter>_<index>, with its RTV and SRV. Returns the HRESULT of CreateCommittedResource.
HRESULT CreateRenderTarget(const Block& block, UINT index)
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

    HRESULT hr = g_gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&g_rts[index]));
    if (FAILED(hr))
        return hr;
    wchar_t name[32];
    swprintf_s(name, L"%c_%u", wchar_t(block.name), index);
    g_rts[index]->SetName(name);
    g_gpu.device->CreateRenderTargetView(g_rts[index].Get(), nullptr, g_rtvs.Cpu(index));
    g_gpu.device->CreateShaderResourceView(g_rts[index].Get(), nullptr, g_srvs.Cpu(index));
    return hr;
}

// Creates all render targets of the block. If the device runs out of memory, it stops there and says so
// (and exits if it could not create a single one).
void CreateBlock(Block& block)
{
    for (block.allocated = 0; block.allocated < block.count; ++block.allocated)
    {
        HRESULT hr = CreateRenderTarget(block, block.first + block.allocated);
        if (FAILED(hr))
        {
            Log("CreateCommittedResource failed (hr=0x%08X) after %u of %u render targets of block %c, "
                "continuing with %u", (unsigned)hr, block.allocated, block.count, block.name, block.allocated);
            if (block.allocated == 0)
                CheckHr(hr, "creating any render target of the block");
            return;
        }
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

// Sets the state shared by all fullscreen passes into a width x height render target.
void BeginPass(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* pipeline, UINT width, UINT height)
{
    ID3D12DescriptorHeap* heaps[] = { g_srvs.heap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetGraphicsRootSignature(g_rootSignature.Get());
    cl->SetPipelineState(pipeline);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT viewport{ 0, 0, (float)width, (float)height, 0, 1 };
    const D3D12_RECT scissor{ 0, 0, (LONG)width, (LONG)height };
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
        BeginPass(cl, g_chainPipeline.Get(), kRtSize, kRtSize);
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

// Draws the last render target of the active block into the window, stretched to the whole back buffer,
// so you can see the test is alive.
void ShowLastRT()
{
    ID3D12Resource* backBuffer = g_gpu.backBuffers[g_gpu.swapChain->GetCurrentBackBufferIndex()].Get();
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_gpu.backBufferRtvs.Cpu(g_gpu.swapChain->GetCurrentBackBufferIndex());

    ID3D12GraphicsCommandList* cl = BeginCommandList(g_showList);
    BeginPass(cl, g_showPipeline.Get(), g_gpu.width, g_gpu.height);
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

// What the report of one second found.
struct Measurement
{
    UINT slowCount;        // render targets of the active block whose draws were slow, i.e. in system memory
    uint64_t evictedBytes; // the kernel's demoted bytes of the whole process (0 if the query failed)
};

// Logs the report line for the last second, discards its timing samples and returns what it found.
Measurement PrintReport(ULONGLONG seconds)
{
    const UINT slowCount = Classify();
    const UINT fastCount = Active().allocated - slowCount;

    // Process-wide: includes the idle blocks.
    char evicted[32] = "n/a";
    uint64_t demotedBytes = 0;
    if (QueryKernelDemotedBytes(g_gpu.adapterDesc.AdapterLuid, &demotedBytes))
        sprintf_s(evicted, "%lluMB", (unsigned long long)(demotedBytes >> 20));

    // Worst case (5-digit seconds, RT indices and MB, 6-digit alloc MB, 4-digit frame ms) is ~150 chars.
    Log("t=%llus  block %c %c_%u..%u | alloc %u MB | vram(fast) %uMB/%u | sys(slow) %uMB/%u | evicted(kernel) %s | frame %.1f ms",
        seconds, Active().name, Active().name, Active().first, ActiveLast(), AllocatedMegabytes(), fastCount * kRtMegabytes, fastCount,
        slowCount * kRtMegabytes, slowCount, evicted, Median(g_frameMs));
    ClearSamples();
    return { slowCount, demotedBytes };
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
        sprintf_s(freed, "freed block %c %c_%u..%u (%u MB) ", Active().name, Active().name, Active().first, ActiveLast(),
            Active().allocated * kRtMegabytes);
        FreeBlock(Active());
    }
    g_active = next;
    if (Active().dynamic)
        CreateBlock(Active());
    ClearSamples();
    Log("t=%llus  %s-> block %c %c_%u..%u (%u MB%s)", seconds, freed, Active().name, Active().name, Active().first,
        ActiveLast(), Active().allocated * kRtMegabytes, Active().dynamic ? ", allocated now" : "");
}

// ---- memorylimittest ----
//
// Finds how much render target memory this process can use before render targets stay in system memory.
// Block A starts empty and grows by kStepMB every second, up to 2x the adapter's dedicated video memory.
// Every frame renders all of it, as the normal mode renders the active block. After each second's report
// it checks whether anything is in system memory: a render target classified slow by the timing, or the
// kernel's evicted (demoted) bytes > 0. If so, it stops growing and keeps rendering the same render
// targets for kPageInWaitSeconds. If by then nothing is in system memory any more, it was only paged out
// for a moment: it continues growing. Otherwise that is the limit.

constexpr UINT kStepMB = 128;               // growth per second: 8 render targets
constexpr ULONGLONG kPageInWaitSeconds = 5; // how long the kernel gets to page render targets back in

bool InSystemMemory(const Measurement& m) { return m.slowCount > 0 || m.evictedBytes > 0; }

// The signal(s) saying part of block A is in system memory, e.g. "slow 3 RTs / 48 MB, evicted(kernel) 32 MB".
// The evicted MB are rounded up, so that a few evicted bytes do not show as 0 MB.
std::string Signals(const Measurement& m)
{
    char text[128] = "";
    if (m.slowCount > 0)
        sprintf_s(text, "slow %u RTs / %u MB", m.slowCount, m.slowCount * kRtMegabytes);
    if (m.evictedBytes > 0)
        sprintf_s(text + strlen(text), sizeof(text) - strlen(text), "%sevicted(kernel) %llu MB", m.slowCount > 0 ? ", " : "",
            (unsigned long long)((m.evictedBytes + (1 << 20) - 1) >> 20));
    return text;
}

// Adds kStepMB of render targets to block A (fewer if that would pass its end).
// Returns S_OK, or the HRESULT of the CreateCommittedResource that failed (then block A keeps the ones before).
HRESULT GrowBlockA()
{
    Block& a = Active();
    const UINT target = std::min(a.count, a.allocated + kStepMB / kRtMegabytes);
    for (; a.allocated < target; ++a.allocated)
    {
        const HRESULT hr = CreateRenderTarget(a, a.first + a.allocated);
        if (FAILED(hr))
            return hr;
    }
    return S_OK;
}

// Logs the result with some context, releases all D3D12 objects once the GPU is idle, and shows the result
// in a message box (the fullscreen window hid everything until now).
void ReportMemoryLimit(HWND window, const std::string& result)
{
    DXGI_QUERY_VIDEO_MEMORY_INFO local{};
    ComPtr<IDXGIAdapter3> adapter3;
    if (SUCCEEDED(g_gpu.adapter.As(&adapter3)))
        adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);

    char adapter[256], memory[256];
    sprintf_s(adapter, "adapter %ls, %llu MB dedicated video memory", g_gpu.adapterDesc.Description,
        (unsigned long long)(g_gpu.adapterDesc.DedicatedVideoMemory >> 20));
    sprintf_s(memory, "DXGI local video memory now: budget %llu MB, current usage %llu MB",
        (unsigned long long)(local.Budget >> 20), (unsigned long long)(local.CurrentUsage >> 20));
    Log("---- result ----");
    Log("%s", result.c_str());
    Log("%s", adapter);
    Log("%s", memory);

    WaitForGpu(g_gpu);
    g_rts.clear();
    g_rtvs = {};
    g_srvs = {};
    g_rootSignature.Reset();
    g_chainPipeline.Reset();
    g_showPipeline.Reset();
    g_chainLists.clear();
    g_showList = {};
    g_timestamps.Reset();
    g_timestampReadback.Reset();
    g_gpu = {}; // swap chain, queue, device, ...
    ShowWindow(window, SW_HIDE);

    const std::string text = result + "\n\n" + adapter + "\n" + memory;
    MessageBoxA(nullptr, text.c_str(), "vramtiming memorylimittest", MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
    Log("exiting");
}

// Runs the memorylimittest in the (fullscreen) window, after the device and swap chain exist.
int RunMemoryLimitTest(HWND window)
{
    const UINT maxCount = UINT(2 * (g_gpu.adapterDesc.DedicatedVideoMemory >> 20) / kRtMegabytes);
    g_blocks.push_back({ 'A', maxCount * kRtMegabytes, 0, maxCount, false, 0 });
    g_rts.resize(RtCount());
    CreateDescriptorHeaps();
    CreatePipelines();
    CreateTimestampQueries();
    Log("memorylimittest: block A grows by %u MB per second (max %u MB, 2x dedicated) until render targets stay in "
        "system memory for %llu s; Esc stops", kStepMB, maxCount * kRtMegabytes, kPageInWaitSeconds);

    UINT fitMB = 0;     // the largest allocation so far with nothing in system memory
    std::string result; // set when the test is over
    char text[512];
    HRESULT hr = GrowBlockA();
    if (FAILED(hr))
    {
        sprintf_s(text, "memory limit not reached: CreateCommittedResource failed (hr=0x%08X) at %u MB allocated",
            (unsigned)hr, AllocatedMegabytes());
        result = text;
    }

    const ULONGLONG start = GetTickCount64();
    ULONGLONG lastReport = 0;
    ULONGLONG waitUntil = 0; // while waiting for render targets to be paged back in: when the wait ends; else 0
    while (result.empty() && PumpMessages())
    {
        RenderChain();
        ShowLastRT();
        ReadTimings();

        const ULONGLONG seconds = (GetTickCount64() - start) / 1000;
        if (seconds == lastReport)
            continue;
        lastReport = seconds;
        const Measurement m = PrintReport(seconds);

        if (waitUntil == 0 && InSystemMemory(m))
        {
            Log("t=%llus  textures in system memory at %u MB allocated (%s); rendering %llu s to see if they are paged back in",
                seconds, AllocatedMegabytes(), Signals(m).c_str(), kPageInWaitSeconds);
            waitUntil = seconds + kPageInWaitSeconds;
            ClearSamples();
            continue;
        }
        if (waitUntil != 0)
        {
            if (seconds < waitUntil)
                continue;
            if (InSystemMemory(m))
            {
                sprintf_s(text, "memory limit: %u MB stayed in video memory; at %u MB, textures stayed in system memory "
                    "after %llu s (%s)", fitMB, AllocatedMegabytes(), kPageInWaitSeconds, Signals(m).c_str());
                result = text;
                continue;
            }
            Log("t=%llus  paged back in, continuing", seconds);
            waitUntil = 0;
        }

        // Nothing is in system memory: all of block A fits. Grow it, unless it is already at its maximum.
        fitMB = AllocatedMegabytes();
        if (Active().allocated == Active().count)
        {
            sprintf_s(text, "no limit found up to %u MB (2x dedicated video memory): all of it stayed in video memory", fitMB);
            result = text;
        }
        else if (FAILED(hr = GrowBlockA()))
        {
            sprintf_s(text, "memory limit not reached: CreateCommittedResource failed (hr=0x%08X) at %u MB allocated; "
                "%u MB stayed in video memory", (unsigned)hr, AllocatedMegabytes(), fitMB);
            result = text;
        }
    }

    if (result.empty())
    {
        WaitForGpu(g_gpu);
        Log("window closed at %u MB allocated, before the limit was found; so far %u MB stayed in video memory; exiting",
            AllocatedMegabytes(), fitMB);
        return 0;
    }
    ReportMemoryLimit(window, result);
    return 0;
}

} // namespace

// A Windows (GUI) program: wWinMain instead of main, the command line comes from __argc / __wargv.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    const int argc = __argc;
    wchar_t** argv = __wargv;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // sizes in physical pixels at any display scaling

    if (!ParseArguments(argc, argv))
    {
        StartLog(false);
        Log("%s", kUsage);
        MessageBoxA(nullptr, (std::string("Invalid command line.\n\n") + kUsage).c_str(), "vramtiming", MB_OK | MB_ICONWARNING);
        return 1;
    }
    // Fullscreen covers the screen, so a console would only be in the way: then the log goes only to the file.
    StartLog(!g_fullscreen);
    for (const Block& block : g_blocks)
        if (block.count * kRtMegabytes != block.megabytes)
            Log("block %c: %u MB rounded up to %u MB (whole 16 MiB render targets)", block.name, block.megabytes,
                block.count * kRtMegabytes);

    // Give a separately started ETW monitor (e.g. dxtcl_monitor) time to start its session before the device and resources exist.
    Log("waiting 2 s so an ETW monitor can attach...");
    Sleep(2000);

    CreateGpu(g_gpu);
    std::wstring title = L"vramtiming";
    for (int i = 1; i < argc; ++i)
        title += std::wstring(L" ") + argv[i];
    HWND window = nullptr;
    if (g_fullscreen)
        window = CreateFullscreenWindow(title.c_str(), AdapterDesktopRect(g_gpu));
    else
        window = CreateAppWindow(title.c_str(), kWindowSize, kWindowSize);
    CreateSwapChain(g_gpu, window);
    Log("vramtiming pid %lu | adapter %ls, %llu MB dedicated video memory", GetCurrentProcessId(),
        g_gpu.adapterDesc.Description, (unsigned long long)(g_gpu.adapterDesc.DedicatedVideoMemory >> 20));
    if (g_memoryLimitTest)
        return RunMemoryLimitTest(window);

    std::string blocks = "blocks:";
    for (const Block& block : g_blocks)
    {
        char text[64];
        sprintf_s(text, "%s %c %u MB%s", &block == &g_blocks[0] ? "" : ",", block.name, block.count * kRtMegabytes,
            block.dynamic ? " dynamic" : "");
        blocks += text;
    }
    Log("%s | %llu s each | %s", blocks.c_str(), g_blockSeconds, g_flip ? "-flip: loop" : "then stays on the last block");

    // Allocate the persistent blocks, in order, and block A, which is active first, even if it is dynamic.
    g_rts.resize(RtCount());
    CreateDescriptorHeaps();
    for (Block& block : g_blocks)
        if (!block.dynamic || &block == &Active())
            CreateBlock(block);
    CreatePipelines();
    CreateTimestampQueries();
    Log("allocated %u MB at startup, rendering block A; close the window or press Esc to exit", AllocatedMegabytes());

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
    Log("window closed, exiting");
    return 0;
}
