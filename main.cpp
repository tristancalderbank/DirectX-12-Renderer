#define WIN32_LEAN_AND_MEAN // reduces size of headers by excluding some less frequently used APIs
#define NOMINMAX

#include <Windows.h>
#include <shellapi.h> // For CommandLineToArgvW

// Windows Runtime Library, needed for ComPtr<> 
// it allows smart pointers for COM objects
#include <wrl.h>

// DirectX 12 specific headers
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <dxcapi.h>

// DirectX 12 extension library
#include "d3dx12.h"

// NVidia DXR Helpers
#include "dxr/TopLevelASGenerator.h"

// STL
#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>
#include <filesystem>

// helpers
#include "helpers.h"
#include "raytracing.h"

using Microsoft::WRL::ComPtr;

// globals
const uint8_t gNumFrames = 3; // num swap chain back buffers
uint32_t gClientWidth = 1280;
uint32_t gClientHeight = 720;

bool gIsInitialized = false; // set to true once DX12 objects initialized

// Windows globals

HWND ghWnd;
RECT gWindowRect;

// DirectX 12 objects
ComPtr<ID3D12Device5> gDevice;
ComPtr<ID3D12CommandQueue> gCommandQueue;
ComPtr<IDXGISwapChain4> gSwapChain;
ComPtr<ID3D12Resource> gBackBuffers[gNumFrames];
ComPtr<ID3D12GraphicsCommandList4> gCommandList;
ComPtr<ID3D12CommandAllocator> gCommandAllocators[gNumFrames];
ComPtr<ID3D12DescriptorHeap> gRTVDescriptorHeap; // render target view
ComPtr<ID3D12RootSignature> gRootSignature;
ComPtr<ID3D12PipelineState> gPipelineState;
CD3DX12_VIEWPORT gViewport;
CD3DX12_RECT gScissorRect;
UINT gRTVDescriptorSize;
UINT gCurrentBackBufferIndex;

// DirectX 12 resources
ComPtr<ID3D12Resource> gVertexBuffer;
D3D12_VERTEX_BUFFER_VIEW gVertexBufferView;

// DXR specific stuff 

ComPtr<ID3D12Resource> gBLAS;

nv_helpers_dx12::TopLevelASGenerator gTopLevelASGenerator;
AccelerationStructureBuffers gTopLevelASBuffers;
AccelerationStructureBuffers gBottomLevelASBuffers;

ComPtr<IDxcBlob> gRayGenLibrary;
ComPtr<IDxcBlob> gHitLibrary;
ComPtr<IDxcBlob> gMissLibrary;

ComPtr<ID3D12RootSignature> gRayGenSignature;
ComPtr<ID3D12RootSignature> gHitSignature;
ComPtr<ID3D12RootSignature> gMissSignature;

ComPtr<ID3D12StateObject> gRaytracingPipelineState;
ComPtr<ID3D12StateObjectProperties> gRaytracingStateObjectProperties;

ComPtr<ID3D12Resource> gRaytracingOutputBuffer; // The UAV buffer that the RT writes to (gets copied to RTV)
ComPtr<ID3D12DescriptorHeap> gSrvUavHeap; // holds descriptor to the RT output buffer 

nv_helpers_dx12::ShaderBindingTableGenerator gSBTGenerator;
ComPtr<ID3D12Resource> gSBTStorage;

// syncronization stuff

ComPtr<ID3D12Fence> gFence;
uint64_t gFenceValue = 0;
uint64_t gFrameFenceValues[gNumFrames] = {};
HANDLE gFenceEvent;

// settings

bool gVSync = true; // toggle with v key
bool gTearingSupported = false;
bool gFullscreen = false; // toggle with Alt + Enter or F11
bool gRayTracingEnabled = false;

// Window callback function forward decl
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

void parseCommandLineArguments() {
	int argc;

	wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);

	for (int i = 0; i < argc; i++) {
		wchar_t* arg = argv[i];

		if (::wcscmp(arg, L"-w") == 0 || ::wcscmp(arg, L"--width") == 0) {
			gClientWidth = ::wcstol(argv[i + 1], nullptr, 10);
		}
		if (::wcscmp(arg, L"-h") == 0 || ::wcscmp(arg, L"--height") == 0) {
			gClientHeight = ::wcstol(argv[i + 1], nullptr, 10);
		}
	}

	::LocalFree(argv);
}

