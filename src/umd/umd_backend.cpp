// Based on the embedded backend by sunflower2333 (Kancy Joe).
#include "umd_api.h"
#include "umd_renderer.h"
#include "../d3d11/d3d11_context_imm.h"
#include "../d3d11/d3d11_device.h"

namespace dxvk {
Logger Logger::s_instance("");
}

namespace dxvk::umd {
HRESULT createSharedTexture(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& desc,
                            const D3D11_SUBRESOURCE_DATA* data, ID3D11Texture2D** texture) {
  D3D11_TEXTURE2D_DESC1 extended = {desc.Width, desc.Height, desc.MipLevels, desc.ArraySize,
    desc.Format, desc.SampleDesc, desc.Usage, desc.BindFlags, desc.CPUAccessFlags, desc.MiscFlags,
    D3D11_TEXTURE_LAYOUT_UNDEFINED};
  ID3D11Texture2D1* result = nullptr;
  HRESULT hr = static_cast<D3D11Device*>(device)->CreateTexture2DBase(&extended, data, &result, true);
  if (hr != S_OK)
    return device->CreateTexture2D(&desc, data, texture);
  *texture = result;
  return S_OK;
}

bool hasSharedImageMapping(ID3D11Texture2D* texture) {
  auto common = GetCommonTexture(texture);
  return common->Desc()->PreferMappedTransfer && common->GetMapMode() == D3D11_COMMON_TEXTURE_MAP_MODE_DIRECT;
}

HRESULT getSharedImageMapping(ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                              bool read, D3D11_MAPPED_SUBRESOURCE* mapping) noexcept {
  try { return static_cast<D3D11ImmediateContext*>(context)->GetSharedImageMapping(texture, read, mapping); }
  catch (...) { return DXGI_ERROR_DEVICE_REMOVED; }
}

HRESULT getAttachmentFormatSupport(ID3D11Device* device, DXGI_FORMAT format,
                                   UINT* support, UINT* sampleCounts) {
  return static_cast<D3D11Device*>(device)->GetFormatSupportFlags(format, support, nullptr, sampleCounts);
}

HRESULT flushRuntimeSubmission(ID3D11DeviceContext* context) noexcept {
  try { return static_cast<D3D11ImmediateContext*>(context)->FlushRuntimeSubmission(); }
  catch (...) { return DXGI_ERROR_DEVICE_REMOVED; }
}
HRESULT isStagingResourceBusy(ID3D11DeviceContext* context, ID3D11Resource* resource, BOOL* busy) noexcept {
  try { return static_cast<D3D11ImmediateContext*>(context)->IsStagingResourceBusy(resource, busy); }
  catch (...) { return DXGI_ERROR_DEVICE_REMOVED; }
}

HRESULT createDevice(const LUID& luid, D3D_FEATURE_LEVEL level, bool disableExtraThreads,
                     ID3D11Device** device, ID3D11DeviceContext** context) noexcept {
  if (!device || !context) return E_POINTER;
  *device = nullptr;
  *context = nullptr;
  try {
    Rc<DxvkInstance> instance;
    Rc<DxvkAdapter> adapter;
    HRESULT loaded = openRenderer(luid, 0, instance, adapter);
    if (FAILED(loaded)) return loaded;
    auto renderer = adapter->createDevice(disableExtraThreads);
    level = implementationFeatureLevel(level);
    if (level > D3D11Device::GetMaxFeatureLevel(*renderer)) return DXGI_ERROR_UNSUPPORTED;
    Com<D3D11DXGIDevice> container = new D3D11DXGIDevice(nullptr, nullptr, nullptr,
      instance, adapter, renderer, level, 0);
    auto hr = container->QueryInterface(__uuidof(ID3D11Device), reinterpret_cast<void**>(device));
    if (SUCCEEDED(hr)) (*device)->GetImmediateContext(context);
    return hr;
  } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (const DxvkError& e) {
      Logger::err(str::format("qcdx: ", e.message()));
      return DXGI_ERROR_UNSUPPORTED;
    } catch (...) { return E_FAIL; }
}
}
