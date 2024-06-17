#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>

#pragma comment(lib, "user32.lib")

#include <dxgi1_6.h>
#include <dxgidebug.h>
#include <d3d12.h>
#include <dxcapi.h>

#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxcompiler.lib")

//------------------------------------------------------------------------
// Utilities

#define CHECK_HR(hr) assert(SUCCEEDED(hr))
#define COM_SAFE_RELEASE(obj) ((obj) ? (obj)->Release(), (obj) = nullptr, true : false)

static constexpr size_t KiB(size_t num)
{
	return num << 10ull;
}

static constexpr size_t MiB(size_t num)
{
	return num << 20ull;
}

static constexpr size_t GiB(size_t num)
{
	return num << 30ull;
}

#define ArrayCount(arr) (sizeof(arr) / sizeof((arr)[0]))

template <typename T>
static void ZeroStruct(T *to_zero)
{
	memset(to_zero, 0, sizeof(*to_zero));
}

//------------------------------------------------------------------------
// Window

static LRESULT Win32_WindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param)
{
	return DefWindowProcW(hwnd, message, w_param, l_param);
}

static HWND Win32_CreateWindow()
{
	HWND result = NULL;

	int desktop_w = GetSystemMetrics(SM_CXFULLSCREEN);
	int desktop_h = GetSystemMetrics(SM_CYFULLSCREEN);

	int w = 3*desktop_w / 4;
	int h = 3*desktop_h / 4;

	WNDCLASSEXW wclass = 
	{
		.cbSize        = sizeof(wclass),
		.style         = CS_HREDRAW|CS_VREDRAW,
		.lpfnWndProc   = Win32_WindowProc,
		.hIcon         = LoadIconW(NULL, L"APPICON"),
		.hCursor       = NULL, 
		.lpszClassName = L"HelloBindlessD3D12",
	};

	if (!RegisterClassExW(&wclass))
	{
		assert(!"Failed to register window class");
	}

	RECT wrect = {
		.left   = 0,
		.top    = 0,
		.right  = w,
		.bottom = h,
	};

	AdjustWindowRect(&wrect, WS_OVERLAPPEDWINDOW, FALSE);

	result = CreateWindowExW(0, L"HelloBindlessD3D12", L"Hello Bindless",
							WS_OVERLAPPEDWINDOW,
							64, 64,
							wrect.right - wrect.left,
							wrect.bottom - wrect.top,
							NULL, NULL, NULL, NULL);

	assert(result || !"Failed to create window");

	ShowWindow(result, SW_SHOW);

	return result;
}

//------------------------------------------------------------------------
// DXC

struct DXC_State
{
	IDxcCompiler3 *compiler;
};

DXC_State g_dxc;

void DXC_Init()
{
	HRESULT hr;

	hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&g_dxc.compiler));
	CHECK_HR(hr);
}

bool DXC_CompileShader(
	const char    *source,
	uint32_t       source_size,
	const wchar_t *entry_point,
	const wchar_t *target,
	IDxcBlob     **result_blob,
	IDxcBlob     **error_blob)
{
	assert(g_dxc.compiler || !"Call DXC_Init before calling DXC_CompileShader");

	bool result = false;

	const wchar_t *args[] = {
		L"-E", entry_point,
		L"-T", target,
		L"-WX",
		L"-Zi",
	};

	DxcBuffer source_buffer = {
		.Ptr      = source,
		.Size     = source_size,
		.Encoding = 0,
	};

	HRESULT hr;

	IDxcResult *compile_result = nullptr;
	hr = g_dxc.compiler->Compile(&source_buffer, args, ArrayCount(args), nullptr, IID_PPV_ARGS(&compile_result));

	if (SUCCEEDED(hr))
	{
		HRESULT compile_hr;
		hr = compile_result->GetStatus(&compile_hr);

		if (SUCCEEDED(compile_hr))
		{
			if (compile_result->HasOutput(DXC_OUT_OBJECT))
			{
				hr = compile_result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(result_blob), nullptr);
				CHECK_HR(hr);

				result = true;
			}
		}

		// even if we succeeded, we could have warnings (if I didn't pass WX above...)
		if (compile_result->HasOutput(DXC_OUT_ERRORS))
		{
			hr = compile_result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(error_blob), nullptr);
			CHECK_HR(hr);
		}
	}

	return result;
}