ComPtr<ID3D12CommandQueue> createCommandQueue(ComPtr<ID3D12Device5> device, D3D12_COMMAND_LIST_TYPE type) {
	ComPtr<ID3D12CommandQueue> d3d12CommandQueue;

	D3D12_COMMAND_QUEUE_DESC desc = {};
	desc.Type = type;
	desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	desc.NodeMask = 0;

	throwIfFailed(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&d3d12CommandQueue)));

	return d3d12CommandQueue;
}

ComPtr<ID3D12Fence> createFence(ComPtr<ID3D12Device5> device) {
	ComPtr<ID3D12Fence> fence;

	throwIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));

	return fence;
}

HANDLE createEventHandle() {
	HANDLE fenceEvent;

	fenceEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
	assert(fenceEvent && "Failed to create fence event.");

	return fenceEvent;
}

uint64_t signal(ComPtr<ID3D12CommandQueue> commandQueue,
	ComPtr<ID3D12Fence> fence,
	uint64_t& fenceValue) {
	fenceValue++;
	uint64_t fenceValueForSignal = fenceValue;

	throwIfFailed(commandQueue->Signal(fence.Get(), fenceValueForSignal));

	return fenceValueForSignal;
}

void waitForFenceValue(ComPtr<ID3D12Fence> fence, uint64_t fenceValue,
	HANDLE fenceEvent, std::chrono::milliseconds duration = std::chrono::milliseconds::max()) {
	if (fence->GetCompletedValue() < fenceValue) {
		throwIfFailed(fence->SetEventOnCompletion(fenceValue, fenceEvent));
		::WaitForSingleObject(fenceEvent, static_cast<DWORD>(duration.count()));
	}
}

void flush(ComPtr<ID3D12CommandQueue> commandQueue,
	ComPtr<ID3D12Fence> fence,
	uint64_t& fenceValue, HANDLE fenceEvent) {
	uint64_t fenceValueForSignal = signal(commandQueue, fence, fenceValue);
	waitForFenceValue(fence, fenceValueForSignal, fenceEvent);
}

void update() {
	static uint64_t frameCounter = 0;
	static double elapsedSeconds = 0.0;
	static std::chrono::high_resolution_clock clock;
	static auto t0 = clock.now();

	frameCounter++;
	auto t1 = clock.now();
	auto deltaTime = t1 - t0;
	t0 = t1;

	elapsedSeconds += deltaTime.count() * 1e-9;

	if (elapsedSeconds > 1.0) {
		char buffer[500];
		auto fps = frameCounter / elapsedSeconds;
		sprintf_s(buffer, 500, "FPS: %f\n", fps);
		OutputDebugString(buffer);

		frameCounter = 0;
		elapsedSeconds = 0.0;
	}
}

