// d3d_helpers: the D3D12 / Win32 boilerplate of vramtiming (log, window, device, swap chain, descriptor
// heaps, shaders, pipelines). Nothing in here is specific to the test; main.cpp holds the test itself.

#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// ---- Log ----

// Opens the log file vramtiming.log in the current directory (overwritten) and logs its full path as the
// first line. With console = true it also opens a console window for the process and logs to it, too.
void StartLog(bool console);

// Logs one line (printf format, no trailing newline needed) to the log file, flushed right away, and to
// the console if there is one.
void Log(const char* format, ...);

// Logs "FATAL: <message>", shows the message in a message box, then exits the process with code 2.
[[noreturn]] void Fatal(const char* format, ...);

// If hr is a failure: Fatal() with the failing expression and the HRESULT.
void CheckHr(HRESULT hr, const char* what);
#define CHECK(x) CheckHr((x), #x)

// ---- Window ----

// Both window functions expect per-monitor DPI awareness, so sizes are physical pixels.

// Creates a visible, non-resizable window whose client area is exactly width x height pixels.
HWND CreateAppWindow(const wchar_t* title, UINT width, UINT height);

// Creates a visible borderless window (WS_POPUP) that covers exactly the given desktop rectangle.
HWND CreateFullscreenWindow(const wchar_t* title, const RECT& rect);

// Handles pending window messages. Returns false once the window has been closed: by Esc, Alt+F4, the
// close button or WM_CLOSE.
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
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 adapterDesc{};
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swapChain;
    UINT width = 0, height = 0; // back buffer size
    ComPtr<ID3D12Resource> backBuffers[kBackBufferCount];
    DescriptorHeap backBufferRtvs;
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue = 0;
    HANDLE fenceEvent = nullptr;
};

// Picks the high-performance hardware adapter and creates device, direct queue and fence.
void CreateGpu(Gpu& gpu);

// Desktop rectangle of the adapter's first output (monitor), in physical pixels. If the adapter has no
// outputs (e.g. the discrete GPU of a hybrid laptop), the primary monitor's. Logs which one and its size.
RECT AdapterDesktopRect(const Gpu& gpu);

// Creates a flip-model (flip discard) swap chain for the window, with back buffers the size of its
// client area. Never exclusive fullscreen.
void CreateSwapChain(Gpu& gpu, HWND window);

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