//------------------------------------------------------------------------
// D3D12

static constexpr int  g_frame_latency = 3;
static constexpr bool g_enable_gpu_based_validation = true;

//------------------------------------------------------------------------

enum D3D12_RootParameters
{
	D3D12_RootParameter_32bit_constants,
	D3D12_RootParameter_draw_cbv,
	D3D12_RootParameter_pass_cbv,
	D3D12_RootParameter_view_cbv,
	D3D12_RootParameter_global_cbv,
	D3D12_RootParameter_COUNT,
};

//------------------------------------------------------------------------

static ID3D12Resource *D3D12_CreateUploadBuffer(
	ID3D12Device  *device,
	uint32_t       size,
	const wchar_t *debug_name,
	void          *initial_data      = nullptr,
	uint32_t       initial_data_size = 0) 
{
	D3D12_HEAP_PROPERTIES heap_properties = {
		.Type = D3D12_HEAP_TYPE_UPLOAD,
	};

	D3D12_RESOURCE_DESC desc = {
		.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER,
		.Width            = size,
		.Height           = 1,
		.DepthOrArraySize = 1,
		.MipLevels        = 1,
		.Format           = DXGI_FORMAT_UNKNOWN,
		.SampleDesc       = { .Count = 1, .Quality = 0 },
		.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
	};

	ID3D12Resource *result;
	HRESULT hr = device->CreateCommittedResource(
		&heap_properties,
		D3D12_HEAP_FLAG_NONE,
		&desc,
		D3D12_RESOURCE_STATE_COMMON,
		NULL,
		IID_PPV_ARGS(&result));

	CHECK_HR(hr);

	result->SetName(debug_name);

	if (initial_data)
	{
		assert(initial_data_size <= size || !"Your initial data is too big for this upload buffer!");

		void *mapped;

		D3D12_RANGE read_range = {};
		hr = result->Map(0, &read_range, &mapped);

		CHECK_HR(hr);

		memcpy(mapped, initial_data, initial_data_size);

		D3D12_RANGE write_range = {
			.Begin = 0,
			.End   = initial_data_size,
		};
		result->Unmap(0, &write_range);
	}

	return result;
}

//------------------------------------------------------------------------

struct D3D12_BufferAllocation
{
	ID3D12Resource           *buffer;
	void                     *cpu_base;
	D3D12_GPU_VIRTUAL_ADDRESS gpu_base;
	uint32_t                  offset;
};

struct D3D12_LinearAllocator
{
	ID3D12Resource           *buffer;
	char                     *cpu_base;
	D3D12_GPU_VIRTUAL_ADDRESS gpu_base;
	uint32_t                  at;
	uint32_t                  capacity;

	void Init(ID3D12Device *device, uint32_t size)
	{
		buffer = D3D12_CreateUploadBuffer(device, size, L"Frame Allocator");

		void *mapped;

		D3D12_RANGE null_range = {};
		HRESULT hr = buffer->Map(0, &null_range, &mapped);
		CHECK_HR(hr);

		cpu_base = (char *)mapped;
		gpu_base = buffer->GetGPUVirtualAddress();
		at       = 0;
		capacity = size;
	}

	D3D12_BufferAllocation Allocate(uint32_t size, uint32_t align)
	{
		// evil bit hack: round up to the next multiple of `align` so long as `align` is a power of 2
		uint32_t at_aligned = (at + (align - 1)) & (-(int32_t)align);
		assert(at_aligned <= capacity);

		D3D12_BufferAllocation result = {
			.buffer   = buffer,
			.cpu_base = cpu_base + at_aligned,
			.gpu_base = gpu_base + at_aligned,
			.offset   = at_aligned,
		};

		at = at_aligned + size;

		return result;
	}

	void Reset()
	{
		at = 0;
	}

	void Release()
	{
		buffer->Unmap(0, nullptr);
		buffer->Release();
		ZeroStruct(this);
	}
};

//------------------------------------------------------------------------

