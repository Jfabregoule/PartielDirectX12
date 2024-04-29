#include <Windows.h>

#include <windowsx.h>
#include "d3dUtil.h"
#include <d3dcompiler.h>

using namespace DirectX;

struct ObjectConstants
{
	DirectX::XMFLOAT4X4 WorldViewProj = MathHelper::Identity4x4();
};

struct Vertex
{
	XMFLOAT3 Pos;
	XMFLOAT4 Color;
};

struct MyMeshGeometry
{
	Microsoft::WRL::ComPtr<ID3D12Resource> VertexBuffer = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> IndexBuffer = nullptr;
	D3D12_VERTEX_BUFFER_VIEW VertexBufferView;
	D3D12_INDEX_BUFFER_VIEW IndexBufferView;
	UINT VertexBufferStride = 0;
	UINT VertexBufferOffset = 0;
	UINT IndexBufferFormat = DXGI_FORMAT_R16_UINT;
	UINT IndexBufferOffset = 0;
};

HINSTANCE								g_hInstance;
HWND									g_mainWindow;

	
bool									m4xMsaaState = false;    // 4X MSAA enabled
UINT									m4xMsaaQuality = 0;      // quality level of 4X MSAA

IDXGIFactory4							*mdxgiFactory;
IDXGISwapChain							*mSwapChain;
ID3D12Device							*md3dDevice;

ID3D12Fence								*mFence;
UINT64									mCurrentFence = 0;

ID3D12CommandQueue						*mCommandQueue;
ID3D12CommandAllocator					*mDirectCmdListAlloc;
ID3D12GraphicsCommandList				*mCommandList;

static const int						SwapChainBufferCount = 2;
int										mCurrBackBuffer = 0;
ID3D12Resource							*mSwapChainBuffer[SwapChainBufferCount];
ID3D12Resource							*mDepthStencilBuffer;

ID3D12DescriptorHeap					*mRtvHeap;
ID3D12DescriptorHeap					*mDsvHeap;

D3D12_VIEWPORT							mScreenViewport;
D3D12_RECT								mScissorRect;

UINT									mRtvDescriptorSize = 0;
UINT									mDsvDescriptorSize = 0;
UINT									mCbvSrvUavDescriptorSize = 0;

D3D_DRIVER_TYPE							md3dDriverType = D3D_DRIVER_TYPE_HARDWARE;
DXGI_FORMAT								mBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
DXGI_FORMAT								mDepthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

int										g_ClientWidth = 800;
int										g_ClientHeight = 600;

ID3D12RootSignature						*mRootSignature = nullptr;
ID3D12DescriptorHeap					*mCbvHeap = nullptr;

ID3DBlob								*mvsByteCode = nullptr;
ID3DBlob								*mpsByteCode = nullptr;

std::vector<D3D12_INPUT_ELEMENT_DESC>	mInputLayout;
ID3D12PipelineState						*mPSO = nullptr;

XMFLOAT4X4								 mWorld = MathHelper::Identity4x4();
XMFLOAT4X4								 mView = MathHelper::Identity4x4();
XMFLOAT4X4								 mProj = MathHelper::Identity4x4();

float									mTheta = 1.5f * XM_PI;
float									mPhi = XM_PIDIV4;
float									mRadius = 5.0f;

ID3D12Resource*							constantBuffer;

bool									Init();
bool									Build();
int										Run();

MyMeshGeometry mBoxGeo; // Define mBoxGeo

void FlushCommandQueue()
{
	// Advance the fence value to mark commands up to this fence point.
	mCurrentFence++;

	// Add an instruction to the command queue to set a new fence point.  Because we 
	// are on the GPU timeline, the new fence point won't be set until the GPU finishes
	// processing all the commands prior to this Signal().
	mCommandQueue->Signal(mFence, mCurrentFence);

	// Wait until the GPU has completed commands up to this fence point.
	if (mFence->GetCompletedValue() < mCurrentFence)
	{
		HANDLE eventHandle = CreateEventEx(nullptr, nullptr, TRUE, EVENT_ALL_ACCESS);

		// Fire event when GPU hits current fence.  
		mFence->SetEventOnCompletion(mCurrentFence, eventHandle);

		// Wait until the GPU hits current fence event is fired.
		WaitForSingleObject(eventHandle, INFINITE);
		CloseHandle(eventHandle);
	}
}