void render() {
	auto commandAllocator = gCommandAllocators[gCurrentBackBufferIndex];
	auto backBuffer = gBackBuffers[gCurrentBackBufferIndex];

	throwIfFailed(commandAllocator->Reset());
	throwIfFailed(gCommandList->Reset(commandAllocator.Get(), gPipelineState.Get()));

	// Set state
	{
		gCommandList->SetGraphicsRootSignature(gRootSignature.Get());
		gCommandList->RSSetViewports(1, &gViewport);
		gCommandList->RSSetScissorRects(1, &gScissorRect);
	}

	// Raster
	if (!gRayTracingEnabled) {
		// transition backbuffer state from present to render target 
		CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(backBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
		gCommandList->ResourceBarrier(1, &barrier);

		CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(gRTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
			gCurrentBackBufferIndex, gRTVDescriptorSize);

		gCommandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

		FLOAT clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
		gCommandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);

		// Draw triangle 
		gCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		gCommandList->IASetVertexBuffers(0, 1, &gVertexBufferView);
		gCommandList->DrawInstanced(3, 1, 0, 0);
	}
	else {
	// RT
		// Bind the descriptor heap giving access to RT output buffer as well as TLAS 
		std::vector<ID3D12DescriptorHeap*> heaps = { gSrvUavHeap.Get() };

		gCommandList->SetDescriptorHeaps(static_cast<UINT>(heaps.size()), heaps.data());

		// Prepare RT output buffer
		CD3DX12_RESOURCE_BARRIER transition = CD3DX12_RESOURCE_BARRIER::Transition(gRaytracingOutputBuffer.Get(), 
			D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		gCommandList->ResourceBarrier(1, &transition);

		// Set up raytracing task 
		D3D12_DISPATCH_RAYS_DESC desc = {};

		// Layout of the SBT is as follows 
		// ray generation shader
		// miss shaders
		// hit groups
		
		// All SBT entries of the  same type have the same size to allow fixed stride 

		uint32_t rayGenerationSectionSizeInBytes = gSBTGenerator.GetRayGenSectionSize();
		desc.RayGenerationShaderRecord.StartAddress = gSBTStorage->GetGPUVirtualAddress();
		desc.RayGenerationShaderRecord.SizeInBytes = rayGenerationSectionSizeInBytes;

		uint32_t missSectionSizeInBytes = gSBTGenerator.GetMissSectionSize();
		desc.MissShaderTable.StartAddress = gSBTStorage->GetGPUVirtualAddress() + rayGenerationSectionSizeInBytes;
		desc.MissShaderTable.SizeInBytes = missSectionSizeInBytes;
		desc.MissShaderTable.StrideInBytes = gSBTGenerator.GetMissEntrySize();

		uint32_t hitGroupsSectionSize = gSBTGenerator.GetHitGroupSectionSize();
		desc.HitGroupTable.StartAddress = gSBTStorage->GetGPUVirtualAddress() + rayGenerationSectionSizeInBytes + missSectionSizeInBytes;
		desc.HitGroupTable.SizeInBytes = hitGroupsSectionSize;
		desc.HitGroupTable.StrideInBytes = gSBTGenerator.GetHitGroupEntrySize();

		desc.Width = gClientWidth;
		desc.Height = gClientHeight;
		desc.Depth = 1;

		// bind RT pipeline
		gCommandList->SetPipelineState1(gRaytracingPipelineState.Get());

		// Dispatch the rays, which writes to RT output buffer
		gCommandList->DispatchRays(&desc);

		// Now copy RT output buffer to the render target 
		transition = CD3DX12_RESOURCE_BARRIER::Transition(gRaytracingOutputBuffer.Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE); 

		gCommandList->ResourceBarrier(1, &transition);

		transition = CD3DX12_RESOURCE_BARRIER::Transition(backBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);

		gCommandList->ResourceBarrier(1, &transition);

		gCommandList->CopyResource(backBuffer.Get(), gRaytracingOutputBuffer.Get());

		transition = CD3DX12_RESOURCE_BARRIER::Transition(backBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);

		gCommandList->ResourceBarrier(1, &transition);
	}

	// Present
	{
		// transition backbuffer state from render target to present 
		CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
			backBuffer.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

		gCommandList->ResourceBarrier(1, &barrier);

		throwIfFailed(gCommandList->Close());

		ID3D12CommandList* const commandLists[] = {
			gCommandList.Get() 
		};

		gCommandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);

		UINT syncInterval = gVSync ? 1 : 0;
		UINT presentFlags = gTearingSupported && !gVSync ? DXGI_PRESENT_ALLOW_TEARING : 0;
		throwIfFailed(gSwapChain->Present(syncInterval, presentFlags));

		gFrameFenceValues[gCurrentBackBufferIndex] = signal(gCommandQueue, gFence, gFenceValue);

		gCurrentBackBufferIndex = gSwapChain->GetCurrentBackBufferIndex();

		waitForFenceValue(gFence, gFrameFenceValues[gCurrentBackBufferIndex], gFenceEvent);
	}
}

void updateRenderTargetViews(ComPtr<ID3D12Device5> device,
	ComPtr<IDXGISwapChain4> swapChain, ComPtr<ID3D12DescriptorHeap> descriptorHeap) {
	auto rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(descriptorHeap->GetCPUDescriptorHandleForHeapStart());

	for (int i = 0; i < gNumFrames; ++i)
	{
		ComPtr<ID3D12Resource> backBuffer;
		throwIfFailed(swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer)));

		device->CreateRenderTargetView(backBuffer.Get(), nullptr, rtvHandle);

		gBackBuffers[i] = backBuffer;

		rtvHandle.Offset(rtvDescriptorSize);
	}
}

