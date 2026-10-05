#include "d3d_helpers.h"

#include <d3dcompiler.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

void CheckHr(HRESULT hr, const char* what)
{
    if (SUCCEEDED(hr))
        return;
    printf("FATAL: %s failed, hr=0x%08X\n", what, (unsigned)hr);
    fflush(stdout);
    ExitProcess(2);
}

// ---- Window ----

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_DESTROY)
    {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND CreateAppWindow(const wchar_t* title, UINT width, UINT height)
{
    // Per-monitor DPI awareness, so "width x height" means physical pixels at any display scaling.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"vramtiming";
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX; // fixed size
    RECT rect{ 0, 0, (LONG)width, (LONG)height };
    AdjustWindowRectExForDpi(&rect, style, FALSE, 0, GetDpiForSystem());
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title, style, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd)
        CheckHr(HRESULT_FROM_WIN32(GetLastError()), "CreateWindowExW");
    ShowWindow(hwnd, SW_SHOW);
    return hwnd;
}

bool PumpMessages()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
            return false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

// ---- Device, queue, swap chain ----

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::Cpu(UINT index) const
{
    return { heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(index) * stride };
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeap::Gpu(UINT index) const
{
    return { heap->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(index) * stride };
}

DescriptorHeap CreateDescriptorHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, bool shaderVisible)
{
    DescriptorHeap result;
    D3D12_DESCRIPTOR_HEAP_DESC desc{ type, count,
        shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    CHECK(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&result.heap)));
    result.stride = device->GetDescriptorHandleIncrementSize(type);
    return result;
}

void CreateGpu(Gpu& gpu, HWND window, UINT width, UINT height)
{
#ifdef _DEBUG
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        debug->EnableDebugLayer();
#endif

    ComPtr<IDXGIFactory6> factory;
    CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        adapter->GetDesc1(&gpu.adapterDesc);
        if (!(gpu.adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            break;
        adapter.Reset();
    }
    if (!adapter)
        CheckHr(E_FAIL, "finding a hardware adapter");

    CHECK(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&gpu.device)));

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    CHECK(gpu.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&gpu.queue)));

    CHECK(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence)));
    gpu.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.Width = width;
    scDesc.Height = height;
    scDesc.Format = kBackBufferFormat;
    scDesc.SampleDesc.Count = 1;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = kBackBufferCount;
    scDesc.Scaling = DXGI_SCALING_STRETCH;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swapChain1;
    CHECK(factory->CreateSwapChainForHwnd(gpu.queue.Get(), window, &scDesc, nullptr, nullptr, &swapChain1));
    CHECK(factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER));
    CHECK(swapChain1.As(&gpu.swapChain));

    gpu.backBufferRtvs = CreateDescriptorHeap(gpu.device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kBackBufferCount, false);
    for (UINT i = 0; i < kBackBufferCount; ++i)
    {
        CHECK(gpu.swapChain->GetBuffer(i, IID_PPV_ARGS(&gpu.backBuffers[i])));
        gpu.device->CreateRenderTargetView(gpu.backBuffers[i].Get(), nullptr, gpu.backBufferRtvs.Cpu(i));
    }
}

void WaitForGpu(Gpu& gpu)
{
    const UINT64 value = ++gpu.fenceValue;
    CHECK(gpu.queue->Signal(gpu.fence.Get(), value));
    CHECK(gpu.fence->SetEventOnCompletion(value, gpu.fenceEvent));
    // Wake up regularly so a removed device ends the program instead of hanging it.
    while (WaitForSingleObject(gpu.fenceEvent, 1000) == WAIT_TIMEOUT)
        CHECK(gpu.device->GetDeviceRemovedReason());
}

// ---- Command lists ----

CommandList CreateCommandList(ID3D12Device* device)
{
    CommandList result;
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&result.allocator)));
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, result.allocator.Get(), nullptr, IID_PPV_ARGS(&result.list)));
    CHECK(result.list->Close());
    return result;
}

ID3D12GraphicsCommandList* BeginCommandList(CommandList& commandList)
{
    CHECK(commandList.allocator->Reset());
    CHECK(commandList.list->Reset(commandList.allocator.Get(), nullptr));
    return commandList.list.Get();
}

// ---- Resources ----

ComPtr<ID3D12Resource> CreateReadbackBuffer(ID3D12Device* device, UINT64 bytes)
{
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&buffer)));
    return buffer;
}

void CreateNullTextureSrv(ID3D12Device* device, DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE descriptor)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = format;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(nullptr, &desc, descriptor);
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

// ---- Shaders and pipelines ----

ComPtr<ID3DBlob> CompileShader(const char* source, const char* entryPoint, const char* target)
{
    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompile(source, strlen(source), "vramtiming.hlsl", nullptr, nullptr, entryPoint, target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr) && errors)
        printf("Shader compile error (%s): %s\n", entryPoint, (const char*)errors->GetBufferPointer());
    CheckHr(hr, "D3DCompile");
    return code;
}

ComPtr<ID3D12RootSignature> CreateRootSignature(ID3D12Device* device, UINT rootConstantCount)
{
    D3D12_DESCRIPTOR_RANGE srvRange{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = { 0, 0, rootConstantCount };
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = { 1, &srvRange };
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{ 2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ComPtr<ID3DBlob> blob, errors;
    CHECK(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));
    ComPtr<ID3D12RootSignature> rootSignature;
    CHECK(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSignature)));
    return rootSignature;
}

ComPtr<ID3D12PipelineState> CreateFullscreenPipeline(ID3D12Device* device, ID3D12RootSignature* rootSignature,
    ID3DBlob* vertexShader, ID3DBlob* pixelShader, DXGI_FORMAT renderTargetFormat)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rootSignature;
    desc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
    desc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = renderTargetFormat;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipeline;
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)));
    return pipeline;
}
