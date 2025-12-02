/****************************************************************************************/
/*  DIRECT3D12DRIVER.H                                                                  */
/*                                                                                      */
/*  Author: Styx3D Modernization                                                        */
/*  Description: DirectX 12 rendering driver for Windows 11                             */
/*                                                                                      */
/*  The contents of this file are subject to the 0BSD License.                          */
/*  See LICENSE file in the project root for full license information.                  */
/*                                                                                      */
/****************************************************************************************/
#ifndef DIRECT3D12DRIVER_H
#define DIRECT3D12DRIVER_H

#define DRIVERAPI	_declspec(dllexport)
#define INITGUID

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <vector>
#include <queue>
#include <unordered_map>
#include "DCommon.h"

// Use Microsoft WRL ComPtr for COM object management
using Microsoft::WRL::ComPtr;

#define LOG_LEVEL								1
#define FRAME_COUNT								2  // Double buffering

#ifdef _DEBUG
#define REPORT(x)							OutputDebugStringA(x)
#else
#define REPORT(x)
#endif

// Forward declarations
struct D3D12Texture;
struct D3D12DescriptorHeap;
class D3D12TextureMgr;
class D3D12PolyCache;
class D3D12Log;

// Global D3D12 objects
extern HWND									g_hWnd;
extern ComPtr<ID3D12Device>					g_pDevice;
extern ComPtr<ID3D12CommandQueue>			g_pCommandQueue;
extern ComPtr<IDXGISwapChain3>				g_pSwapChain;
extern ComPtr<ID3D12DescriptorHeap>			g_pRTVHeap;
extern ComPtr<ID3D12DescriptorHeap>			g_pDSVHeap;
extern ComPtr<ID3D12DescriptorHeap>			g_pSRVHeap;
extern ComPtr<ID3D12Resource>				g_pRenderTargets[FRAME_COUNT];
extern ComPtr<ID3D12Resource>				g_pDepthStencil;
extern ComPtr<ID3D12CommandAllocator>		g_pCommandAllocators[FRAME_COUNT];
extern ComPtr<ID3D12GraphicsCommandList>	g_pCommandList;
extern ComPtr<ID3D12Fence>					g_pFence;
extern UINT64								g_FenceValues[FRAME_COUNT];
extern HANDLE								g_FenceEvent;
extern UINT									g_FrameIndex;
extern UINT									g_RTVDescriptorSize;
extern UINT									g_DSVDescriptorSize;
extern UINT									g_CBVSRVDescriptorSize;
extern float								g_LocalGamma;
extern D3D12_VIEWPORT						g_Viewport;
extern D3D12_RECT							g_ScissorRect;
extern bool									g_IsWindowed;

#define MAX_LAYERS							2
#define D3D12_HW_FVF						0 // Not used in D3D12, but kept for compatibility

#define SAFE_DELETE(x)						{ if (x) delete x; x = nullptr; }
#define SAFE_DELETE_ARRAY(x)				{ if (x) delete [] x; x = nullptr; }
#define SAFE_RELEASE(x)						{ if (x) { (x)->Release(); x = nullptr; } }

// D3D12 specific texture structure
struct jeTexture
{
	ComPtr<ID3D12Resource>	pResource;
	D3D12_CPU_DESCRIPTOR_HANDLE	SRVHandle;
	D3D12_CPU_DESCRIPTOR_HANDLE	RTVHandle;
	int32						Width;
	int32						Height;
	int32						stride;
	uint8						Log;
	jeRDriver_PixelFormat		PixelFormat;
	DXGI_FORMAT					Format;
	D3D12_RESOURCE_STATES		CurrentState;
};

// Font structure for D3D12
struct jeFont
{
	// In D3D12, we'll need to implement text rendering differently
	// For now, store basic font information
	int32		Height;
	int32		Width;
	uint32		Weight;
	jeBoolean	Italic;
	char		FaceName[64];
	ComPtr<ID3D12Resource>	pFontTexture;
};

// Gamma lookup tables
typedef struct RGB_LUT
{
	uint32				R[256];
	uint32				G[256];
	uint32				B[256];
	uint32				A[256];
} RGB_LUT;

extern RGB_LUT g_Lut1;

void BuildRGBGammaTables(float Gamma);

typedef DRV_Driver				D3D12Driver;

void									D3DMatrix_ToXForm3d(DirectX::XMFLOAT4X4* mat, jeXForm3d* XForm);
void									jeXForm3d_ToD3DMatrix(jeXForm3d* XForm, DirectX::XMFLOAT4X4* mat);

// Driver interface functions
jeBoolean								DRIVERCC D3D12Drv_EnumSubDrivers(DRV_ENUM_DRV_CB* Cb, void* Context);
jeBoolean								DRIVERCC D3D12Drv_EnumModes(S32 Driver, char* DriverName, DRV_ENUM_MODES_CB* Cb, void* Context);
jeBoolean								DRIVERCC D3D12Drv_EnumPixelFormats(DRV_ENUM_PFORMAT_CB* Cb, void* Context);

