#pragma once
#include <d3d10_1.h>
#include <d3d10umddi.h>

namespace dxvk::umd {

template<typename Desc>
constexpr UINT resourceMiscFlags(const Desc& desc) {
  // The runtime controls presentation of the VidMM allocation. Display and
  // discard-on-present flags do not describe its private renderable texture.
  return (desc.BindFlags & D3D10_DDI_BIND_PRESENT)
    ? desc.MiscFlags & ~(D3DWDDM2_0DDI_RESOURCE_MISC_DISPLAYABLE_SURFACE | D3D10_DDI_RESOURCE_MISC_DISCARD_ON_PRESENT) : desc.MiscFlags;
}

}
