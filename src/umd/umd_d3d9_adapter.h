#pragma once

#include <d3d9.h>
#include <d3dumddi.h>
#include <cstddef>

namespace dxvk::umd {
// These entrypoints implement the original D3D9 DDI. Current SDK headers are
// a superset used for local storage; their default version also requires newer
// resource, blit and synchronization DDIs that this bridge does not implement.
inline constexpr UINT d3d9DriverVersion = D3D_UMD_INTERFACE_VERSION_VISTA;
// Driver-only FVF fog capability from SDK d3dhal.h (FOGINFVF). This is
// deliberately distinct from public D3DPMISCCAPS_FOGANDSPECULARALPHA.
inline constexpr DWORD d3d9DdiFogInFvf = 0x00002000;
static_assert(d3d9DdiFogInFvf != D3DPMISCCAPS_FOGANDSPECULARALPHA);
inline constexpr size_t d3d9DeviceFunctionBytes =
  offsetof(D3DDDI_DEVICEFUNCS, pfnRename) + sizeof(PFND3DDDI_RENAME);
static_assert(d3d9DeviceFunctionBytes == 99 * sizeof(void*));

HRESULT createAdapterDevice9(const LUID& luid, D3DDDIARG_CREATEDEVICE* args);
}
