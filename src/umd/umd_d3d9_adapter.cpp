// Adapted from sunflower2333's native DXVK D3D9 driver (Kancy Joe).
#include "umd_d3d9_adapter.h"
#include "umd_luid.h"
#include "../d3d9/d3d9_caps.h"
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace {
std::mutex adaptersMutex;
std::unordered_map<HANDLE, LUID> adapters;
uintptr_t nextAdapter = 1;

// This is the subset implemented by the native DDI, rather than the larger
// private DXVK renderer's caps. OpenAdapter exposes only this legacy profile.
// In particular: one RT, static/dynamic 2D mip chains, no cube/volume/MSAA/instancing,
// no filtered stretch, autogen, shared resources or gamma. Plain BGRA
// surfaces support ColorFill and unfiltered GPU-to-GPU StretchRect.
constexpr FORMATOP formats[] = {
  {D3DDDIFMT_X8R8G8B8, FORMATOP_TEXTURE | FORMATOP_OFFSCREEN_RENDERTARGET
    | FORMATOP_OFFSCREENPLAIN | FORMATOP_DISPLAYMODE | FORMATOP_3DACCELERATION, 0, 0, 0},
  {D3DDDIFMT_A8R8G8B8, FORMATOP_TEXTURE | FORMATOP_OFFSCREEN_RENDERTARGET
    | FORMATOP_OFFSCREENPLAIN, 0, 0, 0},
  {D3DDDIFMT_D16, FORMATOP_ZSTENCIL_WITH_ARBITRARY_COLOR_DEPTH, 0, 0, 0},
  {D3DDDIFMT_D24S8, FORMATOP_ZSTENCIL_WITH_ARBITRARY_COLOR_DEPTH, 0, 0, 0},
};
constexpr D3DDDIQUERYTYPE queries[] = {
  D3DDDIQUERYTYPE_VCACHE, D3DDDIQUERYTYPE_EVENT, D3DDDIQUERYTYPE_OCCLUSION,
  D3DDDIQUERYTYPE_TIMESTAMP, D3DDDIQUERYTYPE_TIMESTAMPDISJOINT, D3DDDIQUERYTYPE_TIMESTAMPFREQ,
};

D3DCAPS9 nativeCaps() {
  // The native resource path supports 2D textures and shader model 2.
  D3DCAPS9 caps = {};
  caps.DeviceType = D3DDEVTYPE_HAL;
  caps.Caps2 = D3DCAPS2_DYNAMICTEXTURES;
  caps.Caps3 = D3DCAPS3_COPY_TO_VIDMEM | D3DCAPS3_COPY_TO_SYSTEMMEM;
  caps.PresentationIntervals = D3DPRESENT_INTERVAL_IMMEDIATE | D3DPRESENT_INTERVAL_ONE;
  caps.DevCaps = D3DDEVCAPS_TLVERTEXSYSTEMMEMORY | D3DDEVCAPS_TLVERTEXVIDEOMEMORY
    | D3DDEVCAPS_TEXTUREVIDEOMEMORY | D3DDEVCAPS_DRAWPRIMTLVERTEX
    | D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_DRAWPRIMITIVES2EX
    | D3DDEVCAPS_HWTRANSFORMANDLIGHT | D3DDEVCAPS_HWRASTERIZATION;
  caps.PrimitiveMiscCaps = D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW
    | D3DPMISCCAPS_COLORWRITEENABLE | D3DPMISCCAPS_CLIPTLVERTS | D3DPMISCCAPS_TSSARGTEMP
    | D3DPMISCCAPS_BLENDOP | D3DPMISCCAPS_PERSTAGECONSTANT | D3DPMISCCAPS_SEPARATEALPHABLEND
    | D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_FOGANDSPECULARALPHA
    | dxvk::umd::d3d9DdiFogInFvf;
  caps.RasterCaps = D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGTABLE
    | D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_FOGRANGE | D3DPRASTERCAPS_WFOG
    | D3DPRASTERCAPS_ZFOG | D3DPRASTERCAPS_COLORPERSPECTIVE | D3DPRASTERCAPS_SCISSORTEST
    | D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS | D3DPRASTERCAPS_DEPTHBIAS;
  caps.ZCmpCaps = D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESS | D3DPCMPCAPS_EQUAL | D3DPCMPCAPS_LESSEQUAL
    | D3DPCMPCAPS_GREATER | D3DPCMPCAPS_NOTEQUAL | D3DPCMPCAPS_GREATEREQUAL | D3DPCMPCAPS_ALWAYS;
  caps.AlphaCmpCaps = caps.ZCmpCaps;
  caps.SrcBlendCaps = D3DPBLENDCAPS_ZERO | D3DPBLENDCAPS_ONE | D3DPBLENDCAPS_SRCCOLOR
    | D3DPBLENDCAPS_INVSRCCOLOR | D3DPBLENDCAPS_SRCALPHA | D3DPBLENDCAPS_INVSRCALPHA
    | D3DPBLENDCAPS_DESTALPHA | D3DPBLENDCAPS_INVDESTALPHA | D3DPBLENDCAPS_DESTCOLOR
    | D3DPBLENDCAPS_INVDESTCOLOR | D3DPBLENDCAPS_SRCALPHASAT | D3DPBLENDCAPS_BLENDFACTOR;
  caps.DestBlendCaps = caps.SrcBlendCaps;
  caps.ShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_SPECULARGOURAUDRGB
    | D3DPSHADECAPS_ALPHAGOURAUDBLEND | D3DPSHADECAPS_FOGGOURAUD;
  caps.TextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_MIPMAP
    | D3DPTEXTURECAPS_TEXREPEATNOTSCALEDBYSIZE | D3DPTEXTURECAPS_PROJECTED;
  caps.TextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR
    | D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR;
  caps.TextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP
    | D3DPTADDRESSCAPS_BORDER | D3DPTADDRESSCAPS_INDEPENDENTUV | D3DPTADDRESSCAPS_MIRRORONCE;
  caps.LineCaps = D3DLINECAPS_TEXTURE | D3DLINECAPS_ZTEST | D3DLINECAPS_BLEND | D3DLINECAPS_ALPHACMP | D3DLINECAPS_FOG;
  caps.MaxTextureWidth = caps.MaxTextureHeight = caps.MaxTextureAspectRatio = 16384;
  caps.MaxTextureRepeat = 128; caps.MaxAnisotropy = 1; caps.MaxVertexW = 1e10f;
  caps.StencilCaps = D3DSTENCILCAPS_KEEP | D3DSTENCILCAPS_ZERO | D3DSTENCILCAPS_REPLACE
    | D3DSTENCILCAPS_INCRSAT | D3DSTENCILCAPS_DECRSAT | D3DSTENCILCAPS_INVERT
    | D3DSTENCILCAPS_INCR | D3DSTENCILCAPS_DECR | D3DSTENCILCAPS_TWOSIDED;
  caps.FVFCaps = dxvk::caps::MaxSimultaneousTextures | D3DFVFCAPS_PSIZE;
  caps.TextureOpCaps = D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1 | D3DTEXOPCAPS_SELECTARG2
    | D3DTEXOPCAPS_MODULATE | D3DTEXOPCAPS_MODULATE2X | D3DTEXOPCAPS_MODULATE4X
    | D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_SUBTRACT | D3DTEXOPCAPS_DOTPRODUCT3 | D3DTEXOPCAPS_LERP;
  caps.MaxTextureBlendStages = dxvk::caps::MaxTextureBlendStages;
  caps.MaxSimultaneousTextures = dxvk::caps::MaxSimultaneousTextures;
  caps.VertexProcessingCaps = D3DVTXPCAPS_TEXGEN | D3DVTXPCAPS_MATERIALSOURCE7
    | D3DVTXPCAPS_DIRECTIONALLIGHTS | D3DVTXPCAPS_POSITIONALLIGHTS | D3DVTXPCAPS_LOCALVIEWER
    | D3DVTXPCAPS_TEXGEN_SPHEREMAP;
  caps.MaxActiveLights = dxvk::caps::MaxEnabledLights; caps.MaxUserClipPlanes = dxvk::caps::MaxClipPlanes;
  caps.MaxVertexBlendMatrices = 4; caps.MaxPointSize = 64.f;
  caps.MaxPrimitiveCount = 65535; caps.MaxVertexIndex = 65534;
  caps.MaxStreams = dxvk::caps::MaxStreams; caps.MaxStreamStride = 256;
  caps.VertexShaderVersion = D3DVS_VERSION(2, 0); caps.PixelShaderVersion = D3DPS_VERSION(2, 0);
  caps.MaxVertexShaderConst = dxvk::caps::MaxFloatConstantsVS; caps.PixelShader1xMaxValue = 8.f;
  caps.DevCaps2 = D3DDEVCAPS2_STREAMOFFSET;
  caps.NumberOfAdaptersInGroup = 1;
  caps.DeclTypes = D3DDTCAPS_UBYTE4 | D3DDTCAPS_UBYTE4N | D3DDTCAPS_SHORT2N | D3DDTCAPS_SHORT4N
    | D3DDTCAPS_USHORT2N | D3DDTCAPS_USHORT4N | D3DDTCAPS_UDEC3 | D3DDTCAPS_DEC3N
    | D3DDTCAPS_FLOAT16_2 | D3DDTCAPS_FLOAT16_4;
  caps.NumSimultaneousRTs = 1;
  caps.VS20Caps.NumTemps = 12; caps.VS20Caps.StaticFlowControlDepth = 1;
  caps.PS20Caps.NumTemps = 12; caps.PS20Caps.StaticFlowControlDepth = 1;
  caps.PS20Caps.NumInstructionSlots = 96;
  caps.MaxVShaderInstructionsExecuted = caps.MaxPShaderInstructionsExecuted = 65535;
  return caps;
}

HRESULT APIENTRY getCaps(HANDLE handle, const D3DDDIARG_GETCAPS* args) {
  std::lock_guard lock(adaptersMutex);
  if (!adapters.count(handle) || !args) return E_INVALIDARG;
  const auto input = *args;
  if (input.pInfo) return E_INVALIDARG;
  UINT size = 0;
  switch (input.Type) {
    case D3DDDICAPS_GETFORMATCOUNT:
    case D3DDDICAPS_GETD3DQUERYCOUNT: size = sizeof(UINT); break;
    case D3DDDICAPS_GETD3D9CAPS:
      size = sizeof(D3DCAPS9); break;
    case D3DDDICAPS_GETFORMATDATA: size = sizeof(formats); break;
    case D3DDDICAPS_GETD3DQUERYDATA: size = sizeof(queries); break;
    case D3DDDICAPS_GETGAMMARAMPCAPS: size = sizeof(DDIGAMMACAPS); break;
    default: return D3DERR_NOTAVAILABLE;
  }
  if (input.DataSize != size || !input.pData) return E_INVALIDARG;
  switch (input.Type) {
    case D3DDDICAPS_GETFORMATCOUNT: {
      const UINT count = _countof(formats); std::memcpy(input.pData, &count, size); break;
    }
    case D3DDDICAPS_GETD3DQUERYCOUNT: {
      const UINT count = _countof(queries); std::memcpy(input.pData, &count, size); break;
    }
    case D3DDDICAPS_GETD3D9CAPS: {
      const auto caps = nativeCaps(); std::memcpy(input.pData, &caps, size); break;
    }
    case D3DDDICAPS_GETFORMATDATA: std::memcpy(input.pData, formats, size); break;
    case D3DDDICAPS_GETD3DQUERYDATA: std::memcpy(input.pData, queries, size); break;
    case D3DDDICAPS_GETGAMMARAMPCAPS: std::memset(input.pData, 0, size); break;
    default: return E_FAIL;
  }
  return S_OK;
}

HRESULT APIENTRY createDevice(HANDLE handle, D3DDDIARG_CREATEDEVICE* args) {
  LUID luid;
  {
    std::lock_guard lock(adaptersMutex);
    auto entry = adapters.find(handle);
    if (entry == adapters.end()) return E_INVALIDARG;
    luid = entry->second;
  }
  if (!args || !args->pDeviceFuncs) return E_INVALIDARG;
  try {
    auto local = *args;
    auto output = local.pDeviceFuncs;
    D3DDDI_DEVICEFUNCS table = {};
    local.pDeviceFuncs = &table;
    HRESULT hr = dxvk::umd::createAdapterDevice9(luid, &local);
    if (FAILED(hr)) return hr;
    {
      std::lock_guard lock(adaptersMutex);
      if (adapters.count(handle)) {
        std::memcpy(output, &table, dxvk::umd::d3d9DeviceFunctionBytes);
        args->hDevice = local.hDevice;
        return S_OK;
      }
    }
    table.pfnDestroyDevice(local.hDevice);
    return D3DERR_DEVICELOST;
  }
  catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
  catch (...) { return E_FAIL; }
}

HRESULT APIENTRY closeAdapter(HANDLE handle) {
  std::lock_guard lock(adaptersMutex);
  return adapters.erase(handle) ? S_OK : E_INVALIDARG;
}
}

extern "C" __declspec(dllexport) HRESULT APIENTRY OpenAdapter(D3DDDIARG_OPENADAPTER* args) {
  if (!args || !args->hAdapter || !args->pAdapterFuncs || !args->pAdapterCallbacks ||
      !args->pAdapterCallbacks->pfnQueryAdapterInfoCb) return E_INVALIDARG;
  if (args->Interface != 9) return D3DERR_NOTAVAILABLE;
  auto output = args->pAdapterFuncs;
  LUID luid;
  HRESULT hr = dxvk::umd::queryAdapterLuid(args->hAdapter, *args->pAdapterCallbacks, luid);
  if (FAILED(hr)) return hr == DXGI_ERROR_UNSUPPORTED ? D3DERR_NOTAVAILABLE : hr;
  try {
    std::lock_guard lock(adaptersMutex);
    if (!nextAdapter) return E_OUTOFMEMORY;
    auto token = reinterpret_cast<HANDLE>(nextAdapter++);
    adapters.emplace(token, luid);
    *output = {getCaps, createDevice, closeAdapter};
    args->hAdapter = token;
    args->DriverVersion = dxvk::umd::d3d9DriverVersion;
    return S_OK;
  } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}