void OnResize()
{
	// Flush before changing any resources.
	FlushCommandQueue();

	mCommandList->Reset(mDirectCmdListAlloc, nullptr);

	// Release the previous resources we will be recreating.
	for (int i = 0; i < SwapChainBufferCount; ++i)
		mSwapChainBuffer[i] = nullptr;
	mDepthStencilBuffer = nullptr;

	// Resize the swap chain.
	mSwapChain->ResizeBuffers(
		SwapChainBufferCount,
		g_ClientWidth, g_ClientHeight,
		mBackBufferFormat,
		DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);

	mCurrBackBuffer = 0;

	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHeapHandle(mRtvHeap->GetCPUDescriptorHandleForHeapStart());
	for (UINT i = 0; i < SwapChainBufferCount; i++)
	{
		mSwapChain->GetBuffer(i, IID_PPV_ARGS(&mSwapChainBuffer[i]));
		md3dDevice->CreateRenderTargetView(mSwapChainBuffer[i], nullptr, rtvHeapHandle);
		rtvHeapHandle.Offset(1, mRtvDescriptorSize);
	}

	// Create the depth/stencil buffer and view.
	D3D12_RESOURCE_DESC depthStencilDesc;
	depthStencilDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthStencilDesc.Alignment = 0;
	depthStencilDesc.Width = g_ClientWidth;
	depthStencilDesc.Height = g_ClientHeight;
	depthStencilDesc.DepthOrArraySize = 1;
	depthStencilDesc.MipLevels = 1;
	depthStencilDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
	depthStencilDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
	depthStencilDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
	depthStencilDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	depthStencilDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE optClear;
	optClear.Format = mDepthStencilFormat;
	optClear.DepthStencil.Depth = 1.0f;
	optClear.DepthStencil.Stencil = 0;
	auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	md3dDevice->CreateCommittedResource(
		&heapProperties,
		D3D12_HEAP_FLAG_NONE,
		&depthStencilDesc,
		D3D12_RESOURCE_STATE_COMMON,
		&optClear,
		IID_PPV_ARGS(&mDepthStencilBuffer));

	// Create descriptor to mip level 0 of entire resource using the format of the resource.
	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc;
	dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	dsvDesc.Format = mDepthStencilFormat;
	dsvDesc.Texture2D.MipSlice = 0;
	md3dDevice->CreateDepthStencilView(mDepthStencilBuffer, &dsvDesc, mDsvHeap->GetCPUDescriptorHandleForHeapStart());

	// Transition the resource from its initial state to be used as a depth buffer.
	auto resourceBarrier = CD3DX12_RESOURCE_BARRIER::Transition(mDepthStencilBuffer,
		D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	mCommandList->ResourceBarrier(1, &resourceBarrier);

	// Execute the resize commands.
	mCommandList->Close();
	ID3D12CommandList* cmdsLists[] = { mCommandList };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	// Wait until resize is complete.
	FlushCommandQueue();

	// Update the viewport transform to cover the client area.
	mScreenViewport.TopLeftX = 0;
	mScreenViewport.TopLeftY = 0;
	mScreenViewport.Width = static_cast<float>(g_ClientWidth);
	mScreenViewport.Height = static_cast<float>(g_ClientHeight);
	mScreenViewport.MinDepth = 0.0f;
	mScreenViewport.MaxDepth = 1.0f;

	mScissorRect = { 0, 0, g_ClientWidth, g_ClientHeight };
}

LRESULT MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_SIZE:
		g_ClientWidth = LOWORD(lParam);
		g_ClientHeight = HIWORD(lParam);
		if (md3dDevice)
			OnResize();
		return 0;

	case WM_EXITSIZEMOVE:
		OnResize();
		return 0;

	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;

	case WM_KEYUP:
		if (wParam == VK_ESCAPE)
		{
			PostQuitMessage(0);
		}
		return 0;
	}

	return DefWindowProc(hwnd, msg, wParam, lParam);
}


LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	return MsgProc(hwnd, msg, wParam, lParam);
}


int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	g_hInstance = hInstance;
	if (!Init())
		return 1;
	if (!Build())
		return 1;
	if (!Run())
		return 1;
}

bool InitMainWindow()
{
	WNDCLASS wc;
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = MainWndProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = g_hInstance;
	wc.hIcon = LoadIcon(0, IDI_APPLICATION);
	wc.hCursor = LoadCursor(0, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
	wc.lpszMenuName = 0;
	wc.lpszClassName = L"MainWnd";

	if (!RegisterClass(&wc))
	{
		MessageBox(0, L"RegisterClass Failed.", 0, 0);
		return false;
	}

	// Compute window rectangle dimensions based on requested client area dimensions.
	RECT R = { 0, 0, g_ClientWidth, g_ClientHeight };
	AdjustWindowRect(&R, WS_OVERLAPPEDWINDOW, false);
	int width = R.right - R.left;
	int height = R.bottom - R.top;

	g_mainWindow = CreateWindow(L"MainWnd", L"App",
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, width, height, 0, 0, g_hInstance, 0);
	if (!g_mainWindow)
	{
		MessageBox(0, L"CreateWindow Failed.", 0, 0);
		return false;
	}

	ShowWindow(g_mainWindow, SW_SHOW);
	UpdateWindow(g_mainWindow);

	return true;
}

void CreateCommandObjects()
{
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	md3dDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&mCommandQueue));

	md3dDevice->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		IID_PPV_ARGS(&mDirectCmdListAlloc));

	md3dDevice->CreateCommandList(
		0,
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		mDirectCmdListAlloc, 
		nullptr,                   
		IID_PPV_ARGS(&mCommandList));

	mCommandList->Close();
}

void CreateSwapChain()
{
	mSwapChain = nullptr;

	DXGI_SWAP_CHAIN_DESC sd;
	sd.BufferDesc.Width = g_ClientWidth;
	sd.BufferDesc.Height = g_ClientHeight;
	sd.BufferDesc.RefreshRate.Numerator = 60;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.BufferDesc.Format = mBackBufferFormat;
	sd.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
	sd.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
	sd.SampleDesc.Count = m4xMsaaState ? 4 : 1;
	sd.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = SwapChainBufferCount;
	sd.OutputWindow = g_mainWindow;
	sd.Windowed = true;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

	// Note: Swap chain uses queue to perform flush.
	mdxgiFactory->CreateSwapChain(
		mCommandQueue,
		&sd,
		&mSwapChain);
}

void CreateRtvAndDsvDescriptorHeaps()
{
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc;
	rtvHeapDesc.NumDescriptors = SwapChainBufferCount;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	rtvHeapDesc.NodeMask = 0;
	md3dDevice->CreateDescriptorHeap(
		&rtvHeapDesc, IID_PPV_ARGS(&mRtvHeap));


	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
	dsvHeapDesc.NumDescriptors = 1;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	dsvHeapDesc.NodeMask = 0;
	md3dDevice->CreateDescriptorHeap(
		&dsvHeapDesc, IID_PPV_ARGS(&mDsvHeap));
}

bool InitDirect3D()
{
	CreateDXGIFactory1(IID_PPV_ARGS(&mdxgiFactory));

	// Try to create hardware device.
	HRESULT hardwareResult = D3D12CreateDevice(
		nullptr,             // default adapter
		D3D_FEATURE_LEVEL_11_0,
		IID_PPV_ARGS(&md3dDevice));

	// Fallback to WARP device.
	if (FAILED(hardwareResult))
	{
		IDXGIAdapter *pWarpAdapter;
		mdxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&pWarpAdapter));

		D3D12CreateDevice(
			pWarpAdapter,
			D3D_FEATURE_LEVEL_11_0,
			IID_PPV_ARGS(&md3dDevice));
	}

	md3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE,
		IID_PPV_ARGS(&mFence));

	mRtvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	mDsvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	mCbvSrvUavDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msQualityLevels;
	msQualityLevels.Format = mBackBufferFormat;
	msQualityLevels.SampleCount = 4;
	msQualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
	msQualityLevels.NumQualityLevels = 0;
	md3dDevice->CheckFeatureSupport(
		D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
		&msQualityLevels,
		sizeof(msQualityLevels));

	m4xMsaaQuality = msQualityLevels.NumQualityLevels;

	CreateCommandObjects();
	CreateSwapChain();
	CreateRtvAndDsvDescriptorHeaps();

	return true;
}


