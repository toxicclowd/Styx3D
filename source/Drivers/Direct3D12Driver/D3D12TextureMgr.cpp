/****************************************************************************************/
/*  D3D12TEXTUREMGR.CPP                                                                 */
/*                                                                                      */
/*  Author: Styx3D Modernization                                                        */
/*  Description: Texture management implementation for DirectX 12 driver                */
/*                                                                                      */
/****************************************************************************************/
#include "D3D12TextureMgr.h"
#include "D3D12Log.h"
#include "pixelformat.h"

D3D12TextureMgr* D3D12TextureMgr::m_pInstance = nullptr;

// Helper to get DXGI format from pixel format
static DXGI_FORMAT GetTextureFormat(jePixelFormat format)
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

bool D3D12TextureMgr::Initialize()
{
	if (m_bInitialized)
		return true;

	m_Textures.clear();
	m_NextSRVIndex = 0;
	m_bInitialized = true;

	D3D12Log::GetPtr()->Printf("D3D12TextureMgr initialized");
	return true;
}

void D3D12TextureMgr::Shutdown()
{
	// Release all textures
	for (auto pTex : m_Textures)
	{
		if (pTex)
		{
			pTex->pResource.Reset();
			delete pTex;
		}
	}
	m_Textures.clear();
	m_NextSRVIndex = 0;
	m_bInitialized = false;

	D3D12Log::GetPtr()->Printf("D3D12TextureMgr shutdown");
}

jeTexture* D3D12TextureMgr::CreateTexture(int32 Width, int32 Height, int32 NumMipLevels, const jeRDriver_PixelFormat* PixelFormat)
{
	if (!m_bInitialized || !g_pDevice)
		return nullptr;

	jeTexture* pTexture = new jeTexture();
	if (!pTexture)
		return nullptr;

	// Snap to power of 2
	int32 snapWidth = Width;
	int32 snapHeight = Height;
	if (snapWidth <= 1) snapWidth = 1;
	else if (snapWidth <= 2) snapWidth = 2;
	else if (snapWidth <= 4) snapWidth = 4;
	else if (snapWidth <= 8) snapWidth = 8;
	else if (snapWidth <= 16) snapWidth = 16;
	else if (snapWidth <= 32) snapWidth = 32;
	else if (snapWidth <= 64) snapWidth = 64;
	else if (snapWidth <= 128) snapWidth = 128;
	else if (snapWidth <= 256) snapWidth = 256;
	else if (snapWidth <= 512) snapWidth = 512;
	else if (snapWidth <= 1024) snapWidth = 1024;
	else if (snapWidth <= 2048) snapWidth = 2048;
	else snapWidth = 4096;

	if (snapHeight <= 1) snapHeight = 1;
	else if (snapHeight <= 2) snapHeight = 2;
	else if (snapHeight <= 4) snapHeight = 4;
	else if (snapHeight <= 8) snapHeight = 8;
	else if (snapHeight <= 16) snapHeight = 16;
	else if (snapHeight <= 32) snapHeight = 32;
	else if (snapHeight <= 64) snapHeight = 64;
	else if (snapHeight <= 128) snapHeight = 128;
	else if (snapHeight <= 256) snapHeight = 256;
	else if (snapHeight <= 512) snapHeight = 512;
	else if (snapHeight <= 1024) snapHeight = 1024;
	else if (snapHeight <= 2048) snapHeight = 2048;
	else snapHeight = 4096;

	pTexture->Width = snapWidth;
	pTexture->Height = snapHeight;
	pTexture->stride = snapWidth;
	pTexture->PixelFormat = *PixelFormat;
	pTexture->Format = GetTextureFormat(PixelFormat->PixelFormat);
	pTexture->Log = static_cast<uint8>(GetLog(snapWidth, snapHeight));
	pTexture->CurrentState = D3D12_RESOURCE_STATE_COPY_DEST;

	// Create texture resource
	D3D12_RESOURCE_DESC texDesc = {};
	texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	texDesc.Width = snapWidth;
	texDesc.Height = snapHeight;
	texDesc.DepthOrArraySize = 1;
	texDesc.MipLevels = (NumMipLevels > 0) ? NumMipLevels : 1;
	texDesc.Format = pTexture->Format;
	texDesc.SampleDesc.Count = 1;
	texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

	HRESULT hr = g_pDevice->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&texDesc,
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		IID_PPV_ARGS(&pTexture->pResource));

	if (FAILED(hr))
	{
		D3D12Log::GetPtr()->Printf("ERROR: Failed to create texture resource");
		delete pTexture;
		return nullptr;
	}

	// Create SRV
	UINT srvIndex = GetNextSRVIndex();
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = pTexture->Format;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MipLevels = texDesc.MipLevels;

	pTexture->SRVHandle = g_pSRVHeap->GetCPUDescriptorHandleForHeapStart();
	pTexture->SRVHandle.ptr += srvIndex * g_CBVSRVDescriptorSize;

	g_pDevice->CreateShaderResourceView(pTexture->pResource.Get(), &srvDesc, pTexture->SRVHandle);

	m_Textures.push_back(pTexture);
	return pTexture;
}

