// d3d_helpers: the D3D12 / Win32 boilerplate of vramtiming (window, device, swap chain, descriptor
// heaps, shaders, pipelines). Nothing in here is specific to the test; main.cpp holds the test itself.

#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// Prints the failing expression and HRESULT, then exits the process.
void CheckHr(HRESULT hr, const char* what);
#define CHECK(x) CheckHr((x), #x)

// ---- Window ----

// Creates a visible, non-resizable window whose client area is exactly width x height pixels.
HWND CreateAppWindow(const wchar_t* title, UINT width, UINT height);

// Handles pending window messages. Returns false once the window has been closed.
bool PumpMessages();

// ---- Device, queue, swap chain ----

struct DescriptorHeap
{
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT stride = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT index) const;
};

DescriptorHeap CreateDescriptorHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, bool shaderVisible);

constexpr UINT kBackBufferCount = 2;
constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

struct Gpu
{
    DXGI_ADAPTER_DESC1 adapterDesc{};
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<ID3D12Resource> backBuffers[kBackBufferCount];
    DescriptorHeap backBufferRtvs;
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue = 0;
    HANDLE fenceEvent = nullptr;
};

// Picks the high-performance hardware adapter and creates device, direct queue, fence and a
// flip-model swap chain of width x height for the window.
void CreateGpu(Gpu& gpu, HWND window, UINT width, UINT height);

// Signals the fence on the queue and blocks until the GPU has reached it (exits if the device is removed).
void WaitForGpu(Gpu& gpu);

// ---- Command lists ----

struct CommandList
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
};

CommandList CreateCommandList(ID3D12Device* device);

// Resets allocator and list for recording. Only call after the GPU has finished the previous use.
ID3D12GraphicsCommandList* BeginCommandList(CommandList& commandList);

// ---- Resources ----

ComPtr<ID3D12Resource> CreateReadbackBuffer(ID3D12Device* device, UINT64 bytes);

// Writes a null Texture2D SRV (reads return 0) to the given descriptor.
void CreateNullTextureSrv(ID3D12Device* device, DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

// ---- Shaders and pipelines ----

ComPtr<ID3DBlob> CompileShader(const char* source, const char* entryPoint, const char* target);

// Root signature used by every pass:
//   parameter 0: rootConstantCount 32-bit constants, visible as cbuffer b0
//   parameter 1: descriptor table with one SRV at t0 (pixel shader)
//   static sampler s0: bilinear, clamp
ComPtr<ID3D12RootSignature> CreateRootSignature(ID3D12Device* device, UINT rootConstantCount);

// Pipeline for a fullscreen triangle (3 vertices from SV_VertexID, no vertex buffer), no blending,
// no depth, one render target of the given format.
ComPtr<ID3D12PipelineState> CreateFullscreenPipeline(ID3D12Device* device, ID3D12RootSignature* rootSignature,
    ID3DBlob* vertexShader, ID3DBlob* pixelShader, DXGI_FORMAT renderTargetFormat);