struct D3D12_Descriptor
{
	D3D12_CPU_DESCRIPTOR_HANDLE cpu;
	D3D12_GPU_DESCRIPTOR_HANDLE gpu;
	uint32_t                    index;
};

struct D3D12_DescriptorAllocator
{
	ID3D12DescriptorHeap       *heap;
	D3D12_DESCRIPTOR_HEAP_TYPE  type;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu_base;
	D3D12_GPU_DESCRIPTOR_HANDLE gpu_base;
	uint32_t                    stride;
	uint32_t                    at;
	uint32_t                    capacity;

	void Init(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE in_type, uint32_t in_capacity, bool shader_visible, const wchar_t *debug_name)
	{
		D3D12_DESCRIPTOR_HEAP_DESC desc = {
			.Type           = type,
			.NumDescriptors = in_capacity,
			.Flags          = shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
		};

		HRESULT hr;

		hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap));
		CHECK_HR(hr);

		heap->SetName(debug_name);

		cpu_base = heap->GetCPUDescriptorHandleForHeapStart();

		if (shader_visible)
		{
			gpu_base = heap->GetGPUDescriptorHandleForHeapStart();
		}

		stride = device->GetDescriptorHandleIncrementSize(in_type);

		type     = in_type;
		at       = 0;
		capacity = in_capacity;
	}

	D3D12_Descriptor Allocate()
	{
		assert(at < capacity);

		uint32_t index = at++;

		D3D12_Descriptor result = {
			.cpu   = { cpu_base.ptr + stride*index },
			.gpu   = { gpu_base.ptr + stride*index },
			.index = index,
		};

		return result;
	}

	void Reset()
	{
		at = 0;
	}

	void Release()
	{
		heap->Release();
		ZeroStruct(this);
	}
};

//------------------------------------------------------------------------

static bool D3D12_Transition(
	ID3D12Resource         *resource,
	D3D12_RESOURCE_STATES  *current_state,
	D3D12_RESOURCE_STATES   desired_state,
	D3D12_RESOURCE_BARRIER *barrier)
{
	bool result = false;

	if (*current_state != desired_state)
	{
		barrier->Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier->Transition.pResource = resource;
		barrier->Transition.Subresource = 0;
		barrier->Transition.StateBefore = *current_state;
		barrier->Transition.StateAfter  = desired_state;
		*current_state = desired_state;

		result = true;
	}

	return result;
}

//------------------------------------------------------------------------

struct D3D12_Frame
{
	uint64_t fence_value;

	D3D12_LinearAllocator      upload_arena;
	ID3D12CommandAllocator    *command_allocator;
	ID3D12GraphicsCommandList *command_list;

	D3D12_RESOURCE_STATES backbuffer_state;
	ID3D12Resource       *backbuffer;
	D3D12_Descriptor      rtv;
};

struct D3D12_State
{
	IDXGIFactory6       *factory;
	IDXGIAdapter1       *adapter;
	ID3D12Device        *device;
	ID3D12CommandQueue  *queue;
	ID3D12Fence         *fence;
	ID3D12RootSignature *rs_bindless;

	uint64_t frame_index;

	D3D12_DescriptorAllocator cbv_srv_uav;
	D3D12_DescriptorAllocator rtv;

	IDXGISwapChain1 *swap_chain;
	int window_w;
	int window_h;

	D3D12_Frame frames[g_frame_latency];
};

//------------------------------------------------------------------------

D3D12_State g_d3d;

//------------------------------------------------------------------------

