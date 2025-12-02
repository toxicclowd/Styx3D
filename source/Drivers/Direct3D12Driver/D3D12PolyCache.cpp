/****************************************************************************************/
/*  D3D12POLYCACHE.CPP                                                                  */
/*                                                                                      */
/*  Author: Styx3D Modernization                                                        */
/*  Description: Polygon caching implementation for DirectX 12 driver                   */
/*                                                                                      */
/****************************************************************************************/
#include "D3D12PolyCache.h"
#include "D3D12Log.h"

// Basic vertex shader
static const char* g_VertexShaderCode = R"(
cbuffer ConstantBuffer : register(b0)
{
    float4x4 WorldViewProj;
};

struct VSInput
{
    float3 position : POSITION;
    float4 color : COLOR;
    float2 texcoord : TEXCOORD0;
    float2 texcoord2 : TEXCOORD1;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
    float2 texcoord : TEXCOORD0;
    float2 texcoord2 : TEXCOORD1;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.position = mul(float4(input.position, 1.0f), WorldViewProj);
    output.color = input.color;
    output.texcoord = input.texcoord;
    output.texcoord2 = input.texcoord2;
    return output;
}
)";

// Basic pixel shader
static const char* g_PixelShaderCode = R"(
Texture2D g_texture : register(t0);
SamplerState g_sampler : register(s0);

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
    float2 texcoord : TEXCOORD0;
    float2 texcoord2 : TEXCOORD1;
};

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 texColor = g_texture.Sample(g_sampler, input.texcoord);
    return texColor * input.color;
}
)";

// Color-only pixel shader
static const char* g_ColorPixelShaderCode = R"(
struct PSInput
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
    float2 texcoord : TEXCOORD0;
    float2 texcoord2 : TEXCOORD1;
};

float4 PSColorMain(PSInput input) : SV_TARGET
{
    return input.color;
}
)";

D3D12PolyCache::D3D12PolyCache()
	: m_pDevice(nullptr)
	, m_pCommandQueue(nullptr)
	, m_NextStaticID(1)  // Start at 1 so 0 can be used as an invalid ID sentinel
	, m_bInitialized(false)
{
}

D3D12PolyCache::~D3D12PolyCache()
{
	Shutdown();
}

bool D3D12PolyCache::CreateRootSignature()
{
	// Create a simple root signature with one constant buffer and one texture
	D3D12_DESCRIPTOR_RANGE1 ranges[2] = {};
	
	// Constant buffer
	ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
	ranges[0].NumDescriptors = 1;
	ranges[0].BaseShaderRegister = 0;
	ranges[0].RegisterSpace = 0;
	ranges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
	ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	// Texture
	ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[1].NumDescriptors = 1;
	ranges[1].BaseShaderRegister = 0;
	ranges[1].RegisterSpace = 0;
	ranges[1].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
	ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	D3D12_ROOT_PARAMETER1 rootParams[2] = {};
	
	// CBV
	rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rootParams[0].DescriptorTable.NumDescriptorRanges = 1;
	rootParams[0].DescriptorTable.pDescriptorRanges = &ranges[0];
	rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

	// SRV
	rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
	rootParams[1].DescriptorTable.pDescriptorRanges = &ranges[1];
	rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	// Static sampler
	D3D12_STATIC_SAMPLER_DESC sampler = {};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.MipLODBias = 0;
	sampler.MaxAnisotropy = 16;
	sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
	sampler.MinLOD = 0.0f;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderRegister = 0;
	sampler.RegisterSpace = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc = {};
	rootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
	rootSigDesc.Desc_1_1.NumParameters = 2;
	rootSigDesc.Desc_1_1.pParameters = rootParams;
	rootSigDesc.Desc_1_1.NumStaticSamplers = 1;
	rootSigDesc.Desc_1_1.pStaticSamplers = &sampler;
	rootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	ComPtr<ID3DBlob> signature;
	ComPtr<ID3DBlob> error;
	HRESULT hr = D3D12SerializeVersionedRootSignature(&rootSigDesc, &signature, &error);
	if (FAILED(hr))
	{
		if (error)
		{
			D3D12Log::GetPtr()->Printf("ERROR: Root signature serialization failed: %s",
				(char*)error->GetBufferPointer());
		}
		return false;
	}

	hr = m_pDevice->CreateRootSignature(0, signature->GetBufferPointer(),
		signature->GetBufferSize(), IID_PPV_ARGS(&m_pRootSignature));
	if (FAILED(hr))
	{
		D3D12Log::GetPtr()->Printf("ERROR: Failed to create root signature");
		return false;
	}

	return true;
}

