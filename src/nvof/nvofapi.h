// ============================================================================
//  nvofapi.h - NVIDIA Optical Flow (NVOFA) interface, reconstructed
//
//  PROVENANCE - read this before trusting anything here.
//
//  1. The function-pointer ORDER below was extracted by disassembling
//     NvOFAPICreateInstanceD3D12 in this machine's own
//     C:/Windows/System32/nvofapi64.dll (export RVA 0xebe0, driver 616.56) and
//     pairing every `lea rax,[rip+X]` with the `mov [list+off], rax` that
//     consumes it.  Slots 2..9 were confirmed instruction by instruction;
//     slots 0 and 1 are consistent with the same extraction (the pairing
//     script had a register-aliasing artefact there, not a disagreement).
//
//  2. There is NO version field in this list.  The API version is passed to
//     NvOFAPICreateInstanceD3D12 only, as (major << 4) | minor; this driver
//     advertises 0x50 = SDK 5.0, and rejects 0x00 with status 7
//     (NV_OF_ERR_INVALID_VERSION).
//
//  3. VERIFIED BY EXECUTION (see tools/nvof_d3d12_probe.cpp): the SDK 2.0
//     field OFFSETS and the NV_OF_PERF_LEVEL values (SLOW 5 / MEDIUM 10 /
//     FAST 20) are all correct on this SDK 5.0 driver.  What is NOT SDK 2.0 is
//     the SIZE: the driver reads offsets 0x30/0x34/0x38, which lie outside the
//     56-byte 2.0 struct.  Passing that struct feeds it stack garbage and every
//     call returns NV_OF_ERR_INVALID_PARAM (5).  Pass a >=0x3c-byte ZEROED
//     buffer with the fields written at these offsets and Init returns 0.
//     This struct is therefore a field map, not an allocation - allocate bigger.
//     Confirmed afterwards: input formats = 3 (DXGI_FORMAT 61 R8_UNORM,
//     103 NV12, 87).
//
//  4. Verified against NVOFA_Programming_Guide.pdf: output vectors are
//     int16 pairs packed into int32, S10.5 fixed point (5 fractional bits,
//     10 integer, 1 sign) - divide by 32.0 to get pixels.  D3D12 uses explicit
//     NV_OF_FENCE_POINT (ID3D12Fence* + value) for input and output sync.
//     Cost buffers should be UINT8.
// ============================================================================
#ifndef SM86_NVOFAPI_H
#define SM86_NVOFAPI_H

#include <windows.h>
#include <stdint.h>
#include <d3d12.h>

#define NVOFAPI __stdcall

typedef void* NvOFHandle;
typedef void* NvOFGPUBufferHandle;
typedef void* NvOFPrivDataHandle;

typedef enum NV_OF_STATUS {
    NV_OF_SUCCESS                    = 0,
    NV_OF_ERR_OF_NOT_AVAILABLE       = 1,
    NV_OF_ERR_UNSUPPORTED_DEVICE     = 2,
    NV_OF_ERR_DEVICE_DOES_NOT_EXIST  = 3,
    NV_OF_ERR_INVALID_PTR            = 4,
    NV_OF_ERR_INVALID_PARAM          = 5,
    NV_OF_ERR_INVALID_CALL           = 6,
    NV_OF_ERR_INVALID_VERSION        = 7,
    NV_OF_ERR_OUT_OF_MEMORY          = 8,
    NV_OF_ERR_NOT_INITIALIZED        = 9,
    NV_OF_ERR_UNSUPPORTED_FEATURE    = 10,
    NV_OF_ERR_GENERIC                = 11
} NV_OF_STATUS;

typedef enum NV_OF_MODE {
    NV_OF_MODE_UNDEFINED = 0, NV_OF_MODE_OPTICALFLOW = 1,
    NV_OF_MODE_STEREODISPARITY = 2, NV_OF_MODE_MAX = 3
} NV_OF_MODE;

typedef enum NV_OF_PERF_LEVEL {   // SDK 2.0 name; SDK 5.0 calls this "preset"
    NV_OF_PERF_LEVEL_UNDEFINED = 0, NV_OF_PERF_LEVEL_SLOW = 5,
    NV_OF_PERF_LEVEL_MEDIUM = 10,   NV_OF_PERF_LEVEL_FAST = 20,
    NV_OF_PERF_LEVEL_MAX = 21
} NV_OF_PERF_LEVEL;

typedef enum NV_OF_OUTPUT_VECTOR_GRID_SIZE {
    NV_OF_OUTPUT_VECTOR_GRID_SIZE_UNDEFINED = 0,
    NV_OF_OUTPUT_VECTOR_GRID_SIZE_1 = 1,
    NV_OF_OUTPUT_VECTOR_GRID_SIZE_2 = 2,
    NV_OF_OUTPUT_VECTOR_GRID_SIZE_4 = 4,
    NV_OF_OUTPUT_VECTOR_GRID_SIZE_MAX = 5
} NV_OF_OUTPUT_VECTOR_GRID_SIZE;