void resize(uint32_t width, uint32_t height)
{
	if (gClientWidth != width || gClientHeight != height)
	{
		// Don't allow 0 size swap chain back buffers.
		gClientWidth = std::max(1u, width);
		gClientHeight = std::max(1u, height);

		// Flush the GPU queue to make sure the swap chain's back buffers
		// are not being referenced by an in-flight command list.
		flush(gCommandQueue, gFence, gFenceValue, gFenceEvent);

		for (int i = 0; i < gNumFrames; ++i)
		{
			// Any references to the back buffers must be released
			// before the swap chain can be resized.
			gBackBuffers[i].Reset();
			gFrameFenceValues[i] = gFrameFenceValues[gCurrentBackBufferIndex];
		}

		DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
		throwIfFailed(gSwapChain->GetDesc(&swapChainDesc));
		throwIfFailed(gSwapChain->ResizeBuffers(gNumFrames, gClientWidth, gClientHeight,
			swapChainDesc.BufferDesc.Format, swapChainDesc.Flags));

		gCurrentBackBufferIndex = gSwapChain->GetCurrentBackBufferIndex();

		updateRenderTargetViews(gDevice, gSwapChain, gRTVDescriptorHeap);
	}
}

void setFullscreen(bool fullscreen) {
	if (gFullscreen != fullscreen) {
		gFullscreen = fullscreen;

		// we changed to fullscreen
		if (gFullscreen) {
			// store current window dimentions so that we can restore
			// them when coming out of fullscreen
			::GetWindowRect(ghWnd, &gWindowRect);

			// use borderless window style
			UINT windowStyle = WS_OVERLAPPEDWINDOW & ~(WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);

			::SetWindowLongW(ghWnd, GWL_STYLE, windowStyle);

			// query the name of the nearest display device for the window
			// so that we can find the dimensions of that monitor
			HMONITOR hMonitor = ::MonitorFromWindow(ghWnd, MONITOR_DEFAULTTONEAREST);
			MONITORINFOEX monitorInfo = {};
			monitorInfo.cbSize = sizeof(MONITORINFOEX);
			::GetMonitorInfo(hMonitor, &monitorInfo);

			// set fullscreen window size
			::SetWindowPos(ghWnd,
				HWND_TOP,
				monitorInfo.rcMonitor.left,
				monitorInfo.rcMonitor.top,
				monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
				monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
				SWP_FRAMECHANGED | SWP_NOACTIVATE);

			::ShowWindow(ghWnd, SW_MAXIMIZE);
		}
		else {
			// going out of fullscreen, restore windowed state
			::SetWindowLong(ghWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW);

			::SetWindowPos(ghWnd,
				HWND_NOTOPMOST,
				gWindowRect.left,
				gWindowRect.top,
				gWindowRect.right - gWindowRect.left,
				gWindowRect.bottom - gWindowRect.top,
				SWP_FRAMECHANGED | SWP_NOACTIVATE);

			::ShowWindow(ghWnd, SW_NORMAL);
		}
	}
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (gIsInitialized)
	{
		switch (message)
		{
		case WM_PAINT:
			update();
			render();
			break;
		case WM_SYSKEYDOWN:
		case WM_KEYDOWN:
		{
			bool alt = (::GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

			switch (wParam)
			{
			case 'V':
				gVSync = !gVSync;
				break;
			case VK_SPACE:
				gRayTracingEnabled = !gRayTracingEnabled;

				if (gRayTracingEnabled) {
					::SetWindowText(ghWnd, "Unreal Engine 6 (RTX: on)");
				}
				else {
					::SetWindowText(ghWnd, "Unreal Engine 6 (RTX: off)");
				}

				break;
			case VK_ESCAPE:
				::PostQuitMessage(0);
				break;
			case VK_RETURN:
				if (alt)
				{
			case VK_F11:
				setFullscreen(!gFullscreen);
				}
				break;
			}
		}
		break;
		// The default window procedure will play a system notification sound 
		// when pressing the Alt+Enter keyboard combination if this message is 
		// not handled.
		case WM_SYSCHAR:
			break;
		case WM_SIZE:
		{
			RECT clientRect = {};
			::GetClientRect(ghWnd, &clientRect);

			int width = clientRect.right - clientRect.left;
			int height = clientRect.bottom - clientRect.top;

			resize(width, height);
		}
		break;
		case WM_DESTROY:
			::PostQuitMessage(0);
			break;
		default:
			return ::DefWindowProcW(hwnd, message, wParam, lParam);
		}
	}
	else
	{
		return ::DefWindowProcW(hwnd, message, wParam, lParam);
	}

	return 0;
}

void enableDebugLayer() {
	// you want to do this before anything else to catch errors
	ComPtr<ID3D12Debug> debugInterface;
	throwIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debugInterface)));
	debugInterface->EnableDebugLayer();
}