bool Init()
{
	if (!InitMainWindow())
		return 0;
	if (!InitDirect3D())
		return 0;
}

void BuildDescriptorHeaps()
{
	D3D12_DESCRIPTOR_HEAP_DESC cbvHeapDesc;
	cbvHeapDesc.NumDescriptors = 1;
	cbvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	cbvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	cbvHeapDesc.NodeMask = 0;
	md3dDevice->CreateDescriptorHeap(&cbvHeapDesc,
		IID_PPV_ARGS(&mCbvHeap));
}

void BuildConstantBuffers()
{
	// Calculate constant buffer size
	UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));

	// Create a committed resource
	
	auto heapProperties3 = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	auto buffer3 = CD3DX12_RESOURCE_DESC::Buffer(objCBByteSize);
	md3dDevice->CreateCommittedResource(
		&heapProperties3,
		D3D12_HEAP_FLAG_NONE,
		&buffer3,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&constantBuffer));

	// Map the resource
	UINT8* cbvDataBegin;
	CD3DX12_RANGE readRange(0, 0); // We do not intend to read from this resource on the CPU.
	constantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&cbvDataBegin));

	// Build the constant buffer view descriptor
	D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
	cbvDesc.BufferLocation = constantBuffer->GetGPUVirtualAddress();
	cbvDesc.SizeInBytes = objCBByteSize;

	// Create the constant buffer view
	md3dDevice->CreateConstantBufferView(&cbvDesc, mCbvHeap->GetCPUDescriptorHandleForHeapStart());

	// Unmap the resource (not strictly necessary since it's an UPLOAD heap, but good practice)
	constantBuffer->Unmap(0, nullptr);
}

void BuildRootSignature()
{

	// Root parameter can be a table, root descriptor or root constants.
	CD3DX12_ROOT_PARAMETER slotRootParameter[1];

	// Create a single descriptor table of CBVs.
	CD3DX12_DESCRIPTOR_RANGE cbvTable;
	cbvTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
	slotRootParameter[0].InitAsDescriptorTable(1, &cbvTable);

	// A root signature is an array of root parameters.
	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(1, slotRootParameter, 0, nullptr,
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
	ID3DBlob *serializedRootSig = nullptr;
	ID3DBlob *errorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		&serializedRootSig, &errorBlob);

	if (errorBlob != nullptr)
	{
		::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
	}
	ThrowIfFailed(hr);

	ThrowIfFailed(md3dDevice->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(&mRootSignature)));
}

ID3DBlob *CompileShader(
	const std::wstring& filename,
	const D3D_SHADER_MACRO* defines,
	const std::string& entrypoint,
	const std::string& target)
{
	UINT compileFlags = 0;

	HRESULT hr = S_OK;

	ID3DBlob *byteCode = nullptr;
	ID3DBlob *errors;
	hr = D3DCompileFromFile(filename.c_str(), defines, D3D_COMPILE_STANDARD_FILE_INCLUDE,
		entrypoint.c_str(), target.c_str(), compileFlags, 0, &byteCode, &errors);

	if (errors != nullptr)
		OutputDebugStringA((char*)errors->GetBufferPointer());

	ThrowIfFailed(hr);

	return byteCode;
}

void BuildShadersAndInputLayout()
{
	HRESULT hr = S_OK;

	mvsByteCode = CompileShader(L"Shaders\\color.hlsl", nullptr, "VS", "vs_5_0");
	mpsByteCode = CompileShader(L"Shaders\\color.hlsl", nullptr, "PS", "ps_5_0");

	mInputLayout =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};
}