typedef enum NV_OF_HINT_VECTOR_GRID_SIZE {
    NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED = 0,
    NV_OF_HINT_VECTOR_GRID_SIZE_1 = 1, NV_OF_HINT_VECTOR_GRID_SIZE_2 = 2,
    NV_OF_HINT_VECTOR_GRID_SIZE_4 = 4, NV_OF_HINT_VECTOR_GRID_SIZE_8 = 8,
    NV_OF_HINT_VECTOR_GRID_SIZE_MAX = 9
} NV_OF_HINT_VECTOR_GRID_SIZE;

typedef enum NV_OF_BUFFER_USAGE {
    NV_OF_BUFFER_USAGE_UNDEFINED = 0, NV_OF_BUFFER_USAGE_INPUT = 1,
    NV_OF_BUFFER_USAGE_OUTPUT = 2, NV_OF_BUFFER_USAGE_HINT = 3,
    NV_OF_BUFFER_USAGE_COST = 4, NV_OF_BUFFER_USAGE_MAX = 5
} NV_OF_BUFFER_USAGE;

typedef enum NV_OF_BUFFER_FORMAT {
    NV_OF_BUFFER_FORMAT_UNDEFINED = 0, NV_OF_BUFFER_FORMAT_UINT8 = 1,
    NV_OF_BUFFER_FORMAT_UINT16 = 2,    NV_OF_BUFFER_FORMAT_UINT32 = 3,
    NV_OF_BUFFER_FORMAT_MAX = 4
} NV_OF_BUFFER_FORMAT;

typedef struct NV_OF_STEREO_DISPARITY_RANGE {
    uint32_t minDisparity; uint32_t maxDisparity;
} NV_OF_STEREO_DISPARITY_RANGE;

typedef struct NV_OF_FENCE_POINT {   // D3D12 only
    ID3D12Fence* fence; uint64_t value;
} NV_OF_FENCE_POINT;

// Maximum number of in-flight queued tasks the D3D12 execute input struct
// carries fence points for.  The SDK guide states the EXECUTE_INPUT_D3D12
// carries an ARRAY of input fence points; this is the array length.  Chosen
// per the NVIDIA Optical Flow SDK (NV_OF_MAX_QUEUED_TASKS).  We allocate for
// this many input fence points below; the driver reads exactly this many.
#define NV_OF_MAX_QUEUED_TASKS 4

// ---- Register resource params (D3D12) ----
// Field offsets mirror the SDK 2.0 common header and were kept (the init
// thunk at RVA 0x57e0 confirms the driver reads params field-by-field from
// rdx).  Allocate generously and zero the buffer at runtime; only the field
// OFFSETS matter, not the C sizeof, because the driver reads past the C
// struct end on SDK 5.0.
typedef struct NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 {
    ID3D12Resource* pResource;             // +0x00
    NV_OF_BUFFER_USAGE bufferUsage;        // +0x08 (4 bytes; INPUT=1/OUTPUT=2/HINT=3/COST=4)
    uint8_t _pad0[4];                     // +0x0c
    NV_OF_FENCE_POINT inputFencePoint;    // +0x10 (fence ptr 0x10, value 0x18)
    NV_OF_FENCE_POINT outputFencePoint;   // +0x20 (fence ptr 0x20, value 0x28)
} NV_OF_REGISTER_RESOURCE_PARAMS_D3D12;   // real size 0x30; allocate >=0x40

// ---- Execute common params (shared D3D12 / CUDA / Vulkan) ----
typedef struct NV_OF_EXECUTE_INPUT_PARAMS {
    NvOFGPUBufferHandle inputFrame;        // +0x00
    NvOFGPUBufferHandle referenceFrame;    // +0x08
    NvOFGPUBufferHandle externalHints;     // +0x10 (0 if none)
    uint32_t disableTemporalHints;         // +0x18 (NV_OF_BOOL / uint32)
    uint8_t _pad1[4];                      // +0x1c
    NvOFPrivDataHandle hPrivData;          // +0x20 (0)
    uint32_t numRois;                      // +0x28 (0)
    uint8_t _pad2[4];                      // +0x2c
    void* roiData;                         // +0x30 (0)
} NV_OF_EXECUTE_INPUT_PARAMS;              // 0x38

typedef struct NV_OF_EXECUTE_OUTPUT_PARAMS {
    NvOFGPUBufferHandle outputBuffer;      // +0x00
    NvOFGPUBufferHandle costBuffer;        // +0x08 (0 if cost disabled)
} NV_OF_EXECUTE_OUTPUT_PARAMS;             // 0x10

// ---- Execute params (D3D12) ----
typedef struct NV_OF_EXECUTE_INPUT_PARAMS_D3D12 {
    NV_OF_EXECUTE_INPUT_PARAMS commonParams;                       // +0x00 .. +0x38
    NV_OF_FENCE_POINT inputFencePoint[NV_OF_MAX_QUEUED_TASKS];      // +0x38 ..
} NV_OF_EXECUTE_INPUT_PARAMS_D3D12;       // 0x38 + N*0x10