bool checkTearingSupport() {
	BOOL allowTearing = FALSE;

	ComPtr<IDXGIFactory4> factory4;
	if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory4)))) {
		ComPtr<IDXGIFactory5> factory5;
		if (SUCCEEDED(factory4.As(&factory5))) {
			if (FAILED(factory5->CheckFeatureSupport(
				DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing)))) {
				allowTearing = FALSE;
			}
		}
	}

	return allowTearing == TRUE;
}

void registerWindowClass(HINSTANCE hInst, const wchar_t* windowClassName)
{
	// Register a window class for creating our render window with.
	WNDCLASSEXW windowClass = {};

	windowClass.cbSize = sizeof(WNDCLASSEX);
	windowClass.style = CS_HREDRAW | CS_VREDRAW;
	windowClass.lpfnWndProc = &WndProc;
	windowClass.cbClsExtra = 0;
	windowClass.cbWndExtra = 0;
	windowClass.hInstance = hInst;
	windowClass.hIcon = ::LoadIcon(hInst, NULL);
	windowClass.hCursor = ::LoadCursor(NULL, IDC_ARROW);
	windowClass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	windowClass.lpszMenuName = NULL;
	windowClass.lpszClassName = windowClassName;
	windowClass.hIconSm = ::LoadIcon(hInst, NULL);

	static ATOM atom = ::RegisterClassExW(&windowClass);
	assert(atom > 0);
}

HWND createWindow(const wchar_t* windowClassName, HINSTANCE hInst,
	const wchar_t* windowTitle, uint32_t width, uint32_t height)
{
	int screenWidth = ::GetSystemMetrics(SM_CXSCREEN);
	int screenHeight = ::GetSystemMetrics(SM_CYSCREEN);

	RECT windowRect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
	::AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

	int windowWidth = windowRect.right - windowRect.left;
	int windowHeight = windowRect.bottom - windowRect.top;

	// Center the window within the screen. Clamp to 0, 0 for the top-left corner.
	int windowX = std::max<int>(0, (screenWidth - windowWidth) / 2);
	int windowY = std::max<int>(0, (screenHeight - windowHeight) / 2);

	HWND hWnd = ::CreateWindowExW(
		NULL,
		windowClassName,
		windowTitle,
		WS_OVERLAPPEDWINDOW,
		windowX,
		windowY,
		windowWidth,
		windowHeight,
		NULL,
		NULL,
		hInst,
		nullptr
	);

	assert(hWnd && "Failed to create window");

	return hWnd;
}

ComPtr<ID3D12Device5>
createDevice(ComPtr<IDXGIAdapter4> adapter) {
	ComPtr<ID3D12Device5> d3d12Device5;
	throwIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&d3d12Device5)));

#if defined(_DEBUG)
	ComPtr<ID3D12InfoQueue> infoQueue;
	if (SUCCEEDED(d3d12Device5.As(&infoQueue))) {
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);

		D3D12_MESSAGE_SEVERITY severities[] = {
			D3D12_MESSAGE_SEVERITY_INFO
		};

		D3D12_MESSAGE_ID denyIds[] = {
			D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
			D3D12_MESSAGE_ID_MAP_INVALID_NULLRANGE,
			D3D12_MESSAGE_ID_UNMAP_INVALID_NULLRANGE
		};

		D3D12_INFO_QUEUE_FILTER newFilter = {};
		newFilter.DenyList.NumSeverities = _countof(severities);
		newFilter.DenyList.pSeverityList = severities;
		newFilter.DenyList.NumIDs = _countof(denyIds);
		newFilter.DenyList.pIDList = denyIds;

		throwIfFailed(infoQueue->PushStorageFilter(&newFilter));
	}