bool D3D12PolyCache::CreatePipelineStates()
{
	// Compile shaders
	ComPtr<ID3DBlob> vertexShader;
	ComPtr<ID3DBlob> pixelShader;
	ComPtr<ID3DBlob> colorPixelShader;
	ComPtr<ID3DBlob> error;

#ifdef _DEBUG
	UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
	UINT compileFlags = 0;
#endif

	HRESULT hr = D3DCompile(g_VertexShaderCode, strlen(g_VertexShaderCode), nullptr, nullptr,
		nullptr, "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, &error);
	if (FAILED(hr))
	{
		if (error)
		{
			D3D12Log::GetPtr()->Printf("ERROR: Vertex shader compilation failed: %s",
				(char*)error->GetBufferPointer());
		}
		return false;
	}

	hr = D3DCompile(g_ColorPixelShaderCode, strlen(g_ColorPixelShaderCode), nullptr, nullptr,
		nullptr, "PSColorMain", "ps_5_0", compileFlags, 0, &colorPixelShader, &error);
	if (FAILED(hr))
	{
		if (error)
		{
			D3D12Log::GetPtr()->Printf("ERROR: Pixel shader compilation failed: %s",
				(char*)error->GetBufferPointer());
		}
		return false;
	}

	// Define input layout
	D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	// Create pipeline state
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
	psoDesc.pRootSignature = m_pRootSignature.Get();
	psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
	psoDesc.PS = { colorPixelShader->GetBufferPointer(), colorPixelShader->GetBufferSize() };
	psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
	psoDesc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
	psoDesc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
	psoDesc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
	psoDesc.RasterizerState.DepthClipEnable = TRUE;
	psoDesc.RasterizerState.MultisampleEnable = FALSE;
	psoDesc.RasterizerState.AntialiasedLineEnable = FALSE;
	psoDesc.RasterizerState.ForcedSampleCount = 0;
	psoDesc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
	psoDesc.BlendState.AlphaToCoverageEnable = FALSE;
	psoDesc.BlendState.IndependentBlendEnable = FALSE;
	psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
	psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
	psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
	psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	psoDesc.DepthStencilState.DepthEnable = TRUE;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
	psoDesc.DepthStencilState.StencilEnable = FALSE;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
	psoDesc.SampleDesc.Count = 1;

	hr = m_pDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pPipelineState));
	if (FAILED(hr))
	{
		D3D12Log::GetPtr()->Printf("ERROR: Failed to create pipeline state");
		return false;
	}

	// Create wireframe PSO
	psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
	hr = m_pDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pWireframePSO));
	if (FAILED(hr))
	{
		D3D12Log::GetPtr()->Printf("ERROR: Failed to create wireframe pipeline state");
		return false;
	}

	return true;
}

bool D3D12PolyCache::Initialize(ID3D12Device* pDevice, ID3D12CommandQueue* pCommandQueue)
{
	if (m_bInitialized)
		return true;

	m_pDevice = pDevice;
	m_pCommandQueue = pCommandQueue;

	if (!CreateRootSignature())
	{
		D3D12Log::GetPtr()->Printf("ERROR: Failed to create root signature");
		return false;
	}

	if (!CreatePipelineStates())
	{
		D3D12Log::GetPtr()->Printf("ERROR: Failed to create pipeline states");
		return false;
	}

	// Reserve space for vertices
	m_Vertices.reserve(10000);

	m_bInitialized = true;
	D3D12Log::GetPtr()->Printf("D3D12PolyCache initialized");
	return true;
}

void D3D12PolyCache::Shutdown()
{
	// Clear static buffers
	for (auto& pair : m_StaticBuffers)
	{
		if (pair.second)
		{
			pair.second->pVertexBuffer.Reset();
			delete pair.second;
		}
	}
	m_StaticBuffers.clear();

	m_Vertices.clear();
	m_pDynamicVB.Reset();
	m_pPipelineState.Reset();
	m_pWireframePSO.Reset();
	m_pRootSignature.Reset();

	m_pDevice = nullptr;
	m_pCommandQueue = nullptr;
	m_bInitialized = false;

	D3D12Log::GetPtr()->Printf("D3D12PolyCache shutdown");
}

