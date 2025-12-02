/****************************************************************************************/
/*  DIRECT3D12DRIVER.CPP                                                                */
/*                                                                                      */
/*  Author: Styx3D Modernization                                                        */
/*  Description: DirectX 12 rendering driver implementation for Windows 11             */
/*                                                                                      */
/*  The contents of this file are subject to the 0BSD License.                          */
/*  See LICENSE file in the project root for full license information.                  */
/*                                                                                      */
/****************************************************************************************/
#include "Direct3D12Driver.h"
#include "D3D12Log.h"
#include "D3D12TextureMgr.h"
#include "D3D12PolyCache.h"
#include "pixelformat.h"
#include <dxgidebug.h>

#ifdef _DEBUG
#pragma comment(lib, "Jet3DClassic11d.lib")
#else
#pragma comment(lib, "Jet3DClassic11.lib")
#endif

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")

// Global D3D12 objects
jeRDriver_PixelFormat						g_PixelFormat[10];
DRV_EngineSettings							g_EngineSettings;
HWND										g_hWnd = nullptr;
ComPtr<IDXGIFactory4>						g_pFactory;
ComPtr<ID3D12Device>						g_pDevice;
ComPtr<ID3D12CommandQueue>					g_pCommandQueue;
ComPtr<IDXGISwapChain3>						g_pSwapChain;
ComPtr<ID3D12DescriptorHeap>				g_pRTVHeap;
ComPtr<ID3D12DescriptorHeap>				g_pDSVHeap;
ComPtr<ID3D12DescriptorHeap>				g_pSRVHeap;
ComPtr<ID3D12Resource>						g_pRenderTargets[FRAME_COUNT];
ComPtr<ID3D12Resource>						g_pDepthStencil;
ComPtr<ID3D12CommandAllocator>				g_pCommandAllocators[FRAME_COUNT];
ComPtr<ID3D12GraphicsCommandList>			g_pCommandList;
ComPtr<ID3D12RootSignature>					g_pRootSignature;
ComPtr<ID3D12PipelineState>					g_pPipelineState;
ComPtr<ID3D12Fence>							g_pFence;
UINT64										g_FenceValues[FRAME_COUNT] = {};
HANDLE										g_FenceEvent = nullptr;
UINT										g_FrameIndex = 0;
UINT										g_RTVDescriptorSize = 0;
UINT										g_DSVDescriptorSize = 0;
UINT										g_CBVSRVDescriptorSize = 0;
float										g_LocalGamma = 1.0f;
D3D12_VIEWPORT								g_Viewport = {};
D3D12_RECT									g_ScissorRect = {};
bool										g_IsWindowed = true;

RGB_LUT										g_Lut1 = {};
D3D12PolyCache*								g_pPolyCache = nullptr;

// Storage for matrices
DirectX::XMFLOAT4X4							g_WorldMatrix;
DirectX::XMFLOAT4X4							g_ViewMatrix;
DirectX::XMFLOAT4X4							g_ProjMatrix;

#define JE_FONT_NORMAL			0x00000001
#define JE_FONT_BOLD			0x00000002

// Helper function to convert DXGI format
DXGI_FORMAT GetDXGIFormat(jePixelFormat format)
{
	switch (format)
	{
	case JE_PIXELFORMAT_32BIT_ARGB:
	case JE_PIXELFORMAT_32BIT_XRGB:
		return DXGI_FORMAT_B8G8R8A8_UNORM;
	case JE_PIXELFORMAT_24BIT_RGB:
		return DXGI_FORMAT_B8G8R8X8_UNORM;
	case JE_PIXELFORMAT_16BIT_565_RGB:
		return DXGI_FORMAT_B5G6R5_UNORM;
	case JE_PIXELFORMAT_16BIT_555_RGB:
	case JE_PIXELFORMAT_16BIT_1555_ARGB:
		return DXGI_FORMAT_B5G5R5A1_UNORM;
	case JE_PIXELFORMAT_16BIT_4444_ARGB:
		return DXGI_FORMAT_B4G4R4A4_UNORM;
	default:
		return DXGI_FORMAT_R8G8B8A8_UNORM;
	}
}

void TransitionResource(ID3D12GraphicsCommandList* pCmdList, ID3D12Resource* pResource,
	D3D12_RESOURCE_STATES stateBefore, D3D12_RESOURCE_STATES stateAfter)
{
	if (stateBefore == stateAfter)
		return;

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = pResource;
	barrier.Transition.StateBefore = stateBefore;
	barrier.Transition.StateAfter = stateAfter;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	pCmdList->ResourceBarrier(1, &barrier);
}

void WaitForGPU()
{
	if (!g_pCommandQueue || !g_pFence)
		return;

	// Schedule a fence signal
	const UINT64 currentFenceValue = g_FenceValues[g_FrameIndex];
	g_pCommandQueue->Signal(g_pFence.Get(), currentFenceValue);

	// Wait until the fence has been processed
	if (g_pFence->GetCompletedValue() < currentFenceValue)
	{
		g_pFence->SetEventOnCompletion(currentFenceValue, g_FenceEvent);
		WaitForSingleObjectEx(g_FenceEvent, INFINITE, FALSE);
	}

	g_FenceValues[g_FrameIndex]++;
}