#endif

	return d3d12Device5;
}

ComPtr<IDXGISwapChain4> createSwapChain(HWND hWnd,
	ComPtr<ID3D12CommandQueue> commandQueue,
	uint32_t width, uint32_t height, uint32_t bufferCount)
{
	ComPtr<IDXGISwapChain4> dxgiSwapChain4;
	ComPtr<IDXGIFactory4> dxgiFactory4;
	UINT createFactoryFlags = 0;
#if defined(_DEBUG)
	createFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif

	throwIfFailed(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&dxgiFactory4)));

	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.Width = width;
	swapChainDesc.Height = height;
	swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapChainDesc.Stereo = FALSE;
	swapChainDesc.SampleDesc = { 1, 0 };
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.BufferCount = bufferCount;
	swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
	// It is recommended to always allow tearing if tearing support is available.
	swapChainDesc.Flags = checkTearingSupport() ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

	ComPtr<IDXGISwapChain1> swapChain1;
	throwIfFailed(dxgiFactory4->CreateSwapChainForHwnd(
		commandQueue.Get(),
		hWnd,
		&swapChainDesc,
		nullptr,
		nullptr,
		&swapChain1));

	// Disable the Alt+Enter fullscreen toggle feature. Switching to fullscreen
	// will be handled manually.
	throwIfFailed(dxgiFactory4->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER));

	throwIfFailed(swapChain1.As(&dxgiSwapChain4));

	return dxgiSwapChain4;
}

ComPtr<ID3D12DescriptorHeap>
createDescriptorHeap(ComPtr<ID3D12Device5> device, UINT numDescriptors, D3D12_DESCRIPTOR_HEAP_TYPE type) {

	D3D12_DESCRIPTOR_HEAP_DESC desc = {};
	desc.Type = type;
	desc.NumDescriptors = numDescriptors;
	desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	desc.NodeMask = 0;

	ComPtr<ID3D12DescriptorHeap> descriptorHeap;

	throwIfFailed(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&descriptorHeap)));

	return descriptorHeap;
}

ComPtr<ID3D12CommandAllocator>
createCommandAllocator(ComPtr<ID3D12Device5> device,
	D3D12_COMMAND_LIST_TYPE type) {
	ComPtr<ID3D12CommandAllocator> commandAllocator;
	throwIfFailed(device->CreateCommandAllocator(type, IID_PPV_ARGS(&commandAllocator)));

	return commandAllocator;
}

ComPtr<IDXGIAdapter4>
getAdapter() {
	ComPtr<IDXGIFactory4> dxgiFactory;
	UINT createFactoryFlags = 0;

#if defined(_DEBUG)
	createFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif

	throwIfFailed(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&dxgiFactory)));

	ComPtr<IDXGIAdapter1> dxgiAdapter1;
	ComPtr<IDXGIAdapter4> dxgiAdapter4;

	SIZE_T maxDedicatedVideoMemory = 0;

	for (UINT i = 0; dxgiFactory->EnumAdapters1(i, &dxgiAdapter1) != DXGI_ERROR_NOT_FOUND; i++) {
		DXGI_ADAPTER_DESC1 dxgiAdapterDesc1;
		dxgiAdapter1->GetDesc1(&dxgiAdapterDesc1);

		// check to see if that adapter can create a D3D12 device 
		// the adapter with largest VRAM is picked
		if ((dxgiAdapterDesc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
			SUCCEEDED(D3D12CreateDevice(dxgiAdapter1.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)) &&
			dxgiAdapterDesc1.DedicatedVideoMemory > maxDedicatedVideoMemory) {
			maxDedicatedVideoMemory = dxgiAdapterDesc1.DedicatedVideoMemory;
			throwIfFailed(dxgiAdapter1.As(&dxgiAdapter4));
		}
	}

	return dxgiAdapter4;

}