bool D3D12PolyCache::AddGouraudPoly(jeTLVertex* Pnts, int32 NumPoints, uint32 Flags)
{
	if (!m_bInitialized || NumPoints < 3)
		return false;

	// Convert triangle fan to triangles
	for (int32 i = 2; i < NumPoints; i++)
	{
		D3D12Vertex v0, v1, v2;

		// First vertex
		v0.x = Pnts[0].x;
		v0.y = Pnts[0].y;
		v0.z = Pnts[0].z;
		v0.r = Pnts[0].r;
		v0.g = Pnts[0].g;
		v0.b = Pnts[0].b;
		v0.a = Pnts[0].a;
		v0.u = Pnts[0].u;
		v0.v = Pnts[0].v;
		v0.u2 = 0.0f;
		v0.v2 = 0.0f;

		// Second vertex
		v1.x = Pnts[i - 1].x;
		v1.y = Pnts[i - 1].y;
		v1.z = Pnts[i - 1].z;
		v1.r = Pnts[i - 1].r;
		v1.g = Pnts[i - 1].g;
		v1.b = Pnts[i - 1].b;
		v1.a = Pnts[i - 1].a;
		v1.u = Pnts[i - 1].u;
		v1.v = Pnts[i - 1].v;
		v1.u2 = 0.0f;
		v1.v2 = 0.0f;

		// Third vertex
		v2.x = Pnts[i].x;
		v2.y = Pnts[i].y;
		v2.z = Pnts[i].z;
		v2.r = Pnts[i].r;
		v2.g = Pnts[i].g;
		v2.b = Pnts[i].b;
		v2.a = Pnts[i].a;
		v2.u = Pnts[i].u;
		v2.v = Pnts[i].v;
		v2.u2 = 0.0f;
		v2.v2 = 0.0f;

		m_Vertices.push_back(v0);
		m_Vertices.push_back(v1);
		m_Vertices.push_back(v2);
	}

	return true;
}

bool D3D12PolyCache::AddWorldPoly(jeTLVertex* Pnts, int32 NumPoints, jeRDriver_Layer* Layers, int32 NumLayers, void* LMapCBContext, uint32 Flags)
{
	return AddGouraudPoly(Pnts, NumPoints, Flags);
}

bool D3D12PolyCache::AddMiscTexturePoly(jeTLVertex* Pnts, int32 NumPoints, jeRDriver_Layer* Layers, int32 NumLayers, uint32 Flags)
{
	return AddGouraudPoly(Pnts, NumPoints, Flags);
}

uint32 D3D12PolyCache::AddStaticBuffer(jeHWVertex* Points, int32 NumPoints, jeRDriver_Layer* Layers, int32 NumLayers, uint32 Flags)
{
	if (!m_bInitialized || !Points || NumPoints <= 0)
		return 0;

	D3D12StaticBuffer* pBuffer = new D3D12StaticBuffer();
	if (!pBuffer)
		return 0;

	pBuffer->ID = m_NextStaticID++;
	pBuffer->NumVertices = NumPoints;
	pBuffer->Flags = Flags;
	pBuffer->NumLayers = NumLayers;

	for (int32 i = 0; i < NumLayers && i < MAX_LAYERS; i++)
	{
		pBuffer->Layers[i] = Layers[i];
	}

	// Create vertex buffer
	size_t bufferSize = sizeof(D3D12Vertex) * NumPoints;
	std::vector<D3D12Vertex> vertices(NumPoints);

	for (int32 i = 0; i < NumPoints; i++)
	{
		vertices[i].x = Points[i].Pos.X;
		vertices[i].y = Points[i].Pos.Y;
		vertices[i].z = Points[i].Pos.Z;
		// Extract ARGB components from Diffuse uint32
		vertices[i].a = ((Points[i].Diffuse >> 24) & 0xFF) / 255.0f;
		vertices[i].r = ((Points[i].Diffuse >> 16) & 0xFF) / 255.0f;
		vertices[i].g = ((Points[i].Diffuse >> 8) & 0xFF) / 255.0f;
		vertices[i].b = (Points[i].Diffuse & 0xFF) / 255.0f;
		vertices[i].u = Points[i].u;
		vertices[i].v = Points[i].v;
		vertices[i].u2 = Points[i].lu;
		vertices[i].v2 = Points[i].lv;
	}

	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

	D3D12_RESOURCE_DESC bufferDesc = {};
	bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufferDesc.Width = bufferSize;
	bufferDesc.Height = 1;
	bufferDesc.DepthOrArraySize = 1;
	bufferDesc.MipLevels = 1;
	bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
	bufferDesc.SampleDesc.Count = 1;
	bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	HRESULT hr = m_pDevice->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&bufferDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&pBuffer->pVertexBuffer));

	if (FAILED(hr))
	{
		delete pBuffer;
		return 0;
	}

	// Copy data to buffer
	void* pData = nullptr;
	D3D12_RANGE readRange = { 0, 0 };
	hr = pBuffer->pVertexBuffer->Map(0, &readRange, &pData);
	if (SUCCEEDED(hr))
	{
		memcpy(pData, vertices.data(), bufferSize);
		pBuffer->pVertexBuffer->Unmap(0, nullptr);
	}

	// Set up vertex buffer view
	pBuffer->VBView.BufferLocation = pBuffer->pVertexBuffer->GetGPUVirtualAddress();
	pBuffer->VBView.StrideInBytes = sizeof(D3D12Vertex);
	pBuffer->VBView.SizeInBytes = static_cast<UINT>(bufferSize);

	m_StaticBuffers[pBuffer->ID] = pBuffer;
	return pBuffer->ID;
}