jeTexture* D3D12TextureMgr::CreateTextureFromFile(jeVFile* File)
{
	// TODO: Implement texture loading from file
	return nullptr;
}

bool D3D12TextureMgr::DestroyTexture(jeTexture* pTexture)
{
	if (!pTexture)
		return false;

	// Remove from list
	for (auto it = m_Textures.begin(); it != m_Textures.end(); ++it)
	{
		if (*it == pTexture)
		{
			m_Textures.erase(it);
			break;
		}
	}

	pTexture->pResource.Reset();
	delete pTexture;
	return true;
}

bool D3D12TextureMgr::LockTexture(jeTexture* pTexture, int32 MipLevel, void** ppData)
{
	// In D3D12, we need to use upload heaps and copy operations
	// For now, return a staging buffer
	// TODO: Implement proper texture locking with upload heap
	if (!pTexture)
		return false;

	// Allocate a temporary buffer for CPU access
	size_t bpp = 4; // Assuming 32-bit textures
	size_t bufferSize = pTexture->Width * pTexture->Height * bpp;
	*ppData = malloc(bufferSize);
	
	if (!*ppData)
		return false;

	memset(*ppData, 0, bufferSize);
	return true;
}

bool D3D12TextureMgr::UnlockTexture(jeTexture* pTexture, int32 MipLevel)
{
	// TODO: Copy data from staging buffer to GPU texture
	// For now, just return success
	return true;
}

bool D3D12TextureMgr::GetTextureInfo(jeTexture* pTexture, int32 MipLevel, jeTexture_Info* pInfo)
{
	if (!pTexture || !pInfo)
		return false;

	pInfo->Width = pTexture->Width >> MipLevel;
	pInfo->Height = pTexture->Height >> MipLevel;
	pInfo->Stride = pInfo->Width;
	pInfo->ColorKey = 0;
	pInfo->Flags = 0;
	pInfo->Log = pTexture->Log - MipLevel;
	pInfo->PixelFormat = pTexture->PixelFormat;
	pInfo->Direct = pTexture->pResource.Get();

	return true;
}

// Global texture management functions
jeBoolean D3D12_THandle_Startup(void)
{
	return D3D12TextureMgr::GetPtr()->Initialize() ? JE_TRUE : JE_FALSE;
}

jeBoolean D3D12_THandle_Shutdown(void)
{
	D3D12TextureMgr::GetPtr()->Shutdown();
	return JE_TRUE;
}

jeTexture* DRIVERCC D3D12_THandle_Create(int32 Width, int32 Height, int32 NumMipLevels, const jeRDriver_PixelFormat* PixelFormat)
{
	return D3D12TextureMgr::GetPtr()->CreateTexture(Width, Height, NumMipLevels, PixelFormat);
}

jeTexture* DRIVERCC D3D12_THandle_CreateFromFile(jeVFile* File)
{
	return D3D12TextureMgr::GetPtr()->CreateTextureFromFile(File);
}

jeBoolean DRIVERCC D3D12_THandle_Destroy(jeTexture* THandle)
{
	return D3D12TextureMgr::GetPtr()->DestroyTexture(THandle) ? JE_TRUE : JE_FALSE;
}

jeBoolean DRIVERCC D3D12_THandle_Lock(jeTexture* THandle, int32 MipLevel, void** Data)
{
	return D3D12TextureMgr::GetPtr()->LockTexture(THandle, MipLevel, Data) ? JE_TRUE : JE_FALSE;
}

jeBoolean DRIVERCC D3D12_THandle_Unlock(jeTexture* THandle, int32 MipLevel)
{
	return D3D12TextureMgr::GetPtr()->UnlockTexture(THandle, MipLevel) ? JE_TRUE : JE_FALSE;
}

jeBoolean DRIVERCC D3D12_THandle_GetInfo(jeTexture* THandle, int32 MipLevel, jeTexture_Info* Info)
{
	return D3D12TextureMgr::GetPtr()->GetTextureInfo(THandle, MipLevel, Info) ? JE_TRUE : JE_FALSE;
}