ComPtr<ID3D12GraphicsCommandList4>
createCommandList(ComPtr<ID3D12Device5> device,
	ComPtr<ID3D12CommandAllocator> commandAllocator, D3D12_COMMAND_LIST_TYPE type) {
	ComPtr<ID3D12GraphicsCommandList4> commandList;

	throwIfFailed(device->CreateCommandList(0, type, commandAllocator.Get(), nullptr, IID_PPV_ARGS(&commandList)));

	return commandList;
}

bool checkRayTracingSupport(ComPtr<ID3D12Device5> device) {
	D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};

	throwIfFailed(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)));

	if (options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0) {
		throw std::runtime_error("Raytracing not supported on device.");
	}

	OutputDebugString("Raytracing is supported.\n");
}

ComPtr<ID3D12RootSignature> 
createRootSignature(ComPtr<ID3D12Device5> device) {
	CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;

	rootSignatureDesc.Init(0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	ComPtr<ID3DBlob> signatureBlob;
	ComPtr<ID3DBlob> errorBlob;

	throwIfFailed(D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob, &errorBlob));

	ComPtr<ID3D12RootSignature> signature;

	throwIfFailed(device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(), IID_PPV_ARGS(&signature)));

	return signature;
}

ComPtr<ID3D12PipelineState>
createPipelineState(ComPtr<ID3D12Device5> device, ComPtr<ID3D12RootSignature> rootSignature) {
	ComPtr<ID3DBlob> vertexShader;
	ComPtr<ID3DBlob> pixelShader;

	#if defined(_DEBUG)
		// Enable better shader debugging with the graphics debugging tools.
		UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
	#else
		UINT compileFlags = 0;
	#endif
		
	std::filesystem::path vertexShaderPath = std::filesystem::current_path() / "shaders" / "Vertex.hlsl";
	std::filesystem::path pixelShaderPath = std::filesystem::current_path() / "shaders" / "Pixel.hlsl";

	throwIfFailed(D3DCompileFromFile(vertexShaderPath.c_str(), nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, nullptr));
	throwIfFailed(D3DCompileFromFile(pixelShaderPath.c_str(), nullptr, nullptr, "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, nullptr));

	// Define the vertex input layout 
	D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
	};

	// now create the PSO 
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};

	psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
	psoDesc.pRootSignature = rootSignature.Get();
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(vertexShader.Get());
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(pixelShader.Get());
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.DepthStencilState.StencilEnable = FALSE;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDesc.SampleDesc.Count = 1;

	ComPtr<ID3D12PipelineState> pipelineState;
	
	throwIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineState)));

	return pipelineState;
}

ComPtr<ID3D12Resource> 
createVertexBuffer(ComPtr<ID3D12Device5> device, ComPtr<ID3D12CommandQueue> commandQueue, 
	D3D12_VERTEX_BUFFER_VIEW &vertexBufferView) {
	Vertex triangleVertices[] =
	{
		{ { 0.0f, 0.25f, 0.0f }, { 1.0f, 0.0f, 0.0f, 1.0f } },
		{ { 0.25f, -0.25f, 0.0f }, { 0.0f, 1.0f, 0.0f, 1.0f } },
		{ { -0.25f, -0.25f, 0.0f }, { 0.0f, 0.0f, 1.0f, 1.0f } }
	};

	const UINT vertexBufferSize = sizeof(triangleVertices);

	ComPtr<ID3D12Resource> vertexBuffer;

	D3D12_HEAP_PROPERTIES heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	D3D12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);

	// Note: upload heaps not recommended for transfering vert buffers normally 
	throwIfFailed(device->CreateCommittedResource(
		&heapProperties,
		D3D12_HEAP_FLAG_NONE,
		&resourceDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&vertexBuffer)
	));

	// copy triangle data to the VB 
	UINT8* pVertexDataBegin;
	CD3DX12_RANGE readRange(0, 0); // We are not reading from it 
	throwIfFailed(vertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pVertexDataBegin)));
	memcpy(pVertexDataBegin, triangleVertices, sizeof(triangleVertices));
	vertexBuffer->Unmap(0, nullptr);

	vertexBufferView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
	vertexBufferView.StrideInBytes = sizeof(Vertex);
	vertexBufferView.SizeInBytes = vertexBufferSize;

	return vertexBuffer;
}