void D3D12_Init(HWND window)
{
	(void)window;

	HRESULT hr;

	//------------------------------------------------------------------------
	// Enable Debug Layer

#if BUILD_DEBUG
	{
		ID3D12Debug *debug;

		hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
		CHECK_HR(hr);

		debug->EnableDebugLayer();

		if (g_enable_gpu_based_validation)
		{
			ID3D12Debug1 *debug1;

			hr = debug->QueryInterface(IID_PPV_ARGS(&debug1));
			CHECK_HR(hr);

			debug1->SetEnableGPUBasedValidation(true);
			COM_SAFE_RELEASE(debug1);
		}

		COM_SAFE_RELEASE(debug);
	}
#endif

	//------------------------------------------------------------------------
	// Create Factory

	{
		UINT flags = 0;

#if BUILD_DEBUG
		flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif

		hr = CreateDXGIFactory2(flags, IID_PPV_ARGS(&g_d3d.factory));
		CHECK_HR(hr);
	}

	//------------------------------------------------------------------------
	// Create Adapter and Device

	for (uint32_t adapter_index = 0; 
		 g_d3d.factory->EnumAdapterByGpuPreference(adapter_index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&g_d3d.adapter)) != DXGI_ERROR_NOT_FOUND;
		 adapter_index += 1)
	{
		hr = D3D12CreateDevice(g_d3d.adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_d3d.device));

		if (SUCCEEDED(hr))
		{
			break;
		}

		COM_SAFE_RELEASE(g_d3d.adapter);
	}

	if (!g_d3d.device)
	{
		assert(!"Failed to create D3D12 device");
	}

	//------------------------------------------------------------------------
	// Configure info queue for helpful debug messages

	ID3D12InfoQueue *info_queue;

	hr = g_d3d.device->QueryInterface(IID_PPV_ARGS(&info_queue));

	if (SUCCEEDED(hr))
	{
		info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
		info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
		info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);

		D3D12_MESSAGE_SEVERITY severities[] = {
			D3D12_MESSAGE_SEVERITY_INFO,
		};

		D3D12_MESSAGE_ID deny_ids[] = {
			D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
			D3D12_MESSAGE_ID_MAP_INVALID_NULLRANGE,
			D3D12_MESSAGE_ID_UNMAP_INVALID_NULLRANGE,
		};

		D3D12_INFO_QUEUE_FILTER filter = {
			.DenyList = {
				.NumSeverities = ArrayCount(severities),
				.pSeverityList = severities,
				.NumIDs        = ArrayCount(deny_ids),
				.pIDList       = deny_ids,
			},
		};

		hr = info_queue->PushStorageFilter(&filter);
		CHECK_HR(hr);

		COM_SAFE_RELEASE(info_queue);
	}

	//------------------------------------------------------------------------
	// Create command queue

	{
		D3D12_COMMAND_QUEUE_DESC desc = {
			.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT,
			.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
		};

		hr = g_d3d.device->CreateCommandQueue(&desc, IID_PPV_ARGS(&g_d3d.queue));
		CHECK_HR(hr);

		g_d3d.queue->SetName(L"Direct Command Queue");
	}

	//------------------------------------------------------------------------
	// Create per-frame command allocator, command list, and upload arena

	for (int i = 0; i < g_frame_latency; i++)
	{
		D3D12_Frame *frame = &g_d3d.frames[i];

		hr = g_d3d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame->command_allocator));
		CHECK_HR(hr);

		hr = g_d3d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frame->command_allocator, NULL, IID_PPV_ARGS(&frame->command_list));
		CHECK_HR(hr);

		frame->command_list->Close();

		frame->upload_arena.Init(g_d3d.device, (uint32_t)KiB(64));
	}

	//------------------------------------------------------------------------
	// Create bindless root signature

	{
		D3D12_ROOT_PARAMETER1 parameters[D3D12_RootParameter_COUNT] = {};

		parameters[D3D12_RootParameter_32bit_constants] = {
			.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
			.Constants = {
				.ShaderRegister = 0,
				.RegisterSpace  = 0,
				.Num32BitValues = 58, // 58 because each root cbv takes 2 uints, and the max is 64
			},
		};

		parameters[D3D12_RootParameter_pass_cbv] = {
			.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
			.Descriptor = {
				.ShaderRegister = 1,
				.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE,
			},
		};

		parameters[D3D12_RootParameter_view_cbv] = {
			.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
			.Descriptor = {
				.ShaderRegister = 2,
				.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE,
			},
		};

		parameters[D3D12_RootParameter_global_cbv] = {
			.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
			.Descriptor = {
				.ShaderRegister = 3,
				.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE,
			},
		};

		D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {
			.Version = D3D_ROOT_SIGNATURE_VERSION_1_1,
			.Desc_1_1 = {
				.NumParameters     = ArrayCount(parameters),
				.pParameters       = parameters,
				.NumStaticSamplers = 0,
				.pStaticSamplers   = nullptr,
				.Flags             = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED,
			},
		};

		ID3DBlob *serialized_desc = nullptr;

		hr = D3D12SerializeVersionedRootSignature(&desc, &serialized_desc, nullptr);
		CHECK_HR(hr);

		hr = g_d3d.device->CreateRootSignature(0, serialized_desc->GetBufferPointer(), serialized_desc->GetBufferSize(), IID_PPV_ARGS(&g_d3d.rs_bindless));
		CHECK_HR(hr);

		serialized_desc->Release();
	}

	//------------------------------------------------------------------------
	// Create Fence

	g_d3d.frame_index = 0;

	hr = g_d3d.device->CreateFence(g_d3d.frame_index, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_d3d.fence));
	CHECK_HR(hr);

	//------------------------------------------------------------------------
	// Initialize descriptor allocators

	g_d3d.cbv_srv_uav.Init(g_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096, true, L"CBV SRV UAV Heap");
	g_d3d.rtv        .Init(g_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV,         64,   true, L"RTV Heap");

	//------------------------------------------------------------------------
	// Create swap chain

	{
		DXGI_SWAP_CHAIN_DESC1 desc = {
			.Format      = DXGI_FORMAT_R8G8B8A8_UNORM,
			.SampleDesc  = { .Count = 1, .Quality = 0 },
			.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
			.BufferCount = g_frame_latency,
			.Scaling     = DXGI_SCALING_STRETCH,
			.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD,
		};

		hr = g_d3d.factory->CreateSwapChainForHwnd(g_d3d.queue, window, &desc, nullptr, nullptr, &g_d3d.swap_chain);
		CHECK_HR(hr);

		for (uint32_t i = 0; i < g_frame_latency; i++)
		{
			D3D12_Frame *frame = &g_d3d.frames[i];

			hr = g_d3d.swap_chain->GetBuffer(i, IID_PPV_ARGS(&frame->backbuffer));
			CHECK_HR(hr);

			frame->backbuffer_state = D3D12_RESOURCE_STATE_PRESENT;
			frame->rtv = g_d3d.rtv.Allocate();

			D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {
				.Format        = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
				.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D,
			};
			g_d3d.device->CreateRenderTargetView(frame->backbuffer, &rtv_desc, frame->rtv.cpu);
		}

		// Figure out window dimensions

		RECT client_rect;
		GetClientRect(window, &client_rect);

		g_d3d.window_w = client_rect.right;
		g_d3d.window_h = client_rect.bottom;
	}
}

