#pragma once

#include "umd_ddi.h"

namespace dxvk::umd {

inline bool supportedNativeInterface(UINT interfaceVersion, UINT version, UINT flags) noexcept {
  UINT build;
  switch (interfaceVersion) {
    case D3D11_0_DDI_INTERFACE_VERSION: build = D3D11_0_DDI_BUILD_VERSION; break;
    case D3D11_0_7_DDI_INTERFACE_VERSION: build = D3D11_0_7_DDI_BUILD_VERSION; break;
    default: return false;
  }
  constexpr UINT allowed = D3D11DDI_CREATEDEVICE_FLAG_SINGLETHREADED
    | D3D11DDI_CREATEDEVICE_FLAG_3DPIPELINESUPPORT_MASK;
  return (version >> 16) >= build && !(flags & ~allowed)
      && D3D11DDI_EXTRACT_3DPIPELINELEVEL_FROM_FLAGS(flags) == D3D11DDI_3DPIPELINELEVEL_10_0;
}

inline bool nativeDxgiUses1_1(UINT interfaceVersion, UINT version) noexcept {
  // DXGI's table revision is carried in the lower runtime-version bits, even
  // within one exact D3D device interface. The WDK macro defines its bounds.
  return IS_DXGI1_1_BASE_FUNCTIONS(interfaceVersion, version) != FALSE;
}

}
