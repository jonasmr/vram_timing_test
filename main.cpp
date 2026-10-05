// vramtiming: ground-truth test for GPU memory demotion.
//
// Usage: vramtiming <megabytes>
//
// Allocates <megabytes>/16 render targets of 16 MiB each and renders ALL of them every frame. When the
// total exceeds what the OS lets this process keep in video memory, the graphics kernel demotes some of
// them to system memory, and the GPU then has to read and write those over PCIe, which is ~25x slower.
// Timing each render target's draw with GPU timestamps therefore shows which render targets live in
// video memory (fast) and which in system memory (slow). Once per second this is printed next to the
// kernel's own count of demoted bytes, so the two can be compared.
//
// "MB" in the output means MiB (1024 * 1024 bytes).

#include "d3d_helpers.h"
#include "kernelstats.h"
#include "shaders.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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

// ---- Test state ----

Gpu g_gpu;
std::vector<ComPtr<ID3D12Resource>> g_rts; // the render targets under test, named RT_0 .. RT_<N-1>
DescriptorHeap g_rtvs;                     // RTV i = RT i
DescriptorHeap g_srvs;                     // SRV i = RT i, SRV N = null (RT 0 has no predecessor)
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
std::vector<bool> g_previousSlow;          // classification printed in the previous report

UINT RtCount() { return (UINT)g_rts.size(); }

// ---- Setup ----