int CALLBACK wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, int nCmdShow) {

	// client area of window can have 100% scaling while non-client window content
	// can still be drawn in DPI sensitive fashion
	// without this our client area would get scaled based on DPI scaling 
	// we don't want that here 
	SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	parseCommandLineArguments();

	gViewport = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(gClientWidth), static_cast<float>(gClientHeight));
	gScissorRect = CD3DX12_RECT(0, 0, static_cast<LONG>(gClientWidth), static_cast<LONG>(gClientHeight));

	enableDebugLayer();

	gTearingSupported = checkTearingSupport();

	// Window stuff
	const wchar_t* windowClassName = L"DX12WindowClass";
	registerWindowClass(hInstance, windowClassName);
	ghWnd = createWindow(windowClassName, hInstance, 
		L"Unreal Engine 6", gClientWidth, gClientHeight);

	// Init global window rect variable
	::GetWindowRect(ghWnd, &gWindowRect);

	// DirectX 12 stuff
	ComPtr<IDXGIAdapter4> dxgiAdapter4 = getAdapter();

	gDevice = createDevice(dxgiAdapter4);

	checkRayTracingSupport(gDevice);

	gCommandQueue = createCommandQueue(gDevice, D3D12_COMMAND_LIST_TYPE_DIRECT);

	gSwapChain = createSwapChain(ghWnd, gCommandQueue, gClientWidth, gClientHeight, gNumFrames);

	gCurrentBackBufferIndex = gSwapChain->GetCurrentBackBufferIndex();

	gRTVDescriptorHeap = createDescriptorHeap(gDevice, gNumFrames, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	gRTVDescriptorSize = gDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	updateRenderTargetViews(gDevice, gSwapChain, gRTVDescriptorHeap);

	for (int i = 0; i < gNumFrames; i++) {
		gCommandAllocators[i] = createCommandAllocator(gDevice, D3D12_COMMAND_LIST_TYPE_DIRECT);
	}

	gCommandList = createCommandList(gDevice, gCommandAllocators[gCurrentBackBufferIndex], D3D12_COMMAND_LIST_TYPE_DIRECT);

	gFence = createFence(gDevice);
	gFenceEvent = createEventHandle();

	gRootSignature = createRootSignature(gDevice);

	gPipelineState = createPipelineState(gDevice, gRootSignature);

	gVertexBuffer = createVertexBuffer(gDevice, gCommandQueue, gVertexBufferView);

	createAccelerationStructures(gDevice, gCommandList, gVertexBuffer, 
		gTopLevelASGenerator, gBottomLevelASBuffers, gTopLevelASBuffers);

	gRaytracingPipelineState = createRaytracingPipelineState(gDevice, gRayGenLibrary, gHitLibrary, 
		gMissLibrary, gRayGenSignature, gHitSignature, gMissSignature,
		gRaytracingStateObjectProperties);

	gRaytracingOutputBuffer = createRaytracingOutputBuffer(gDevice, gClientWidth, gClientHeight);

	gSrvUavHeap = createShaderResourceHeap(gDevice, gRaytracingOutputBuffer, gTopLevelASBuffers);

	gSBTStorage = createShaderBindingTable(gDevice, gSBTGenerator, gSrvUavHeap, gVertexBuffer, gRaytracingStateObjectProperties);

	// Flush command list to make sure everything above finished 
	throwIfFailed(gCommandList->Close());
	ID3D12CommandList* const commandLists[] = { gCommandList.Get() };
	gCommandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);

	flush(gCommandQueue, gFence, gFenceValue, gFenceEvent);
	gCurrentBackBufferIndex = gSwapChain->GetCurrentBackBufferIndex();

	// now start the message loop
	gIsInitialized = true;

	::ShowWindow(ghWnd, SW_SHOW);

	MSG msg = {};
	while (msg.message != WM_QUIT)
	{
		if (::PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
		{
			::TranslateMessage(&msg);
			::DispatchMessage(&msg);
		}
	}

	flush(gCommandQueue, gFence, gFenceValue, gFenceEvent);

	::CloseHandle(gFenceEvent);

	return 0;
}