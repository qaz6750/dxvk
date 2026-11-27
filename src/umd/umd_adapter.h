#pragma once
#include "umd_interface.h"
#include <memory>

namespace dxvk::umd {
struct AdapterIdentity { LUID luid; bool gpuPublish; };
SIZE_T privateDeviceSize();
HRESULT createAdapterDevice(const std::shared_ptr<const AdapterIdentity>& identity,
  D3D10DDIARG_CREATEDEVICE* args);
}
