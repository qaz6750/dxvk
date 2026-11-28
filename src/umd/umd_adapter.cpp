// Adapted from sunflower2333's native D3D11 adapter (Kancy Joe).
#include "umd_adapter.h"
#include "umd_luid.h"
#include <mutex>
#include <cstring>
#include <algorithm>
#include <unordered_map>

namespace {
using namespace dxvk::umd;
std::mutex adaptersMutex;
std::unordered_map<void*, std::shared_ptr<const AdapterIdentity>> adapters;
uintptr_t nextToken = 1;

auto retain(D3D10DDI_HADAPTER handle) {
  std::lock_guard lock(adaptersMutex);
  auto entry = adapters.find(handle.pDrvPrivate);
  return entry == adapters.end() ? nullptr : entry->second;
}

SIZE_T APIENTRY sizeDevice(D3D10DDI_HADAPTER handle, const D3D10DDIARG_CALCPRIVATEDEVICESIZE* args) {
  return args && retain(handle) && supportedNativeInterface(args->Interface, args->Version, args->Flags)
      ? privateDeviceSize() : 0;
}
HRESULT APIENTRY createDevice(D3D10DDI_HADAPTER handle, D3D10DDIARG_CREATEDEVICE* args) {
  auto identity = retain(handle);
  if (!identity || !args || !args->pKTCallbacks
      || !args->DXGIBaseDDI.pDXGIBaseCallbacks || !args->DXGIBaseDDI.pDXGIDDIBaseFunctions)
    return E_INVALIDARG;
  if (!supportedNativeInterface(args->Interface, args->Version, args->Flags)) return DXGI_ERROR_UNSUPPORTED;
  const auto& cb = *args->pKTCallbacks;
  if (!cb.pfnAllocateCb || !cb.pfnLock2Cb || !cb.pfnUnlock2Cb || !cb.pfnDeallocate2Cb
      || !cb.pfnCreateContextCb || !cb.pfnDestroyContextCb || !cb.pfnSetDisplayModeCb
      || !args->DXGIBaseDDI.pDXGIBaseCallbacks->pfnPresentCb) return DXGI_ERROR_UNSUPPORTED;
  try { return createAdapterDevice(identity, args); }
  catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
  catch (...) { return E_FAIL; }
}
HRESULT APIENTRY closeAdapter(D3D10DDI_HADAPTER handle) {
  std::lock_guard lock(adaptersMutex);
  return adapters.erase(handle.pDrvPrivate) ? S_OK : E_INVALIDARG;
}
HRESULT APIENTRY versions(D3D10DDI_HADAPTER handle, UINT* count, UINT64* output) {
  if (!count || !retain(handle)) return E_INVALIDARG;
  constexpr UINT64 values[] = {D3D11_0_DDI_SUPPORTED, D3D11_0_7_DDI_SUPPORTED,
    D3D10_0_DDI_SUPPORTED, D3D10_0_7_DDI_SUPPORTED};
  const auto capacity = output ? *count : 0;
  *count = UINT(std::size(values));
  if (output && capacity < *count) return E_OUTOFMEMORY;
  if (output) std::copy(std::begin(values), std::end(values), output);
  return S_OK;
}
HRESULT APIENTRY caps(D3D10DDI_HADAPTER handle, const D3D10_2DDIARG_GETCAPS* args) {
  if (!retain(handle) || !args || !args->pData) return E_INVALIDARG;
  switch (args->Type) {
    case D3D11DDICAPS_3DPIPELINESUPPORT:
      if (args->DataSize != sizeof(D3D11DDI_3DPIPELINESUPPORT_CAPS)) return E_INVALIDARG;
      static_cast<D3D11DDI_3DPIPELINESUPPORT_CAPS*>(args->pData)->Caps =
        D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11DDI_3DPIPELINELEVEL_10_0);
      return S_OK;
    case D3D11DDICAPS_THREADING:
      if (args->DataSize != sizeof(D3D11DDI_THREADING_CAPS)) return E_INVALIDARG;
      break;
    case D3D11DDICAPS_SHADER:
      if (args->DataSize != sizeof(D3D11DDI_SHADER_CAPS)) return E_INVALIDARG;
      break;
    default: return DXGI_ERROR_UNSUPPORTED;
  }
  std::memset(args->pData, 0, args->DataSize);
  return S_OK;
}
}

template<typename Table>
static HRESULT openAdapter(D3D10DDIARG_OPENADAPTER* args, Table* output) {
  if (!args || !args->hRTAdapter.handle || !output ||
      !args->pAdapterCallbacks || !args->pAdapterCallbacks->pfnQueryAdapterInfoCb) return E_INVALIDARG;
  args->hAdapter = {};
  LUID luid;
  UINT features = 0;
  HRESULT hr = dxvk::umd::queryAdapterLuid(args->hRTAdapter.handle, *args->pAdapterCallbacks, luid, &features);
  if (FAILED(hr)) return hr;
  try {
    auto identity = std::make_shared<dxvk::umd::AdapterIdentity>();
    identity->luid = luid;
    identity->gpuPublish = (features & QCDX_CAP_GPU_PUBLISH) != 0;
    identity->scanoutGamma = (features & QCDX_CAP_SCANOUT_GAMMA) != 0;
    std::lock_guard lock(adaptersMutex);
    if (!nextToken) return E_OUTOFMEMORY;
    auto token = reinterpret_cast<void*>(nextToken++);
    adapters.emplace(token, std::move(identity));
    auto& table = *output;
    table.pfnCalcPrivateDeviceSize = sizeDevice;
    table.pfnCreateDevice = createDevice;
    table.pfnCloseAdapter = closeAdapter;
    if constexpr (std::is_same_v<Table, D3D10_2DDI_ADAPTERFUNCS>) {
      table.pfnGetSupportedVersions = versions;
      table.pfnGetCaps = caps;
    }
    args->hAdapter.pDrvPrivate = token;
    return S_OK;
  } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

extern "C" HRESULT APIENTRY OpenAdapter10(D3D10DDIARG_OPENADAPTER* args) {
  if (!args) return E_INVALIDARG;
  if (!dxvk::umd::isD3D10Interface(args->Interface)) return DXGI_ERROR_UNSUPPORTED;
  return openAdapter(args, args->pAdapterFuncs);
}

extern "C" HRESULT APIENTRY OpenAdapter10_2(D3D10DDIARG_OPENADAPTER* args) {
  return args ? openAdapter(args, args->pAdapterFuncs_2) : E_INVALIDARG;
}