// Creates up to `count` committed 16 MiB render targets in the DEFAULT heap, named RT_<index>.
// If the device runs out of memory, continues with the ones created so far.
void CreateRenderTargets(UINT count)
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

    for (UINT i = 0; i < count; ++i)
    {
        ComPtr<ID3D12Resource> rt;
        HRESULT hr = g_gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&rt));
        if (FAILED(hr))
        {
            printf("CreateCommittedResource failed (hr=0x%08X) after %u of %u render targets, continuing with %u\n",
                (unsigned)hr, i, count, i);
            break;
        }
        wchar_t name[32];
        swprintf_s(name, L"RT_%u", i);
        rt->SetName(name);
        g_rts.push_back(rt);
    }
    if (g_rts.empty())
        CheckHr(E_OUTOFMEMORY, "creating any render target");

    const UINT n = RtCount();
    g_rtvs = CreateDescriptorHeap(g_gpu.device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, n, false);
    g_srvs = CreateDescriptorHeap(g_gpu.device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, n + 1, true);
    for (UINT i = 0; i < n; ++i)
    {
        g_gpu.device->CreateRenderTargetView(g_rts[i].Get(), nullptr, g_rtvs.Cpu(i));
        g_gpu.device->CreateShaderResourceView(g_rts[i].Get(), nullptr, g_srvs.Cpu(i));
    }
    CreateNullTextureSrv(g_gpu.device.Get(), kRtFormat, g_srvs.Cpu(n));
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

    g_chainLists.resize((RtCount() + kDrawsPerCommandList - 1) / kDrawsPerCommandList);
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
void DrawChainLink(ID3D12GraphicsCommandList* cl, UINT i, UINT seed)
{
    ID3D12Resource* rt = g_rts[i].Get();
    const UINT constants[2] = { seed, i };
    cl->SetGraphicsRoot32BitConstants(0, 2, constants, 0);
    cl->SetGraphicsRootDescriptorTable(1, g_srvs.Gpu(i > 0 ? i - 1 : RtCount()));

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

// Draws every render target once, in order: RT[0] = noise(seed), RT[i] = noise(seed, RT[i-1]).
// Because each RT reads its predecessor, every RT is read and written every frame, so all of them
// are in use all the time and none can be left idle in system memory without it showing in the timing.
void RenderChain()
{
    const UINT n = RtCount();
    const UINT seed = g_random();
    std::vector<ID3D12CommandList*> lists;
    for (UINT first = 0; first < n; first += kDrawsPerCommandList)
    {
        ID3D12GraphicsCommandList* cl = BeginCommandList(g_chainLists[first / kDrawsPerCommandList]);
        BeginPass(cl, g_chainPipeline.Get(), kRtSize);
        const UINT end = std::min(n, first + kDrawsPerCommandList);
        for (UINT i = first; i < end; ++i)
            DrawChainLink(cl, i, seed);
        if (end == n)
            cl->ResolveQueryData(g_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2 * n, g_timestampReadback.Get(), 0);
        CHECK(cl->Close());
        lists.push_back(cl);
    }
    g_gpu.queue->ExecuteCommandLists((UINT)lists.size(), lists.data());
}

// Draws the last render target of the chain into the window, so you can see the test is alive.
void ShowLastRT()
{
    ID3D12Resource* backBuffer = g_gpu.backBuffers[g_gpu.swapChain->GetCurrentBackBufferIndex()].Get();
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_gpu.backBufferRtvs.Cpu(g_gpu.swapChain->GetCurrentBackBufferIndex());

    ID3D12GraphicsCommandList* cl = BeginCommandList(g_showList);
    BeginPass(cl, g_showPipeline.Get(), kWindowSize);
    const UINT unusedConstants[2] = {};
    cl->SetGraphicsRoot32BitConstants(0, 2, unusedConstants, 0);
    cl->SetGraphicsRootDescriptorTable(1, g_srvs.Gpu(RtCount() - 1));
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

// Waits until the GPU has finished the frame, then records every RT's draw time. Waiting for the whole
// frame is the simplest correct way to read the timestamps; throughput does not matter for this test.
void ReadTimings()
{
    WaitForGpu(g_gpu);

    const UINT n = RtCount();
    const D3D12_RANGE readRange{ 0, SIZE_T(2 * n) * sizeof(UINT64) };
    UINT64* ticks = nullptr;
    CHECK(g_timestampReadback->Map(0, &readRange, (void**)&ticks));
    for (UINT i = 0; i < n; ++i)
        g_drawMs[i].push_back(double(ticks[2 * i + 1] - ticks[2 * i]) * g_msPerTick);
    g_frameMs.push_back(double(ticks[2 * n - 1] - ticks[0]) * g_msPerTick);
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

// Classifies every render target from its draw times during the last second. true = slow.
//  - Per RT we take the MEDIAN draw time, so a single slow frame (e.g. while the OS is paging) does not
//    change the result.
//  - The fast reference is the 10th percentile of those medians: the draw time of an RT in video memory,
//    as long as at least 10% of the RTs are in video memory (true unless the allocation is many times
//    the size of the GPU's memory).
//  - An RT is slow if its median is more than kSlowFactor (4x) the fast reference. On an RTX 2060 an RT
//    in video memory takes ~0.05 ms and one in system memory ~1.3 ms, so the exact factor hardly matters.
std::vector<bool> Classify()
{
    const UINT n = RtCount();
    std::vector<double> medians(n);
    for (UINT i = 0; i < n; ++i)
        medians[i] = Median(g_drawMs[i]);

    std::vector<double> sorted = medians;
    std::sort(sorted.begin(), sorted.end());
    const double fastReference = sorted[n / 10];

    std::vector<bool> slow(n);
    for (UINT i = 0; i < n; ++i)
        slow[i] = medians[i] > kSlowFactor * fastReference;
    return slow;
}

// Indices where slow[i] is true, as ranges: "0-12, 40-283". "none" if there are none.
std::string FormatRanges(const std::vector<bool>& slow)
{
    std::string text;
    for (size_t i = 0; i < slow.size(); ++i)
    {
        if (!slow[i])
            continue;
        size_t last = i;
        while (last + 1 < slow.size() && slow[last + 1])
            ++last;
        text += text.empty() ? "" : ", ";
        text += last == i ? std::to_string(i) : std::to_string(i) + "-" + std::to_string(last);
        i = last;
    }
    return text.empty() ? "none" : text;
}

void PrintReport(ULONGLONG seconds)
{
    const std::vector<bool> slow = Classify();
    const UINT n = RtCount();
    const UINT slowCount = (UINT)std::count(slow.begin(), slow.end(), true);
    const UINT fastCount = n - slowCount;

    char evicted[32] = "n/a";
    uint64_t demotedBytes = 0;
    if (QueryKernelDemotedBytes(g_gpu.adapterDesc.AdapterLuid, &demotedBytes))
        sprintf_s(evicted, "%llu MB", (unsigned long long)(demotedBytes >> 20));

    printf("t=%llus  allocated %u MB | VRAM (fast) %u MB [%u RTs] | system memory (slow) %u MB [%u RTs] | evicted (kernel) %s | frame %.1f ms\n",
        seconds, n * kRtMegabytes, fastCount * kRtMegabytes, fastCount, slowCount * kRtMegabytes, slowCount, evicted,
        Median(g_frameMs));
    if (slow != g_previousSlow)
        printf("  slow: %s\n", FormatRanges(slow).c_str());

    g_previousSlow = slow;
    for (std::vector<double>& times : g_drawMs)
        times.clear();
    g_frameMs.clear();
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    wchar_t* end = nullptr;
    const unsigned long megabytes = argc == 2 ? wcstoul(argv[1], &end, 10) : 0;
    if (megabytes < kRtMegabytes || *end != L'\0')
    {
        printf("Usage: vramtiming <megabytes>\n"
               "  Allocates <megabytes>/16 render targets of 16 MiB, renders all of them every frame and\n"
               "  prints once per second how many are in video memory (fast) vs system memory (slow).\n"
               "  Example: vramtiming 8192\n");
        return 1;
    }
    setvbuf(stdout, nullptr, _IONBF, 0); // every line appears immediately, also when redirected to a file

    wchar_t title[64];
    swprintf_s(title, L"vramtiming %lu MB", megabytes);
    HWND window = CreateAppWindow(title, kWindowSize, kWindowSize);
    CreateGpu(g_gpu, window, kWindowSize, kWindowSize);
    printf("vramtiming pid %lu | adapter %ls, %llu MB dedicated video memory\n", GetCurrentProcessId(),
        g_gpu.adapterDesc.Description, (unsigned long long)(g_gpu.adapterDesc.DedicatedVideoMemory >> 20));

    CreateRenderTargets(megabytes / kRtMegabytes);
    CreatePipelines();
    CreateTimestampQueries();
    printf("rendering %u render targets (RT_0 .. RT_%u, %u MB) every frame; close the window to exit\n",
        RtCount(), RtCount() - 1, RtCount() * kRtMegabytes);

    const ULONGLONG start = GetTickCount64();
    ULONGLONG lastReport = 0;
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
        }
    }

    WaitForGpu(g_gpu);
    printf("window closed, exiting\n");
    return 0;
}