//------------------------------------------------------------------------

D3D12_Frame *D3D12_GetFrameState()
{
	uint64_t index = g_d3d.frame_index % g_frame_latency;
	return &g_d3d.frames[index];
}

//------------------------------------------------------------------------

void D3D12_BeginFrame()
{
	D3D12_Frame *frame = D3D12_GetFrameState();

	//------------------------------------------------------------------------
	// Wait for frame

	uint64_t completed = g_d3d.fence->GetCompletedValue();

	if (completed < frame->fence_value)
	{
		// NOTE: If you pass nullptr for the event, this call will simply block until the fence value is reached, which is exactly what we want. So we don't even need to make an event!
		g_d3d.fence->SetEventOnCompletion(frame->fence_value, nullptr);
	}

	//------------------------------------------------------------------------
	// Initialize command list

	ID3D12CommandAllocator    *allocator = frame->command_allocator;
	ID3D12GraphicsCommandList *list      = frame->command_list;

	allocator->Reset();
	list     ->Reset(allocator, nullptr);

	list->SetDescriptorHeaps      (1, &g_d3d.cbv_srv_uav.heap);
	list->SetGraphicsRootSignature(g_d3d.rs_bindless);
}

//------------------------------------------------------------------------

void D3D12_EndFrame()
{
	D3D12_Frame *frame = D3D12_GetFrameState();

	//------------------------------------------------------------------------
	// Submit command list

	frame->command_list->Close();

	ID3D12CommandList *lists[] = { frame->command_list };
	g_d3d.queue->ExecuteCommandLists(1, lists);

	//------------------------------------------------------------------------
	// Present

	g_d3d.swap_chain->Present(1, 0);

	//------------------------------------------------------------------------
	// Advance fence

	frame->fence_value = ++g_d3d.frame_index;
	g_d3d.queue->Signal(g_d3d.fence, frame->fence_value);
}

