#pragma once
#include <d3d10_1.h>
#include <d3d10umddi.h>

namespace dxvk::umd {

template<typename Desc>
constexpr UINT resourceMiscFlags(const Desc& desc) {
  // BIND_PRESENT already puts the displayable surface in a VidMM allocation.
  // The DISPLAYABLE_SURFACE bit does not describe the private cache texture.
  return (desc.BindFlags & D3D10_DDI_BIND_PRESENT)
    ? desc.MiscFlags & ~D3DWDDM2_0DDI_RESOURCE_MISC_DISPLAYABLE_SURFACE : desc.MiscFlags;
}

}