jeBoolean								DRIVERCC D3D12Drv_GetDeviceCaps(jeDeviceCaps* DeviceCaps);

jeBoolean								DRIVERCC D3D12Drv_Init(DRV_DriverHook* hook);
jeBoolean								DRIVERCC D3D12Drv_Shutdown(void);
jeBoolean								DRIVERCC D3D12Drv_Reset(void);
jeBoolean								DRIVERCC D3D12Drv_UpdateWindow(void);
jeBoolean								DRIVERCC D3D12Drv_SetActive(jeBoolean Active);

jeBoolean								DRIVERCC D3D12Drv_BeginScene(jeBoolean Clear, jeBoolean ClearZ, RECT* WorldRect, jeBoolean Wireframe);
jeBoolean								DRIVERCC D3D12Drv_EndScene(void);
jeBoolean								DRIVERCC D3D12Drv_BeginBatch(void);
jeBoolean								DRIVERCC D3D12Drv_EndBatch(void);

jeBoolean								DRIVERCC D3D12Drv_RenderGouraudPoly(jeTLVertex* Pnts, int32 NumPoints, uint32 Flags);
jeBoolean								DRIVERCC D3D12Drv_RenderWorldPoly(jeTLVertex* Pnts, int32 NumPoints, jeRDriver_Layer* Layers, int32 NumLayers, void* LMapCBContext, uint32 Flags);
jeBoolean								DRIVERCC D3D12Drv_RenderMiscTexturePoly(jeTLVertex* Pnts, int32 NumPoints, jeRDriver_Layer* Layers, int32 NumLayers, uint32 Flags);

jeBoolean								DRIVERCC D3D12Drv_DrawDecal(jeTexture* Handle, RECT* SrcRect, int32 x, int32 y);

jeBoolean								DRIVERCC D3D12Drv_Screenshot(const char* filename);

jeBoolean								DRIVERCC D3D12Drv_GetGamma(float* gamma);
jeBoolean								DRIVERCC D3D12Drv_SetGamma(float gamma);

jeBoolean								DRIVERCC D3D12Drv_SetMatrix(uint32 Type, jeXForm3d* Matrix);
jeBoolean								DRIVERCC D3D12Drv_GetMatrix(uint32 Type, jeXForm3d* Matrix);

uint32									DRIVERCC D3D12Drv_CreateStaticMesh(jeHWVertex* Points, int32 NumPoints, jeRDriver_Layer* Layers, int32 NumLayers, uint32 Flags);
jeBoolean								DRIVERCC D3D12Drv_RemoveStaticMesh(uint32 id);
jeBoolean								DRIVERCC D3D12Drv_RenderStaticMesh(uint32 id, int32 StartVertex, int32 NumPolys, jeXForm3d* XForm);

jeFont*									DRIVERCC D3D12Drv_CreateFont(int32 Height, int32 Width, uint32 Weight, jeBoolean Italic, const char* facename);
jeBoolean								DRIVERCC D3D12Drv_DrawFont(jeFont* Font, int32 x, int32 y, uint32 Color, const char* text);
jeBoolean								DRIVERCC D3D12Drv_DestroyFont(jeFont** Font);

jeBoolean								DRIVERCC D3D12Drv_SetRenderState(uint32 state, uint32 value);
jeBoolean								DRIVERCC D3D12Drv_DrawText(char* text, int x, int y, uint32 color);

// Texture management functions
jeTexture*								DRIVERCC D3D12_THandle_Create(int32 Width, int32 Height, int32 NumMipLevels, const jeRDriver_PixelFormat* PixelFormat);
jeTexture*								DRIVERCC D3D12_THandle_CreateFromFile(jeVFile* File);
jeBoolean								DRIVERCC D3D12_THandle_Destroy(jeTexture* THandle);
jeBoolean								DRIVERCC D3D12_THandle_Lock(jeTexture* THandle, int32 MipLevel, void** Data);
jeBoolean								DRIVERCC D3D12_THandle_Unlock(jeTexture* THandle, int32 MipLevel);
jeBoolean								DRIVERCC D3D12_THandle_GetInfo(jeTexture* THandle, int32 MipLevel, jeTexture_Info* Info);
jeBoolean								D3D12_THandle_Startup(void);
jeBoolean								D3D12_THandle_Shutdown(void);

extern "C" DRIVERAPI D3D12Driver			g_D3D12Drv;

int32 GetLog(int32 Width, int32 Height);

// Helper functions for D3D12
void WaitForGPU();
void MoveToNextFrame();
void TransitionResource(ID3D12GraphicsCommandList* pCmdList, ID3D12Resource* pResource,
	D3D12_RESOURCE_STATES stateBefore, D3D12_RESOURCE_STATES stateAfter);

#endif //DIRECT3D12DRIVER_H