void MoveToNextFrame()
{
	// Schedule a signal command in the queue
	const UINT64 currentFenceValue = g_FenceValues[g_FrameIndex];
	g_pCommandQueue->Signal(g_pFence.Get(), currentFenceValue);

	// Update the frame index
	g_FrameIndex = g_pSwapChain->GetCurrentBackBufferIndex();

	// If the next frame is not ready to be rendered yet, wait until it is ready
	if (g_pFence->GetCompletedValue() < g_FenceValues[g_FrameIndex])
	{
		g_pFence->SetEventOnCompletion(g_FenceValues[g_FrameIndex], g_FenceEvent);
		WaitForSingleObjectEx(g_FenceEvent, INFINITE, FALSE);
	}

	// Set the fence value for the next frame
	g_FenceValues[g_FrameIndex] = currentFenceValue + 1;
}

void jeXForm3d_ToD3DMatrix(jeXForm3d* XForm, DirectX::XMFLOAT4X4* mat)
{
	mat->_11 = XForm->AX;
	mat->_12 = XForm->AY;
	mat->_13 = XForm->AZ;
	mat->_14 = 0.0f;

	mat->_21 = XForm->BX;
	mat->_22 = XForm->BY;
	mat->_23 = XForm->BZ;
	mat->_24 = 0.0f;

	mat->_31 = XForm->CX;
	mat->_32 = XForm->CY;
	mat->_33 = XForm->CZ;
	mat->_34 = 0.0f;

	mat->_41 = XForm->Translation.X;
	mat->_42 = XForm->Translation.Y;
	mat->_43 = XForm->Translation.Z;
	mat->_44 = 1.0f;
}

void D3DMatrix_ToXForm3d(DirectX::XMFLOAT4X4* mat, jeXForm3d* XForm)
{
	XForm->AX = mat->_11;
	XForm->AY = mat->_12;
	XForm->AZ = mat->_13;

	XForm->BX = mat->_21;
	XForm->BY = mat->_22;
	XForm->BZ = mat->_23;

	XForm->CX = mat->_31;
	XForm->CY = mat->_32;
	XForm->CZ = mat->_33;

	XForm->Translation.X = mat->_41;
	XForm->Translation.Y = mat->_42;
	XForm->Translation.Z = mat->_43;
}

jeBoolean DRIVERCC D3D12Drv_EnumSubDrivers(DRV_ENUM_DRV_CB* Cb, void* Context)
{
	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  EnumSubDrivers()");
	else
		REPORT("Function Call:  EnumSubDrivers()");

	Cb(1, "Direct3D 12 Driver", Context);
	return JE_TRUE;
}

jeBoolean DRIVERCC D3D12Drv_EnumModes(S32 Driver, char* DriverName, DRV_ENUM_MODES_CB* Cb, void* Context)
{
	HRESULT hres;
	int32 numModes = 0;

	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  EnumModes()");
	else
		REPORT("Function Call:  EnumModes()");

	// Create factory if needed
	ComPtr<IDXGIFactory4> pFactory;
	hres = CreateDXGIFactory2(0, IID_PPV_ARGS(&pFactory));
	if (FAILED(hres))
	{
		D3D12Log::GetPtr()->Printf("ERROR:  Could not create DXGI factory!!");
		return FALSE;
	}

	// Get default adapter
	ComPtr<IDXGIAdapter1> pAdapter;
	pFactory->EnumAdapters1(0, &pAdapter);

	// Get default output
	ComPtr<IDXGIOutput> pOutput;
	hres = pAdapter->EnumOutputs(0, &pOutput);
	if (FAILED(hres))
	{
		D3D12Log::GetPtr()->Printf("ERROR:  Could not enumerate outputs!!");
		return FALSE;
	}

	// Enumerate display modes
	DXGI_FORMAT formats[] = { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM };

	for (int f = 0; f < 2; f++)
	{
		UINT modeCount = 0;
		pOutput->GetDisplayModeList(formats[f], 0, &modeCount, nullptr);

		if (modeCount == 0)
			continue;

		std::vector<DXGI_MODE_DESC> modes(modeCount);
		pOutput->GetDisplayModeList(formats[f], 0, &modeCount, modes.data());

		for (UINT i = 0; i < modeCount; i++)
		{
			if (modes[i].Width <= 3840 && modes[i].Height <= 2160 &&
				modes[i].RefreshRate.Numerator / modes[i].RefreshRate.Denominator >= 60)
			{
				char modename[32];
				int32 bpp = (formats[f] == DXGI_FORMAT_R8G8B8A8_UNORM || formats[f] == DXGI_FORMAT_B8G8R8A8_UNORM) ? 32 : 16;

				sprintf_s(modename, "%dx%dx%d", modes[i].Width, modes[i].Height, bpp);
				Cb(numModes, modename, modes[i].Width, modes[i].Height, bpp, Context);
				numModes++;
			}
		}
	}

	Cb(numModes, "WindowMode", -1, -1, -1, Context);

	return TRUE;
}