void BuildBoxGeometry()
{
	// Define vertices and indices for a cube
	std::array<Vertex, 8> vertices = 
	{
		Vertex({XMFLOAT3(-1.0f,-1.0f,-1.0f), XMFLOAT4(Colors::White)}),
		Vertex({XMFLOAT3(-1.0f,1.0f,-1.0f), XMFLOAT4(Colors::Black)}),
		Vertex({XMFLOAT3(1.0f,1.0f,-1.0f), XMFLOAT4(Colors::Red)}),
		Vertex({XMFLOAT3(1.0f,-1.0f,-1.0f), XMFLOAT4(Colors::Green)}),
		Vertex({XMFLOAT3(-1.0f,-1.0f,1.0f), XMFLOAT4(Colors::Blue)}),
		Vertex({XMFLOAT3(-1.0f,1.0f,1.0), XMFLOAT4(Colors::Yellow)}),
		Vertex({XMFLOAT3(1.0f,1.0f,1.0f), XMFLOAT4(Colors::Cyan)}),
		Vertex({XMFLOAT3(1.0f,-1.0f,1.0f), XMFLOAT4(Colors::Magenta)})
	};

	std::array<std::uint16_t, 36> indices =
	{
		// Front face
		0, 1, 2,
		0, 2, 3,
		// Back face
		4, 6, 5,
		4, 7, 6,
		// Left face
		4, 5, 1,
		4, 1, 0,
		// Right face
		3, 2, 6,
		3, 6, 7,
		// Top face
		1, 5, 6,
		1, 6, 2,
		// Bottom face
		4, 0, 3,
		4, 3, 7
	};

	const UINT vbByteSize = static_cast<UINT>(vertices.size()) * sizeof(Vertex);
	const UINT ibByteSize = static_cast<UINT>(indices.size()) * sizeof(std::uint16_t);

	// Create vertex buffer
	Microsoft::WRL::ComPtr<ID3D12Resource> vertexUploadBuffer;
	mBoxGeo.VertexBuffer = d3dUtil::CreateDefaultBuffer(md3dDevice, mCommandList, vertices.data(), vbByteSize, vertexUploadBuffer);

	// Initialize vertex buffer view
	mBoxGeo.VertexBufferView.BufferLocation = mBoxGeo.VertexBuffer->GetGPUVirtualAddress();
	mBoxGeo.VertexBufferView.StrideInBytes = sizeof(Vertex);
	mBoxGeo.VertexBufferView.SizeInBytes = vbByteSize;

	// Create index buffer
	Microsoft::WRL::ComPtr<ID3D12Resource> indexUploadBuffer;
	mBoxGeo.IndexBuffer = d3dUtil::CreateDefaultBuffer(md3dDevice, mCommandList, indices.data(), ibByteSize, indexUploadBuffer);

	// Initialize index buffer view
	mBoxGeo.IndexBufferView.BufferLocation = mBoxGeo.IndexBuffer->GetGPUVirtualAddress();
	mBoxGeo.IndexBufferView.Format = DXGI_FORMAT_R16_UINT;
	mBoxGeo.IndexBufferView.SizeInBytes = ibByteSize;
}


void BuildPSO()
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc;
	ZeroMemory(&psoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
	psoDesc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
	psoDesc.pRootSignature = mRootSignature;
	psoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mvsByteCode->GetBufferPointer()),
		mvsByteCode->GetBufferSize()
	};
	psoDesc.PS =
	{
		reinterpret_cast<BYTE*>(mpsByteCode->GetBufferPointer()),
		mpsByteCode->GetBufferSize()
	};
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = mBackBufferFormat;
	psoDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
	psoDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
	psoDesc.DSVFormat = mDepthStencilFormat;
	md3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPSO));
}

bool Build()
{
	mCommandList->Reset(mDirectCmdListAlloc, nullptr);

	BuildDescriptorHeaps();
	BuildConstantBuffers();
	BuildRootSignature();
	BuildShadersAndInputLayout();
	BuildBoxGeometry();
	BuildPSO();

	mCommandList->Close();
	ID3D12CommandList* cmdsLists[] = { mCommandList };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	FlushCommandQueue();

	return 1;
}