//------------------------------------------------------------------------
// Shader and PSO

struct Vector2D
{
	float x, y;
};

struct Vector3D
{
	float x, y, z;
};

struct Vector4D
{
	float x, y, z, w;
};

struct Vertex
{
	Vector2D position;
	Vector2D uv;
	Vector4D color;
};

struct D3D12_PassConstants
{
	uint32_t vbuffer_srv;
};

struct D3D12_RootConstants
{
	Vector2D offset;
	float    alpha;
};

static_assert(sizeof(D3D12_RootConstants) % 4 == 0, "Root constants have to be a multiple of 4 bytes");

const char *g_shader_source = R"(

//------------------------------------------------------------------------
// Typesafe bindless resource handle

template <typename T>
struct Resource
{
	uint index;

	T Get()
	{
		ResourceDescriptorHeap[index];
	}

	T GetNonUniform()
	{
		ResourceDescriptorHeap[NonUniformResourceIndex(index)];
	}
};

//------------------------------------------------------------------------
// Shader inputs

struct Vertex
{
	float2 position;
	float2 uv;
	float4 color;
};

struct PassConstants
{
	Resource< StructuredBuffer<Vertex> > vbuffer;
};

struct RootConstants
{
	float2 offset;
	float  alpha;
};

ConstantBuffer<PassConstants> pass : register(b1);
ConstantBuffer<RootConstants> root : register(b0);

//------------------------------------------------------------------------
// Vertex shader

void MainVS(
	in  uint   in_vertex_index  : SV_VertexID,
	out float4 out_position     : SV_Position,
	out float2 out_uv           : TEXCOORD,
	out float4 out_color        : COLOR)
{
	Vertex vertex = pass.vbuffer.Get().Load(in_vertex_index);

	out_position = float4(vertex.position + root.offset, 0, 1);
	out_uv       = vertex.uv;
	out_color    = vertex.color;
}

//------------------------------------------------------------------------
// Pixel shader

float4 MainPS(
	in float4 in_position : SV_Position,
	in float2 in_uv       : TEXCOORD,
	in float4 in_color    : COLOR)
{
	float4 color = in_color;
	return color;
}

)";

ID3D12PipelineState *D3D12_CreatePSO()
{
	IDxcBlob *error = nullptr;

	//------------------------------------------------------------------------
	// Compile vertex shader

	IDxcBlob *vs = nullptr;
	if (!DXC_CompileShader(g_shader_source, sizeof(g_shader_source), L"MainVS", L"vs_6_6", &vs, &error))
	{
		const char *error_message = (char *)error->GetBufferPointer();
		fprintf(stderr, "Failed to compile vertex shader:\n%s\n", error_message);
		assert(!"Failed to compile vertex shader, see console output for details");
	}
	COM_SAFE_RELEASE(error);

	//------------------------------------------------------------------------
	// Compile pixel shader

	IDxcBlob *ps = nullptr;
	if (!DXC_CompileShader(g_shader_source, sizeof(g_shader_source), L"MainPS", L"ps_6_6", &ps, &error))
	{
		const char *error_message = (char *)error->GetBufferPointer();
		fprintf(stderr, "Failed to compile pixel shader:\n%s\n", error_message);
		assert(!"Failed to compile vertex shader, see console output for details");
	}
	COM_SAFE_RELEASE(error);

	//------------------------------------------------------------------------
	// Create PSO

	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {
		.pRootSignature = g_d3d.rs_bindless,
		.VS = {
			.pShaderBytecode = vs->GetBufferPointer(),
			.BytecodeLength  = vs->GetBufferSize(),
		},
		.PS = {
			.pShaderBytecode = ps->GetBufferPointer(),
			.BytecodeLength  = ps->GetBufferSize(),
		},
		.BlendState = {
			.RenderTarget = {
				{
					.BlendEnable = true,
					.SrcBlend              = D3D12_BLEND_ONE,
					.DestBlend             = D3D12_BLEND_INV_SRC_ALPHA,
					.BlendOp               = D3D12_BLEND_OP_ADD,
					.SrcBlendAlpha         = D3D12_BLEND_INV_DEST_ALPHA,
					.DestBlendAlpha        = D3D12_BLEND_ONE,
					.BlendOpAlpha          = D3D12_BLEND_OP_ADD,
					.RenderTargetWriteMask = 0xFF,
				},
			},
		},
		.SampleMask = D3D12_DEFAULT_SAMPLE_MASK,
		.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
		.NumRenderTargets = 1,
		.RTVFormats = {
			DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
		},
	};

	ID3D12PipelineState *pso;
	HRESULT hr = g_d3d.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
	CHECK_HR(hr);

	return pso;
}