jeBoolean DRIVERCC D3D12Drv_Init(DRV_DriverHook* hook)
{
	HRESULT hres;
	int32 w = 0, h = 0, bpp = 0;

	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  Init()");
	else
		REPORT("Function Call:  Init()");

	g_hWnd = hook->hWnd;

	// Enable debug layer in debug builds
#ifdef _DEBUG
	{
		ComPtr<ID3D12Debug> debugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
		{
			debugController->EnableDebugLayer();
			D3D12Log::GetPtr()->Printf("DEBUG:  D3D12 Debug layer enabled");
		}
	}
#endif

	// Create DXGI Factory
	UINT dxgiFactoryFlags = 0;
#ifdef _DEBUG
	dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
	hres = CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&g_pFactory));
	if (FAILED(hres))
	{
		D3D12Log::GetPtr()->Printf("ERROR:  Could not create DXGI factory!!");
		return JE_FALSE;
	}

	// Find a hardware adapter that supports D3D12
	ComPtr<IDXGIAdapter1> hardwareAdapter;
	{
		ComPtr<IDXGIAdapter1> adapter;
		for (UINT adapterIndex = 0;
			SUCCEEDED(g_pFactory->EnumAdapters1(adapterIndex, &adapter));
			++adapterIndex)
		{
			DXGI_ADAPTER_DESC1 desc;
			adapter->GetDesc1(&desc);

			if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
				continue;

			// Check if adapter supports D3D12
			if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
				_uuidof(ID3D12Device), nullptr)))
			{
				hardwareAdapter = adapter;
				break;
			}
		}
	}

	// Create D3D12 device
	hres = D3D12CreateDevice(hardwareAdapter.Get(), D3D_FEATURE_LEVEL_12_0,
		IID_PPV_ARGS(&g_pDevice));
	if (FAILED(hres))
	{
		// Try with feature level 11_0
		hres = D3D12CreateDevice(hardwareAdapter.Get(), D3D_FEATURE_LEVEL_11_0,
			IID_PPV_ARGS(&g_pDevice));
		if (FAILED(hres))
		{
			D3D12Log::GetPtr()->Printf("ERROR:  Could not create D3D12 device!!");
			return JE_FALSE;
		}
		D3D12Log::GetPtr()->Printf("DEVICE:  Created with Feature Level 11.0");
	}
	else
	{
		D3D12Log::GetPtr()->Printf("DEVICE:  Created with Feature Level 12.0");
	}

	// Create command queue
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	hres = g_pDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_pCommandQueue));
	if (FAILED(hres))
	{
		D3D12Log::GetPtr()->Printf("ERROR:  Could not create command queue!!");
		return JE_FALSE;
	}

	// Parse mode info
	sscanf_s(hook->ModeName, "%dx%dx%d", &w, &h, &bpp);

	// Determine window dimensions
	if (hook->Width == -1 && hook->Height == -1)
	{
		RECT r;
		GetClientRect(hook->hWnd, &r);
		w = r.right - r.left;
		h = r.bottom - r.top;
		g_IsWindowed = true;
	}
	else
	{
		w = hook->Width;
		h = hook->Height;
		g_IsWindowed = false;
	}

	// Create swap chain
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.Width = w;
	swapChainDesc.Height = h;
	swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapChainDesc.SampleDesc.Count = 1;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.BufferCount = FRAME_COUNT;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

	ComPtr<IDXGISwapChain1> swapChain;
	hres = g_pFactory->CreateSwapChainForHwnd(
		g_pCommandQueue.Get(),
		hook->hWnd,
		&swapChainDesc,
		nullptr,
		nullptr,
		&swapChain);
	if (FAILED(hres))
	{
		D3D12Log::GetPtr()->Printf("ERROR:  Could not create swap chain!!");
		return JE_FALSE;
	}

	// Disable Alt+Enter fullscreen toggle
	g_pFactory->MakeWindowAssociation(hook->hWnd, DXGI_MWA_NO_ALT_ENTER);

	swapChain.As(&g_pSwapChain);
	g_FrameIndex = g_pSwapChain->GetCurrentBackBufferIndex();

	// Create descriptor heaps
	g_RTVDescriptorSize = g_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	g_DSVDescriptorSize = g_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	g_CBVSRVDescriptorSize = g_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	// RTV heap
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
	rtvHeapDesc.NumDescriptors = FRAME_COUNT;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	hres = g_pDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_pRTVHeap));
	if (FAILED(hres))
	{
		D3D12Log::GetPtr()->Printf("ERROR:  Could not create RTV heap!!");
		return JE_FALSE;
	}

	// DSV heap
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
	dsvHeapDesc.NumDescriptors = 1;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	hres = g_pDevice->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&g_pDSVHeap));
	if (FAILED(hres))
	{
		D3D12Log::GetPtr()->Printf("ERROR:  Could not create DSV heap!!");
		return JE_FALSE;
	}

	// SRV heap for textures (1000 descriptors should be enough)
	D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
	srvHeapDesc.NumDescriptors = 1000;
	srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	hres = g_pDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&g_pSRVHeap));
	if (FAILED(hres))
	{
		D3D12Log::GetPtr()->Printf("ERROR:  Could not create SRV heap!!");
		return JE_FALSE;
	}

	// Create render target views
	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_pRTVHeap->GetCPUDescriptorHandleForHeapStart();
	for (UINT i = 0; i < FRAME_COUNT; i++)
	{
		hres = g_pSwapChain->GetBuffer(i, IID_PPV_ARGS(&g_pRenderTargets[i]));
		if (FAILED(hres))
		{
			D3D12Log::GetPtr()->Printf("ERROR:  Could not get swap chain buffer!!");
			return JE_FALSE;
		}
		g_pDevice->CreateRenderTargetView(g_pRenderTargets[i].Get(), nullptr, rtvHandle);
		rtvHandle.ptr += g_RTVDescriptorSize;
	}

	// Create depth stencil
	D3D12_RESOURCE_DESC depthDesc = {};
	depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthDesc.Width = w;
	depthDesc.Height = h;
	depthDesc.DepthOrArraySize = 1;
	depthDesc.MipLevels = 1;
	depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	depthDesc.SampleDesc.Count = 1;
	depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE clearValue = {};
	clearValue.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	clearValue.DepthStencil.Depth = 1.0f;
	clearValue.DepthStencil.Stencil = 0;

	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

	hres = g_pDevice->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&depthDesc,
		D3D12_RESOURCE_STATE_DEPTH_WRITE,
		&clearValue,
		IID_PPV_ARGS(&g_pDepthStencil));
	if (FAILED(hres))
	{
		D3D12Log::GetPtr()->Printf("ERROR:  Could not create depth stencil!!");
		return JE_FALSE;
	}

	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
	dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	g_pDevice->CreateDepthStencilView(g_pDepthStencil.Get(), &dsvDesc,
		g_pDSVHeap->GetCPUDescriptorHandleForHeapStart());

	// Create command allocators and command list
	for (UINT i = 0; i < FRAME_COUNT; i++)
	{
		hres = g_pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
			IID_PPV_ARGS(&g_pCommandAllocators[i]));
		if (FAILED(hres))
		{
			D3D12Log::GetPtr()->Printf("ERROR:  Could not create command allocator!!");
			return JE_FALSE;
		}
	}

	hres = g_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
		g_pCommandAllocators[g_FrameIndex].Get(), nullptr, IID_PPV_ARGS(&g_pCommandList));
	if (FAILED(hres))
	{
		D3D12Log::GetPtr()->Printf("ERROR:  Could not create command list!!");
		return JE_FALSE;
	}
	g_pCommandList->Close();

	// Create fence
	hres = g_pDevice->CreateFence(g_FenceValues[g_FrameIndex], D3D12_FENCE_FLAG_NONE,
		IID_PPV_ARGS(&g_pFence));
	if (FAILED(hres))
	{
		D3D12Log::GetPtr()->Printf("ERROR:  Could not create fence!!");
		return JE_FALSE;
	}
	g_FenceValues[g_FrameIndex]++;

	g_FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (!g_FenceEvent)
	{
		D3D12Log::GetPtr()->Printf("ERROR:  Could not create fence event!!");
		return JE_FALSE;
	}

	// Set viewport and scissor rect
	g_Viewport.TopLeftX = 0;
	g_Viewport.TopLeftY = 0;
	g_Viewport.Width = static_cast<float>(w);
	g_Viewport.Height = static_cast<float>(h);
	g_Viewport.MinDepth = 0.0f;
	g_Viewport.MaxDepth = 1.0f;

	g_ScissorRect.left = 0;
	g_ScissorRect.top = 0;
	g_ScissorRect.right = w;
	g_ScissorRect.bottom = h;

	// Initialize texture manager
	D3D12_THandle_Startup();

	// Initialize poly cache
	g_pPolyCache = new D3D12PolyCache();
	if (!g_pPolyCache || !g_pPolyCache->Initialize(g_pDevice.Get(), g_pCommandQueue.Get()))
	{
		D3D12Log::GetPtr()->Printf("ERROR:  Could not initialize poly cache!!");
		return JE_FALSE;
	}

	// Initialize gamma tables
	g_LocalGamma = 1.0f;
	BuildRGBGammaTables(1.0f);

	// Initialize identity matrices
	DirectX::XMStoreFloat4x4(&g_WorldMatrix, DirectX::XMMatrixIdentity());
	DirectX::XMStoreFloat4x4(&g_ViewMatrix, DirectX::XMMatrixIdentity());
	DirectX::XMStoreFloat4x4(&g_ProjMatrix, DirectX::XMMatrixPerspectiveFovLH(
		DirectX::XM_PIDIV4, static_cast<float>(w) / static_cast<float>(h), 1.0f, 4000.0f));

	D3D12Log::GetPtr()->Printf("DEBUG:  D3D12 Initialization successful");
	return JE_TRUE;
}

