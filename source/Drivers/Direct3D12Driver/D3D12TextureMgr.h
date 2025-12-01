/****************************************************************************************/
/*  D3D12TEXTUREMGR.H                                                                   */
/*                                                                                      */
/*  Author: Styx3D Modernization                                                        */
/*  Description: Texture management for DirectX 12 driver                               */
/*                                                                                      */
/****************************************************************************************/
#ifndef D3D12TEXTUREMGR_H
#define D3D12TEXTUREMGR_H

#include "Direct3D12Driver.h"
#include <vector>
#include <memory>

class D3D12TextureMgr
{
private:
	static D3D12TextureMgr*			m_pInstance;
	std::vector<jeTexture*>			m_Textures;
	UINT							m_NextSRVIndex;
	bool							m_bInitialized;

	D3D12TextureMgr() : m_NextSRVIndex(0), m_bInitialized(false) {}

public:
	static D3D12TextureMgr* GetPtr()
	{
		if (!m_pInstance)
			m_pInstance = new D3D12TextureMgr();
		return m_pInstance;
	}

	bool Initialize();
	void Shutdown();

	jeTexture* CreateTexture(int32 Width, int32 Height, int32 NumMipLevels, const jeRDriver_PixelFormat* PixelFormat);
	jeTexture* CreateTextureFromFile(jeVFile* File);
	bool DestroyTexture(jeTexture* pTexture);

	bool LockTexture(jeTexture* pTexture, int32 MipLevel, void** ppData);
	bool UnlockTexture(jeTexture* pTexture, int32 MipLevel);

	bool GetTextureInfo(jeTexture* pTexture, int32 MipLevel, jeTexture_Info* pInfo);

	UINT GetNextSRVIndex() { return m_NextSRVIndex++; }
};

#endif // D3D12TEXTUREMGR_H