//------------------------------------------------------------------------

struct TriangleGuy
{
	Vector2D position;
	float    alpha;
};

struct D3D12_Scene
{
	ID3D12PipelineState *pso;

	ID3D12Resource *ibuffer;
	ID3D12Resource *vbuffer;

	D3D12_Descriptor vbuffer_srv;

	uint32_t    triangle_guy_count;
	TriangleGuy triangle_guys[16];
};

//------------------------------------------------------------------------

void D3D12_InitScene(D3D12_Scene *scene)
{
	//------------------------------------------------------------------------
	// Create PSO

	scene->pso = D3D12_CreatePSO();

	//------------------------------------------------------------------------
	// Create index and vertex buffer

	uint16_t indices[] = {
		0, 1, 2,
	};

	Vertex vertices[] = {
		{ {  0.0f,  0.5f }, { 0.5f, 1.0f }, { 1, 0, 0, 1 } },
		{ {  0.5f, -0.5f }, { 1.0f, 0.0f }, { 0, 1, 0, 1 } },
		{ { -0.5f, -0.5f }, { 0.0f, 0.0f }, { 0, 0, 1, 1 } },
	};

	scene->ibuffer = D3D12_CreateUploadBuffer(g_d3d.device, sizeof(indices),  L"Index Buffer",  indices,  sizeof(indices));
	scene->vbuffer = D3D12_CreateUploadBuffer(g_d3d.device, sizeof(vertices), L"Vertex Buffer", vertices, sizeof(vertices));

	//------------------------------------------------------------------------
	// Make vertex buffer SRV

	scene->vbuffer_srv = g_d3d.cbv_srv_uav.Allocate();

	{
		D3D12_SHADER_RESOURCE_VIEW_DESC desc = {
			.Format        = DXGI_FORMAT_UNKNOWN,
			.ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
			.Buffer = {
				.FirstElement        = 0,
				.NumElements         = 3,
				.StructureByteStride = sizeof(Vertex),
			},
		};

		g_d3d.device->CreateShaderResourceView(scene->vbuffer, &desc, scene->vbuffer_srv.cpu);
	}
}

//------------------------------------------------------------------------

void D3D12_Update(D3D12_Scene *scene, double current_time)
{
	for (size_t i = 0; i < scene->triangle_guy_count; i++)
	{
		TriangleGuy *guy = &scene->triangle_guys[i];
		
		guy->position.x = (float)(0.5 * sin(0.6 * (double)i + 1.25*current_time));
		guy->position.y = (float)(0.3 * sin(0.4 * (double)i + 0.65*current_time));
		guy->alpha      = 0.75f + 0.25f*(float)(0.6 * sin(0.8 * (double)i + 0.75*current_time));
	}
}

//------------------------------------------------------------------------
// Do the actually actual rendering!! FINALLY!!