jeBoolean DRIVERCC D3D12Drv_Shutdown(void)
{
	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  Shutdown()");
	else
		REPORT("Function Call:  Shutdown()");

	// Wait for GPU to finish
	WaitForGPU();

	// Cleanup poly cache
	if (g_pPolyCache)
	{
		g_pPolyCache->Shutdown();
		delete g_pPolyCache;
		g_pPolyCache = nullptr;
	}

	// Cleanup texture manager
	D3D12_THandle_Shutdown();

	// Cleanup fence event
	if (g_FenceEvent)
	{
		CloseHandle(g_FenceEvent);
		g_FenceEvent = nullptr;
	}

	// Release D3D12 objects (ComPtr handles release automatically)
	g_pCommandList.Reset();
	for (UINT i = 0; i < FRAME_COUNT; i++)
	{
		g_pCommandAllocators[i].Reset();
		g_pRenderTargets[i].Reset();
	}
	g_pDepthStencil.Reset();
	g_pFence.Reset();
	g_pSRVHeap.Reset();
	g_pDSVHeap.Reset();
	g_pRTVHeap.Reset();
	g_pSwapChain.Reset();
	g_pCommandQueue.Reset();
	g_pDevice.Reset();
	g_pFactory.Reset();

	D3D12Log::GetPtr()->Printf("Shutdown complete...");
	D3D12Log::GetPtr()->Shutdown();

	return JE_TRUE;
}

