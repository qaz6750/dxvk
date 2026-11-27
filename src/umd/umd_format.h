#pragma once
// SPDX-License-Identifier: MIT
#include "umd_api.h"
#include "umd_ddi.h"
#include "umd_multisample_policy.h"

namespace dxvk::umd {

// These are the five optional bits in the negotiated 10.0/10.1/11.0 DDI.
// Public D3D11 IA, buffer, UAV and video bits are not this table's ABI.
inline UINT nativeFormatCaps(DXGI_FORMAT format, UINT api) {
  // These views select one plane of a depth/stencil resource. Attachment
  // support on the backing image does not make the view a color target.
  switch (format) {
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
      api &= ~(D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_DEPTH_STENCIL);
      break;
    default:
      break;
  }

  UINT result = 0;
  if (api & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) result |= D3D10_DDI_FORMAT_SUPPORT_SHADER_SAMPLE;
  if (api & D3D11_FORMAT_SUPPORT_RENDER_TARGET) {
    result |= D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET;
    if (api & D3D11_FORMAT_SUPPORT_BLENDABLE) result |= D3D10_DDI_FORMAT_SUPPORT_BLENDABLE;
  }
  if ((api & (D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_DEPTH_STENCIL))
      && (api & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET))
    result |= D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET;
  if (api & D3D11_FORMAT_SUPPORT_MULTISAMPLE_LOAD) result |= D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD;
  return result;
}

template<typename Check>
HRESULT queryNativeFormatCaps(DXGI_FORMAT format, UINT* output, Check&& check) {
  if (!output) return E_INVALIDARG;
  *output = 0;
  // XR_BIAS has scan-out/CPU-lock/cast attributes, not sampling or RT
  // attributes. Our paired primary allocation path implements RGBA8/BGRA8,
  // so even a renderer DISPLAY bit cannot establish native XR scan-out.
  if (format == DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM) {
    *output = D3D10_DDI_FORMAT_SUPPORT_NOT_SUPPORTED;
    return S_OK;
  }
  UINT support = 0;
  const HRESULT hr = check(format, &support);
  if (hr != S_OK) return hr == E_INVALIDARG || SUCCEEDED(hr) ? E_FAIL : hr;
  *output = nativeFormatCaps(format, support);
  return S_OK;
}

inline HRESULT queryNativeMultisampleLevels(ID3D11Device* device, DXGI_FORMAT format,
                                           UINT count, UINT* output) {
  if (!output) return E_INVALIDARG;
  *output = 0;
  if (count > 1 && format == DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM) return S_OK;
  HRESULT hr = S_OK;
  uint32_t levels = 0;
  const bool complete = multisampleQualityLevels(count, levels, [&](uint32_t& staged) {
    // Preserve format validation for single-sample and non-power-of-two queries.
    if (count == 1 || (count & (count - 1))) {
      hr = device->CheckMultisampleQualityLevels(format, count, &staged);
      return hr == S_OK;
    }

    UINT samples = 0;
    UINT support = 0;
    hr = getAttachmentFormatSupport(device, format, &support, &samples);
    if (hr == E_FAIL) {
      // A missing attachment can still be a valid sampled format. Keep the
      // backend's format error, but never publish its sampled-image levels.
      UINT ignored = 0;
      hr = device->CheckMultisampleQualityLevels(format, count, &ignored);
      return hr == S_OK;
    }
    if (hr != S_OK) return false;
    const UINT caps = nativeFormatCaps(format, support);
    if ((caps & D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET) && (samples & count))
      staged = 1;
    return true;
  });
  if (!complete) return FAILED(hr) ? hr : E_FAIL;
  *output = levels;
  return S_OK;
}

}