void D3D12_Render(D3D12_Scene *scene)
{
	D3D12_Frame *frame = D3D12_GetFrameState();

	ID3D12GraphicsCommandList *list = frame->command_list;

	//------------------------------------------------------------------------
	// Set and clear rendertarget

	{
		D3D12_RESOURCE_BARRIER barrier;
		if (D3D12_Transition(frame->backbuffer, &frame->backbuffer_state, D3D12_RESOURCE_STATE_RENDER_TARGET, &barrier))
		{
			list->ResourceBarrier(1, &barrier);
		}
	}

	list->OMSetRenderTargets(1, &frame->rtv.cpu, false, nullptr);

	float clear_color[4] = { 0.2f, 0.3f, 0.2f, 1.0f };
	list->ClearRenderTargetView(frame->rtv.cpu, clear_color, 0, nullptr);

	//------------------------------------------------------------------------
	// Input Assembler

	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	D3D12_INDEX_BUFFER_VIEW ibv = {
		.BufferLocation = scene->ibuffer->GetGPUVirtualAddress(),
		.SizeInBytes    = 3*sizeof(uint16_t),
		.Format         = DXGI_FORMAT_R16_UINT,
	};
	list->IASetIndexBuffer(&ibv);

	//------------------------------------------------------------------------
	// Rasterizer State

	D3D12_VIEWPORT viewport = {
		.TopLeftX = 0.0f,
		.TopLeftY = 0.0f,
		.Width    = (float)g_d3d.window_w,
		.Height   = (float)g_d3d.window_h,
		.MinDepth = 0.0f,
		.MaxDepth = 1.0f,
	};

	list->RSSetViewports(1, &viewport);

	D3D12_RECT scissor_rect = {
		.left   = 0,
		.top    = 0,
		.right  = g_d3d.window_w,
		.bottom = g_d3d.window_h,
	};

	list->RSSetScissorRects(1, &scissor_rect);

	//------------------------------------------------------------------------
	// Set PSO

	list->SetPipelineState(scene->pso);

	//------------------------------------------------------------------------
	// Set pass constants

	D3D12_BufferAllocation pass_alloc = frame->upload_arena.Allocate(sizeof(D3D12_PassConstants), 256);

	D3D12_PassConstants *pass_constants = (D3D12_PassConstants *)pass_alloc.cpu_base;
	pass_constants->vbuffer_srv = scene->vbuffer_srv.index;

	list->SetGraphicsRootConstantBufferView(D3D12_RootParameter_pass_cbv, pass_alloc.gpu_base);

	//------------------------------------------------------------------------
	// Draw

	for (size_t i = 0; i < scene->triangle_guy_count; i++)
	{
		TriangleGuy *guy = &scene->triangle_guys[i];

		//------------------------------------------------------------------------
		// Set root constants

		D3D12_RootConstants root_constants = {
			.offset = guy->position,
			.alpha  = guy->alpha,
		};

		uint32_t uint_count = sizeof(root_constants) / sizeof(uint32_t);
		list->SetGraphicsRoot32BitConstants(D3D12_RootParameter_32bit_constants, uint_count, &root_constants, 0);

		//------------------------------------------------------------------------
		// Draw the triangle

		list->DrawIndexedInstanced(3, 1, 0, 0, 0);
	}
}

//------------------------------------------------------------------------
// Main

LARGE_INTEGER g_qpc_freq;

static LARGE_INTEGER GetTime()
{
	LARGE_INTEGER result;
	QueryPerformanceCounter(&result);

	return result;
}

static double TimeElapsed(LARGE_INTEGER start, LARGE_INTEGER end)
{
	if (!g_qpc_freq.QuadPart)
	{
		QueryPerformanceFrequency(&g_qpc_freq);
	}

	return (double)(end.QuadPart - start.QuadPart) / (double)g_qpc_freq.QuadPart;
}

D3D12_Scene g_scene;

int main(int, char **)
{
	HWND window = Win32_CreateWindow();

	//------------------------------------------------------------------------

	D3D12_Init(window);

	//------------------------------------------------------------------------

	D3D12_InitScene(&g_scene);

	//------------------------------------------------------------------------
	// Main loop

	LARGE_INTEGER start_time = GetTime();

	bool running = true;

	while (running)
	{
		MSG msg;
		while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
		{
			switch (msg.message)
			{
				case WM_QUIT:
				{
					running = false;
				} break;

				default:
				{
					TranslateMessage(&msg);
					DispatchMessageW(&msg);
				} break;
			}
		}

		double current_time = TimeElapsed(start_time, GetTime());

		D3D12_Update(&g_scene, current_time);

		D3D12_BeginFrame();
		D3D12_Render(&g_scene);
		D3D12_EndFrame();
	}
}