jeBoolean DRIVERCC D3D12Drv_EnumPixelFormats(DRV_ENUM_PFORMAT_CB* Cb, void* Context)
{
	int32 i;

	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  EnumPixelFormats()");
	else
		REPORT("Function Call:  EnumPixelFormats()");

	// D3D12 typically uses 32-bit formats
	g_PixelFormat[0].PixelFormat = JE_PIXELFORMAT_32BIT_ARGB;
	g_PixelFormat[0].Flags = RDRIVER_PF_3D | RDRIVER_PF_COMBINE_LIGHTMAP;

	g_PixelFormat[1].PixelFormat = JE_PIXELFORMAT_32BIT_ARGB;
	g_PixelFormat[1].Flags = RDRIVER_PF_3D | RDRIVER_PF_COMBINE_LIGHTMAP | RDRIVER_PF_ALPHA;

	g_PixelFormat[2].PixelFormat = JE_PIXELFORMAT_32BIT_ARGB;
	g_PixelFormat[2].Flags = RDRIVER_PF_2D | RDRIVER_PF_CAN_DO_COLORKEY;

	g_PixelFormat[3].PixelFormat = JE_PIXELFORMAT_32BIT_ARGB;
	g_PixelFormat[3].Flags = RDRIVER_PF_LIGHTMAP;

	g_PixelFormat[4].PixelFormat = JE_PIXELFORMAT_32BIT_ARGB;
	g_PixelFormat[4].Flags = RDRIVER_PF_3D | RDRIVER_PF_COMBINE_LIGHTMAP | RDRIVER_PF_ALPHA;

	for (i = 0; i < 5; i++)
	{
		if (!Cb(&g_PixelFormat[i], Context))
			return JE_TRUE;
	}

	return TRUE;
}

jeBoolean DRIVERCC D3D12Drv_GetDeviceCaps(jeDeviceCaps* DeviceCaps)
{
	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  GetDeviceCaps()");
	else
		REPORT("Function Call:  GetDeviceCaps()");

	DeviceCaps->SuggestedDefaultRenderFlags = JE_RENDER_FLAG_BILINEAR_FILTER;
	DeviceCaps->CanChangeRenderFlags = 0xFFFFFFFF;
	return JE_TRUE;
}

jeBoolean DRIVERCC D3D12Drv_SetGamma(float gamma)
{
	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  SetGamma()");
	else
		REPORT("Function Call:  SetGamma()");

	g_LocalGamma = gamma;
	BuildRGBGammaTables(gamma);
	return JE_TRUE;
}

jeBoolean DRIVERCC D3D12Drv_GetGamma(float* gamma)
{
	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  GetGamma()");
	else
		REPORT("Function Call:  GetGamma()");

	*gamma = g_LocalGamma;
	return JE_TRUE;
}

jeBoolean DRIVERCC D3D12Drv_Reset()
{
	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  Reset()");
	else
		REPORT("Function Call:  Reset()");

	WaitForGPU();

	D3D12_THandle_Shutdown();
	if (g_pPolyCache)
		g_pPolyCache->Shutdown();

	D3D12_THandle_Startup();
	if (g_pPolyCache)
		g_pPolyCache->Initialize(g_pDevice.Get(), g_pCommandQueue.Get());

	return TRUE;
}

jeBoolean DRIVERCC D3D12Drv_UpdateWindow()
{
	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  UpdateWindow()");
	else
		REPORT("Function Call:  UpdateWindow()");

	return TRUE;
}

jeBoolean DRIVERCC D3D12Drv_SetActive(jeBoolean Active)
{
	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  SetActive()");
	else
		REPORT("Function Call:  SetActive()");

	return TRUE;
}