bool D3D12PolyCache::RemoveStaticBuffer(uint32 id)
{
	auto it = m_StaticBuffers.find(id);
	if (it == m_StaticBuffers.end())
		return false;

	if (it->second)
	{
		it->second->pVertexBuffer.Reset();
		delete it->second;
	}
	m_StaticBuffers.erase(it);
	return true;
}

bool D3D12PolyCache::RenderStaticBuffer(uint32 id, int32 StartVertex, int32 NumPolys, jeXForm3d* XForm)
{
	auto it = m_StaticBuffers.find(id);
	if (it == m_StaticBuffers.end() || !it->second)
		return false;

	// TODO: Implement static buffer rendering
	return true;
}

bool D3D12PolyCache::Flush(ID3D12GraphicsCommandList* pCmdList)
{
	if (!m_bInitialized || m_Vertices.empty() || !pCmdList)
		return true; // Nothing to flush

	// Create or update dynamic vertex buffer
	size_t bufferSize = sizeof(D3D12Vertex) * m_Vertices.size();

	if (!m_pDynamicVB || m_DynamicVBView.SizeInBytes < bufferSize)
	{
		m_pDynamicVB.Reset();

		D3D12_HEAP_PROPERTIES heapProps = {};
		heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

		D3D12_RESOURCE_DESC bufferDesc = {};
		bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bufferDesc.Width = bufferSize;
		bufferDesc.Height = 1;
		bufferDesc.DepthOrArraySize = 1;
		bufferDesc.MipLevels = 1;
		bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
		bufferDesc.SampleDesc.Count = 1;
		bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		HRESULT hr = m_pDevice->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_NONE,
			&bufferDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&m_pDynamicVB));

		if (FAILED(hr))
		{
			D3D12Log::GetPtr()->Printf("ERROR: Failed to create dynamic vertex buffer");
			return false;
		}

		m_DynamicVBView.BufferLocation = m_pDynamicVB->GetGPUVirtualAddress();
		m_DynamicVBView.StrideInBytes = sizeof(D3D12Vertex);
		m_DynamicVBView.SizeInBytes = static_cast<UINT>(bufferSize);
	}

	// Copy vertex data
	void* pData = nullptr;
	D3D12_RANGE readRange = { 0, 0 };
	HRESULT hr = m_pDynamicVB->Map(0, &readRange, &pData);
	if (SUCCEEDED(hr))
	{
		memcpy(pData, m_Vertices.data(), bufferSize);
		m_pDynamicVB->Unmap(0, nullptr);
	}

	// Set pipeline state and draw
	pCmdList->SetGraphicsRootSignature(m_pRootSignature.Get());
	pCmdList->SetPipelineState(m_pPipelineState.Get());
	pCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	pCmdList->IASetVertexBuffers(0, 1, &m_DynamicVBView);
	pCmdList->DrawInstanced(static_cast<UINT>(m_Vertices.size()), 1, 0, 0);

	// Clear vertices for next frame
	m_Vertices.clear();

	return true;
}
