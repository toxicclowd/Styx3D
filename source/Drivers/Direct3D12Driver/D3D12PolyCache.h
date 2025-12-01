/****************************************************************************************/
/*  D3D12POLYCACHE.H                                                                    */
/*                                                                                      */
/*  Author: Styx3D Modernization                                                        */
/*  Description: Polygon caching for DirectX 12 driver                                  */
/*                                                                                      */
/****************************************************************************************/
#ifndef D3D12POLYCACHE_H
#define D3D12POLYCACHE_H

#include "Direct3D12Driver.h"
#include <vector>
#include <unordered_map>

// Vertex structure for D3D12
struct D3D12Vertex
{
	float x, y, z;
	float r, g, b, a;
	float u, v;
	float u2, v2;  // For lightmap
};

// Static mesh buffer
struct D3D12StaticBuffer
{
	ComPtr<ID3D12Resource>	pVertexBuffer;
	D3D12_VERTEX_BUFFER_VIEW VBView;
	int32					NumVertices;
	jeRDriver_Layer			Layers[MAX_LAYERS];
	int32					NumLayers;
	uint32					Flags;
	uint32					ID;
};

class D3D12PolyCache
{
private:
	ID3D12Device*						m_pDevice;
	ID3D12CommandQueue*					m_pCommandQueue;
	
	// Dynamic vertex buffer for batching
	ComPtr<ID3D12Resource>				m_pDynamicVB;
	D3D12_VERTEX_BUFFER_VIEW			m_DynamicVBView;
	std::vector<D3D12Vertex>			m_Vertices;
	
	// Static buffers
	std::unordered_map<uint32, D3D12StaticBuffer*>	m_StaticBuffers;
	uint32									m_NextStaticID;
	
	// Root signature and pipeline state
	ComPtr<ID3D12RootSignature>			m_pRootSignature;
	ComPtr<ID3D12PipelineState>			m_pPipelineState;
	ComPtr<ID3D12PipelineState>			m_pWireframePSO;
	
	bool								m_bInitialized;

	bool CreateRootSignature();
	bool CreatePipelineStates();

public:
	D3D12PolyCache();
	~D3D12PolyCache();

	bool Initialize(ID3D12Device* pDevice, ID3D12CommandQueue* pCommandQueue);
	void Shutdown();

	bool AddGouraudPoly(jeTLVertex* Pnts, int32 NumPoints, uint32 Flags);
	bool AddWorldPoly(jeTLVertex* Pnts, int32 NumPoints, jeRDriver_Layer* Layers, int32 NumLayers, void* LMapCBContext, uint32 Flags);
	bool AddMiscTexturePoly(jeTLVertex* Pnts, int32 NumPoints, jeRDriver_Layer* Layers, int32 NumLayers, uint32 Flags);

	uint32 AddStaticBuffer(jeHWVertex* Points, int32 NumPoints, jeRDriver_Layer* Layers, int32 NumLayers, uint32 Flags);
	bool RemoveStaticBuffer(uint32 id);
	bool RenderStaticBuffer(uint32 id, int32 StartVertex, int32 NumPolys, jeXForm3d* XForm);

	bool Flush(ID3D12GraphicsCommandList* pCmdList);
};

#endif // D3D12POLYCACHE_H