typedef struct NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 {
    NV_OF_EXECUTE_OUTPUT_PARAMS commonParams;  // +0x00 .. +0x10
    NV_OF_FENCE_POINT outputFencePoint;        // +0x10 (fence ptr 0x10, value 0x18)
} NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12;       // 0x20

typedef struct NV_OF_INIT_PARAMS {
    uint32_t width;  uint32_t height;
    NV_OF_OUTPUT_VECTOR_GRID_SIZE outGridSize;
    NV_OF_HINT_VECTOR_GRID_SIZE   hintGridSize;
    NV_OF_MODE mode; NV_OF_PERF_LEVEL perfLevel;
    uint8_t enableExternalHints; uint8_t enableOutputCost;
    NvOFPrivDataHandle hPrivData;
    NV_OF_STEREO_DISPARITY_RANGE disparityRange;
    uint8_t enableRoi;
} NV_OF_INIT_PARAMS;

typedef struct NV_OF_BUFFER_DESCRIPTOR {
    uint32_t width; uint32_t height;
    NV_OF_BUFFER_USAGE  bufferUsage;
    NV_OF_BUFFER_FORMAT bufferFormat;
} NV_OF_BUFFER_DESCRIPTOR;

#if defined(__cplusplus)
extern "C" {
#endif

typedef NV_OF_STATUS (NVOFAPI *PFN_NVOF_GET_MAX_SUPPORTED_API_VERSION)(uint32_t* pVersion);
typedef NV_OF_STATUS (NVOFAPI *PFN_NVOF_CREATE_INSTANCE_D3D12)(uint32_t version, void* pFunctionList);

// ---- NV_OF_D3D12_API_FUNCTION_LIST : order verified by disassembly (see top) ----
typedef NV_OF_STATUS (NVOFAPI *PFN_CREATE_OF_D3D12)(
    ID3D12Device* pDevice, NvOFHandle* phOf);
typedef NV_OF_STATUS (NVOFAPI *PFN_OF_INIT)(
    NvOFHandle hOf, const NV_OF_INIT_PARAMS* pInitParams);
typedef NV_OF_STATUS (NVOFAPI *PFN_OF_GET_SURFACE_FORMAT_COUNT_D3D12)(
    NvOFHandle hOf, NV_OF_BUFFER_USAGE usage, NV_OF_MODE mode, uint32_t* pCount);
typedef NV_OF_STATUS (NVOFAPI *PFN_OF_GET_SURFACE_FORMAT_D3D12)(
    NvOFHandle hOf, NV_OF_BUFFER_USAGE usage, NV_OF_MODE mode, DXGI_FORMAT* pFormats);
typedef NV_OF_STATUS (NVOFAPI *PFN_OF_REGISTER_RESOURCE_D3D12)(
    NvOFHandle hOf, const void* pRegisterParams);
typedef NV_OF_STATUS (NVOFAPI *PFN_OF_UNREGISTER_RESOURCE_D3D12)(
    NvOFHandle hOf, NvOFGPUBufferHandle hBuffer);
typedef NV_OF_STATUS (NVOFAPI *PFN_OF_EXECUTE_D3D12)(
    NvOFHandle hOf, const void* pExecuteInParams, void* pExecuteOutParams);
typedef NV_OF_STATUS (NVOFAPI *PFN_OF_DESTROY)(NvOFHandle hOf);
typedef NV_OF_STATUS (NVOFAPI *PFN_OF_GET_LAST_ERROR)(
    NvOFHandle hOf, char* pszLastError, uint32_t bufferSize);
typedef NV_OF_STATUS (NVOFAPI *PFN_OF_GET_CAPS)(
    NvOFHandle hOf, uint32_t capsParam, uint32_t* pVal);

typedef struct NV_OF_D3D12_API_FUNCTION_LIST {
    PFN_CREATE_OF_D3D12                    nvCreateOpticalFlowD3D12;      // +0x00
    PFN_OF_INIT                            nvOFInit;                      // +0x08
    PFN_OF_GET_SURFACE_FORMAT_COUNT_D3D12  nvOFGetSurfaceFormatCountD3D12;// +0x10
    PFN_OF_GET_SURFACE_FORMAT_D3D12        nvOFGetSurfaceFormatD3D12;     // +0x18
    PFN_OF_REGISTER_RESOURCE_D3D12         nvOFRegisterResourceD3D12;     // +0x20
    PFN_OF_UNREGISTER_RESOURCE_D3D12       nvOFUnregisterResourceD3D12;   // +0x28
    PFN_OF_EXECUTE_D3D12                   nvOFExecuteD3D12;              // +0x30
    PFN_OF_DESTROY                         nvOFDestroy;                   // +0x38
    PFN_OF_GET_LAST_ERROR                  nvOFGetLastError;              // +0x40
    PFN_OF_GET_CAPS                        nvOFGetCaps;                   // +0x48
} NV_OF_D3D12_API_FUNCTION_LIST;

#if defined(__cplusplus)
}
#endif
#endif  // SM86_NVOFAPI_H