void Update(ID3D12Resource* constantBuffer)
{
	// Convert Spherical to Cartesian coordinates.
	float x = mRadius * sinf(mPhi) * cosf(mTheta);
	float z = mRadius * sinf(mPhi) * sinf(mTheta);
	float y = mRadius * cosf(mPhi);

	// Build the view matrix.
	XMVECTOR pos = XMVectorSet(x, y, z, 1.0f);
	XMVECTOR target = XMVectorZero();
	XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

	XMMATRIX view = XMMatrixLookAtLH(pos, target, up);
	XMStoreFloat4x4(&mView, view);

	XMMATRIX world = XMLoadFloat4x4(&mWorld);
	XMMATRIX proj = XMLoadFloat4x4(&mProj);
	XMMATRIX worldViewProj = world * view * proj;

	// Update the constant buffer with the latest worldViewProj matrix.
	ObjectConstants objConstants;
	XMStoreFloat4x4(&objConstants.WorldViewProj, XMMatrixTranspose(worldViewProj));

	// Map the constant buffer resource.
	UINT8* cbvDataBegin;
	CD3DX12_RANGE readRange(0, 0); // We do not intend to read from this resource on the CPU.
	HRESULT hr = constantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&cbvDataBegin));
	if (FAILED(hr))
	{
		// Handle mapping failure
		// This could be due to incorrect resource setup or other issues
		// Print error message or throw an exception
		return;
	}

	// Copy the constant buffer data.
	memcpy(cbvDataBegin, &objConstants, sizeof(ObjectConstants));

	// Unmap the constant buffer resource.
	constantBuffer->Unmap(0, nullptr);
}


void Draw()
{
	mDirectCmdListAlloc->Reset();

	mCommandList->Reset(mDirectCmdListAlloc, mPSO);

	mCommandList->RSSetViewports(1, &mScreenViewport);
	mCommandList->RSSetScissorRects(1, &mScissorRect);

	auto resourceBarrier5 = CD3DX12_RESOURCE_BARRIER::Transition(mSwapChainBuffer[mCurrBackBuffer],
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

	mCommandList->ResourceBarrier(1, &resourceBarrier5);

	mCommandList->ClearRenderTargetView(CD3DX12_CPU_DESCRIPTOR_HANDLE(
										mRtvHeap->GetCPUDescriptorHandleForHeapStart(),
										mCurrBackBuffer,
										mRtvDescriptorSize)
										, Colors::LightSteelBlue, 0, nullptr);
	mCommandList->ClearDepthStencilView(mDsvHeap->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

	auto descriptorHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(
		mRtvHeap->GetCPUDescriptorHandleForHeapStart(),
		mCurrBackBuffer,
		mRtvDescriptorSize);
	auto handleforheap = mDsvHeap->GetCPUDescriptorHandleForHeapStart();
	mCommandList->OMSetRenderTargets(1, &descriptorHandle, true, &handleforheap);

	ID3D12DescriptorHeap* descriptorHeaps[] = { mCbvHeap };
	mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

	mCommandList->SetGraphicsRootSignature(mRootSignature);

	mCommandList->IASetVertexBuffers(0, 1, &mBoxGeo.VertexBufferView);
	mCommandList->IASetIndexBuffer(&mBoxGeo.IndexBufferView);
	mCommandList->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	mCommandList->SetGraphicsRootDescriptorTable(0, mCbvHeap->GetGPUDescriptorHandleForHeapStart());

	mCommandList->DrawIndexedInstanced(36, 1, 0, 0, 0);

	auto resourcebarrier6 = CD3DX12_RESOURCE_BARRIER::Transition(mSwapChainBuffer[mCurrBackBuffer],
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

	mCommandList->ResourceBarrier(1, &resourcebarrier6);

	mCommandList->Close();

	ID3D12CommandList* cmdsLists[] = { mCommandList };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	mSwapChain->Present(0, 0);
	mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

	FlushCommandQueue();
}


int Run()
{
	MSG msg = { 0 };

	while (msg.message != WM_QUIT)
	{
		// If there are Window messages then process them.
		if (PeekMessage(&msg, 0, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		// Otherwise, do animation/game stuff.
		else
		{
			Update(constantBuffer);
			Draw();
		}
	}

	return (int)msg.wParam;
}