jeBoolean DRIVERCC D3D12Drv_BeginScene(jeBoolean Clear, jeBoolean ClearZ, RECT* WorldRect, jeBoolean Wireframe)
{
	HRESULT hres;

	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  BeginScene()");
	else
		REPORT("Function Call:  BeginScene()");

	// Reset command allocator and command list
	hres = g_pCommandAllocators[g_FrameIndex]->Reset();
	if (FAILED(hres))
	{
		D3D12Log::GetPtr()->Printf("ERROR:  Could not reset command allocator!!");
		return JE_FALSE;
	}

	hres = g_pCommandList->Reset(g_pCommandAllocators[g_FrameIndex].Get(), g_pPipelineState.Get());
	if (FAILED(hres))
	{
		D3D12Log::GetPtr()->Printf("ERROR:  Could not reset command list!!");
		return JE_FALSE;
	}

	// Set viewport and scissor rect
	g_pCommandList->RSSetViewports(1, &g_Viewport);
	g_pCommandList->RSSetScissorRects(1, &g_ScissorRect);

	// Transition render target to render target state
	TransitionResource(g_pCommandList.Get(), g_pRenderTargets[g_FrameIndex].Get(),
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

	// Get RTV and DSV handles
	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_pRTVHeap->GetCPUDescriptorHandleForHeapStart();
	rtvHandle.ptr += g_FrameIndex * g_RTVDescriptorSize;
	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = g_pDSVHeap->GetCPUDescriptorHandleForHeapStart();

	// Set render targets
	g_pCommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

	// Clear if requested
	if (Clear)
	{
		g_D3D12Drv.NumRenderedPolys = 0;
		const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
		g_pCommandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
	}

	if (ClearZ)
	{
		g_pCommandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
	}

	// Set SRV heap
	ID3D12DescriptorHeap* ppHeaps[] = { g_pSRVHeap.Get() };
	g_pCommandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

	return JE_TRUE;
}

jeBoolean DRIVERCC D3D12Drv_EndScene()
{
	HRESULT hres;

	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  EndScene()");
	else
		REPORT("Function Call:  EndScene()");

	// Flush poly cache
	if (g_pPolyCache && !g_pPolyCache->Flush(g_pCommandList.Get()))
	{
		D3D12Log::GetPtr()->Printf("ERROR:  Failed to flush poly cache!!");
		return JE_FALSE;
	}

	// Transition render target to present state
	TransitionResource(g_pCommandList.Get(), g_pRenderTargets[g_FrameIndex].Get(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

	// Close command list
	hres = g_pCommandList->Close();
	if (FAILED(hres))
	{
		D3D12Log::GetPtr()->Printf("ERROR:  Could not close command list!!");
		return JE_FALSE;
	}

	// Execute command list
	ID3D12CommandList* ppCommandLists[] = { g_pCommandList.Get() };
	g_pCommandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

	// Present
	UINT syncInterval = 0; // Immediate presentation
	UINT presentFlags = g_IsWindowed ? DXGI_PRESENT_ALLOW_TEARING : 0;
	hres = g_pSwapChain->Present(syncInterval, presentFlags);
	if (FAILED(hres))
	{
		D3D12Log::GetPtr()->Printf("ERROR:  Could not present!!");
		return JE_FALSE;
	}

	MoveToNextFrame();

	return JE_TRUE;
}

jeBoolean DRIVERCC D3D12Drv_BeginBatch()
{
	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  BeginBatch()");
	else
		REPORT("Function Call:  BeginBatch()");

	return JE_TRUE;
}

jeBoolean DRIVERCC D3D12Drv_EndBatch()
{
	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  EndBatch()");
	else
		REPORT("Function Call:  EndBatch()");

	return JE_TRUE;
}

jeBoolean DRIVERCC D3D12Drv_RenderGouraudPoly(jeTLVertex* Pnts, int32 NumPoints, uint32 Flags)
{
	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  RenderGouraudPoly()");
	else
		REPORT("Function Call:  RenderGouraudPoly()");

	if (!g_pPolyCache)
	{
		D3D12Log::GetPtr()->Printf("ERROR:  Poly cache not initialized!!");
		return JE_FALSE;
	}

	return g_pPolyCache->AddGouraudPoly(Pnts, NumPoints, Flags);
}

jeBoolean DRIVERCC D3D12Drv_RenderWorldPoly(jeTLVertex* Pnts, int32 NumPoints, jeRDriver_Layer* Layers, int32 NumLayers, void* LMapCBContext, uint32 Flags)
{
	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  RenderWorldPoly()");
	else
		REPORT("Function Call:  RenderWorldPoly()");

	if (!g_pPolyCache)
	{
		D3D12Log::GetPtr()->Printf("ERROR:  Poly cache not initialized!!");
		return JE_FALSE;
	}

	return g_pPolyCache->AddWorldPoly(Pnts, NumPoints, Layers, NumLayers, LMapCBContext, Flags);
}

jeBoolean DRIVERCC D3D12Drv_RenderMiscTexturePoly(jeTLVertex* Pnts, int32 NumPoints, jeRDriver_Layer* Layers, int32 NumLayers, uint32 Flags)
{
	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  RenderMiscTexturePoly()");
	else
		REPORT("Function Call:  RenderMiscTexturePoly()");

	if (!g_pPolyCache)
	{
		D3D12Log::GetPtr()->Printf("ERROR:  Poly cache not initialized!!");
		return JE_FALSE;
	}

	return g_pPolyCache->AddMiscTexturePoly(Pnts, NumPoints, Layers, NumLayers, Flags);
}

jeBoolean DRIVERCC D3D12Drv_DrawDecal(jeTexture* Handle, RECT* SrcRect, int32 x, int32 y)
{
	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  DrawDecal()");
	else
		REPORT("Function Call:  DrawDecal()");

	// TODO: Implement sprite drawing for D3D12
	return JE_TRUE;
}

jeBoolean DRIVERCC D3D12Drv_Screenshot(const char* filename)
{
	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  Screenshot()");
	else
		REPORT("Function Call:  Screenshot()");

	// TODO: Implement screenshot for D3D12
	return JE_TRUE;
}

uint32 DRIVERCC D3D12Drv_CreateStaticMesh(jeHWVertex* Points, int32 NumPoints, jeRDriver_Layer* Layers, int32 NumLayers, uint32 Flags)
{
	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  CreateStaticMesh()");
	else
		REPORT("Function Call:  CreateStaticMesh()");

	if (!g_pPolyCache)
	{
		D3D12Log::GetPtr()->Printf("ERROR:  No poly cache!!");
		return 0;
	}

	return g_pPolyCache->AddStaticBuffer(Points, NumPoints, Layers, NumLayers, Flags);
}

jeBoolean DRIVERCC D3D12Drv_RemoveStaticMesh(uint32 id)
{
	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  RemoveStaticMesh()");
	else
		REPORT("Function Call:  RemoveStaticMesh()");

	if (!g_pPolyCache)
	{
		D3D12Log::GetPtr()->Printf("ERROR:  No poly cache!!");
		return JE_FALSE;
	}

	return g_pPolyCache->RemoveStaticBuffer(id);
}

jeBoolean DRIVERCC D3D12Drv_RenderStaticMesh(uint32 id, int32 StartVertex, int32 NumPolys, jeXForm3d* XForm)
{
	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  RenderStaticMesh()");
	else
		REPORT("Function Call:  RenderStaticMesh()");

	if (!g_pPolyCache)
	{
		D3D12Log::GetPtr()->Printf("ERROR:  No poly cache!!");
		return JE_FALSE;
	}

	return g_pPolyCache->RenderStaticBuffer(id, StartVertex, NumPolys, XForm);
}

jeBoolean DRIVERCC D3D12Drv_SetMatrix(uint32 Type, jeXForm3d* Matrix)
{
	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  SetMatrix()");
	else
		REPORT("Function Call:  SetMatrix()");

	DirectX::XMFLOAT4X4* pMatrix = nullptr;

	switch (Type)
	{
	case JE_XFORM_TYPE_WORLD:
		pMatrix = &g_WorldMatrix;
		break;
	case JE_XFORM_TYPE_VIEW:
		pMatrix = &g_ViewMatrix;
		break;
	case JE_XFORM_TYPE_PROJECTION:
		pMatrix = &g_ProjMatrix;
		break;
	default:
		return JE_FALSE;
	}

	jeXForm3d_ToD3DMatrix(Matrix, pMatrix);
	return JE_TRUE;
}

jeBoolean DRIVERCC D3D12Drv_GetMatrix(uint32 Type, jeXForm3d* Matrix)
{
	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  GetMatrix()");
	else
		REPORT("Function Call:  GetMatrix()");

	DirectX::XMFLOAT4X4* pMatrix = nullptr;

	switch (Type)
	{
	case JE_XFORM_TYPE_WORLD:
		pMatrix = &g_WorldMatrix;
		break;
	case JE_XFORM_TYPE_VIEW:
		pMatrix = &g_ViewMatrix;
		break;
	case JE_XFORM_TYPE_PROJECTION:
		pMatrix = &g_ProjMatrix;
		break;
	default:
		return JE_FALSE;
	}

	D3DMatrix_ToXForm3d(pMatrix, Matrix);
	return JE_TRUE;
}

jeBoolean DRIVERCC D3D12Drv_SetCamera(jeCamera* Camera)
{
	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  SetCamera()");
	else
		REPORT("Function Call:  SetCamera()");

	return JE_TRUE;
}

jeFont* DRIVERCC D3D12Drv_CreateFont(int32 Height, int32 Width, uint32 Weight, jeBoolean Italic, const char* facename)
{
	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  CreateFont()");
	else
		REPORT("Function Call:  CreateFont()");

	jeFont* font = new jeFont;
	if (!font)
	{
		D3D12Log::GetPtr()->Printf("ERROR:  Out of memory!!");
		return nullptr;
	}

	font->Height = Height;
	font->Width = Width;
	font->Weight = Weight;
	font->Italic = Italic;
	strncpy_s(font->FaceName, facename, sizeof(font->FaceName) - 1);

	// TODO: Implement font texture creation for D3D12
	return font;
}

jeBoolean DRIVERCC D3D12Drv_DrawFont(jeFont* Font, int32 x, int32 y, uint32 Color, const char* text)
{
	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  DrawFont()");

	// TODO: Implement font rendering for D3D12
	return JE_TRUE;
}

jeBoolean DRIVERCC D3D12Drv_DestroyFont(jeFont** Font)
{
	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  DestroyFont()");
	else
		REPORT("Function Call:  DestroyFont()");

	if (*Font)
	{
		delete (*Font);
		*Font = nullptr;
	}

	return JE_TRUE;
}

jeBoolean DRIVERCC D3D12Drv_SetRenderState(uint32 state, uint32 value)
{
	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  SetRenderState()");
	else
		REPORT("Function Call:  SetRenderState()");

	// In D3D12, render states are handled through PSO (Pipeline State Objects)
	// This function would need to trigger PSO changes or store state for next PSO creation
	// For now, we'll just acknowledge the call
	return JE_TRUE;
}

jeBoolean DRIVERCC D3D12Drv_DrawText(char* text, int x, int y, uint32 color)
{
	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  DrawText()");
	else
		REPORT("Function Call:  DrawText()");

	// TODO: Implement text rendering for D3D12
	return JE_TRUE;
}

D3D12Driver g_D3D12Drv = {
	"Direct3D 12 Driver.  Copyright 2024, Styx3D",
	DRV_VERSION_MAJOR,
	DRV_VERSION_MINOR,

	// Error handling hooks set by driver
	DRV_ERROR_NONE,
	NULL,

	// Enum Modes/Drivers
	D3D12Drv_EnumSubDrivers,
	D3D12Drv_EnumModes,

	D3D12Drv_EnumPixelFormats,

	// Device Caps
	D3D12Drv_GetDeviceCaps,

	// Init/DeInit functions
	D3D12Drv_Init,
	D3D12Drv_Shutdown,
	D3D12Drv_Reset,
	NULL,
	D3D12Drv_SetActive,

	// Create/Destroy texture functions
	D3D12_THandle_Create,
	D3D12_THandle_CreateFromFile,
	D3D12_THandle_Destroy,

	// Texture manipulation functions
	D3D12_THandle_Lock,
	D3D12_THandle_Unlock,

	// Palette access functions
	NULL,
	NULL,

	// Palette access functions
	NULL,
	NULL,

	D3D12_THandle_GetInfo,

	// Scene management functions
	D3D12Drv_BeginScene,
	D3D12Drv_EndScene,

	D3D12Drv_BeginBatch,
	D3D12Drv_EndBatch,

	// Render functions
	D3D12Drv_RenderGouraudPoly,
	D3D12Drv_RenderWorldPoly,
	D3D12Drv_RenderMiscTexturePoly,

	//Decal functions
	D3D12Drv_DrawDecal,

	0,
	0,
	0,

	NULL,

	D3D12Drv_Screenshot,

	D3D12Drv_SetGamma,
	D3D12Drv_GetGamma,

	// Hardware T&L
	D3D12Drv_SetMatrix,
	D3D12Drv_GetMatrix,
	NULL,

	NULL,
	NULL,

	D3D12Drv_DrawText,

	NULL,

	D3D12Drv_CreateStaticMesh,
	D3D12Drv_RemoveStaticMesh,
	D3D12Drv_RenderStaticMesh,

	D3D12Drv_CreateFont,
	D3D12Drv_DrawFont,
	D3D12Drv_DestroyFont,

	D3D12Drv_SetRenderState,
};

extern "C" DRIVERAPI BOOL DriverHook(DRV_Driver** Driver)
{
	if (LOG_LEVEL > 1)
		D3D12Log::GetPtr()->Printf("Function Call:  DriverHook()");

	D3D12_THandle_Startup();

	g_EngineSettings.CanSupportFlags = (DRV_SUPPORT_ALPHA | DRV_SUPPORT_COLORKEY);
	g_EngineSettings.PreferenceFlags = 0;

	g_D3D12Drv.EngineSettings = &g_EngineSettings;

	*Driver = &g_D3D12Drv;
	return TRUE;
}

// Log2 - Return the log of a size
uint32 Log2(uint32 P2)
{
	uint32 p = 0;
	int32 i = 0;

	for (i = P2; i > 0; i >>= 1)
		p++;

	return (p - 1);
}

// SnapToPower2 - Snaps a number to a power of 2
int32 SnapToPower2(int32 Width)
{
	if (Width <= 1) return 1;
	else if (Width <= 2) return 2;
	else if (Width <= 4) return 4;
	else if (Width <= 8) return 8;
	else if (Width <= 16) return 16;
	else if (Width <= 32) return 32;
	else if (Width <= 64) return 64;
	else if (Width <= 128) return 128;
	else if (Width <= 256) return 256;
	else if (Width <= 512) return 512;
	else if (Width <= 1024) return 1024;
	else if (Width <= 2048) return 2048;
	else if (Width <= 4096) return 4096;
	else
		return -1;
}

// Return the max log of a (power of 2) width and height
int32 GetLog(int32 Width, int32 Height)
{
	int32 LWidth = SnapToPower2(max(Width, Height));
	return Log2(LWidth);
}

void BuildRGBGammaTables(float Gamma)
{
	int32 i, Val;
	int32 GammaTable[256];
	DWORD R_Left, G_Left, B_Left;
	DWORD R_Right, G_Right, B_Right;

	if (Gamma == 1.0f)
	{
		for (i = 0; i < 256; i++)
			GammaTable[i] = i;
	}
	else
	{
		for (i = 0; i < 256; i++)
		{
			float Ratio = (i + 0.5f) / 255.5f;
			float RGB = (float)(255.0 * pow((double)Ratio, 1.0 / (double)Gamma) + 0.5);

			if (RGB < 0.0f)
				RGB = 0.0f;
			if (RGB > 255.0f)
				RGB = 255.0f;

			GammaTable[i] = (int32)RGB;
		}
	}

	for (i = 0; i < 256; i++)
	{
		Val = GammaTable[i];

		R_Left = 16;
		G_Left = 8;
		B_Left = 0;

		R_Right = 0;
		G_Right = 0;
		B_Right = 0;

		g_Lut1.R[i] = ((uint32)Val << R_Left) & 0x00FF0000;
		g_Lut1.G[i] = ((uint32)Val << G_Left) & 0x0000FF00;
		g_Lut1.B[i] = ((uint32)Val << B_Left) & 0x000000FF;
		g_Lut1.A[i] = ((uint32)i << 24) & 0xFF000000;
	}
}
