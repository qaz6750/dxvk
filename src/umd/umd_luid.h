#pragma once
#include <d3dumddi.h>
#include <dxgi.h>
#include "qcdx_escape.h"

namespace dxvk::umd {
inline HRESULT queryAdapterLuid(HANDLE handle, const D3DDDI_ADAPTERCALLBACKS& callbacks, LUID& luid) {
  qcdx_adapter_info_luid info = {};
  static_assert(sizeof(info) == 24);
  D3DDDICB_QUERYADAPTERINFO query = {};
  query.pPrivateDriverData = &info;
  query.PrivateDriverDataSize = sizeof(info);
  HRESULT hr = callbacks.pfnQueryAdapterInfoCb(handle, &query);
  if (FAILED(hr)) return hr;
  if (hr != S_OK || info.base.magic != QCDX_ESC_MAGIC ||
      !(info.base.features & QCDX_CAP_VIDMM_GPUVA) || !(info.luid_low || info.luid_high))
    return DXGI_ERROR_UNSUPPORTED;
  luid = {info.luid_low, info.luid_high};
  return S_OK;
}
}
