#include "umd_allocation.h"
#include "umd_api.h"
#include "umd_residency_transaction.h"
#include <cstring>
#include <utility>

namespace dxvk::umd {
static_assert(sizeof(qcdx_alloc) == 64);
constexpr uint64_t maxSurfaceSize = 64ull << 20;

DXGI_FORMAT allocationFormat(const qcdx_alloc& a) {
  bool srgb = a.flags & QCDX_ALLOC_F_SRGB;
  switch (a.format) {
    case 21: return srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
    case 22: return srgb ? DXGI_FORMAT_B8G8R8X8_UNORM_SRGB : DXGI_FORMAT_B8G8R8X8_UNORM;
    case 31: return srgb ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R10G10B10A2_UNORM;
    case 113: return srgb ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 32: return srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}
static UINT pixelBytes(const qcdx_alloc& a) { return a.format == 113 ? 8u : 4u; }
static UINT rowBytes(const qcdx_alloc& a) { return a.width * pixelBytes(a); }
static bool validSurface(const qcdx_alloc& a) {
  constexpr auto knownFlags = QCDX_ALLOC_F_DIRECT | QCDX_ALLOC_F_PRIMARY | QCDX_ALLOC_F_SRGB;
  return a.magic == QCDX_ALLOC_MAGIC && !a.bo && !a.bo_offset && !(a.flags & ~knownFlags)
      && a.kind <= QCDX_ALLOC_GDI && a.width && a.height && a.width <= 16384 && a.height <= 16384
      && (!primaryAllocation(a) || !a.vidpn) && allocationFormat(a) != DXGI_FORMAT_UNKNOWN
      && a.pitch >= rowBytes(a) && !(a.pitch % 4) && uint64_t(a.pitch) * a.height <= a.size
      && a.size <= maxSurfaceSize;
}

RuntimeAllocation::~RuntimeAllocation() { release(); }
HRESULT RuntimeAllocation::release() {
  auto binding = std::move(surface);
  return binding && binding->owner ? binding->owner->release(binding) : S_OK;
}
bool RuntimeAllocation::canRotateWith(const RuntimeAllocation& other) const {
  if (!surface || !other.surface) return !surface && !other.surface;
  const auto& a = surface->layout;
  const auto& b = other.surface->layout;
  return surface->owner && surface->owner == other.surface->owner && surface->handle &&
      other.surface->handle && !surface->locked && !other.surface->locked &&
      !surface->pendingLock && !other.surface->pendingLock &&
      a.width == b.width && a.height == b.height && a.format == b.format;
}
void RuntimeAllocation::swapIdentity(RuntimeAllocation& other) {
  if (!surface) return;
  std::swap(surface->handle, other.surface->handle);
  std::swap(surface->layout, other.surface->layout);
  // The runtime rotates its resource-to-allocation associations at return.
}
void RuntimeMemory::initialize(HANDLE rtDevice, const D3DDDI_DEVICECALLBACKS& cb,
                               const DXGI_DDI_BASE_CALLBACKS* dxgiCallbacks) {
  device = rtDevice;
  dxgi = dxgiCallbacks;
  if (!dxgi) callbacks.pfnPresentCb = cb.pfnPresentCb;
  callbacks.pfnAllocateCb = cb.pfnAllocateCb;
  callbacks.pfnDeallocate2Cb = cb.pfnDeallocate2Cb;
  callbacks.pfnLock2Cb = cb.pfnLock2Cb;
  callbacks.pfnUnlock2Cb = cb.pfnUnlock2Cb;
  callbacks.pfnSetDisplayModeCb = cb.pfnSetDisplayModeCb;
  callbacks.pfnQueryResidencyCb = cb.pfnQueryResidencyCb;
  callbacks.pfnSetPriorityCb = cb.pfnSetPriorityCb;
  callbacks.pfnCreateContextCb = cb.pfnCreateContextCb;
  callbacks.pfnDestroyContextCb = cb.pfnDestroyContextCb;
}
HRESULT RuntimeMemory::allocate(RuntimeAllocation& out, HANDLE resource, UINT width, UINT height,
                                DXGI_FORMAT format, bool primary) {
  if (!available() || out.surface || !resource || !width || !height ||
      width > 16384 || height > 16384) return E_INVALIDARG;
  qcdx_alloc layout = {};
  layout.magic = QCDX_ALLOC_MAGIC;
  layout.kind = QCDX_ALLOC_UMD;
  layout.width = width;
  layout.height = height;
  layout.flags = primary ? QCDX_ALLOC_F_PRIMARY : 0;
  switch (format) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: layout.format = 113; break;
    case DXGI_FORMAT_R10G10B10A2_UNORM: layout.format = 31; break;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: layout.flags |= QCDX_ALLOC_F_SRGB; [[fallthrough]];
    case DXGI_FORMAT_B8G8R8A8_UNORM: layout.format = 21; break;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: layout.flags |= QCDX_ALLOC_F_SRGB; [[fallthrough]];
    case DXGI_FORMAT_B8G8R8X8_UNORM: layout.format = 22; break;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: layout.flags |= QCDX_ALLOC_F_SRGB; [[fallthrough]];
    case DXGI_FORMAT_R8G8B8A8_UNORM: layout.format = 32; break;
    default: return DXGI_ERROR_UNSUPPORTED;
  }
  layout.pitch = rowBytes(layout);
  if (uint64_t(layout.pitch) * height > maxSurfaceSize) return E_OUTOFMEMORY;
  layout.size = layout.pitch * height;
  auto binding = std::make_shared<VidmmSurface>();
  surfaces.push_front(binding);
  binding->owner = this;
  binding->layout = layout;
  out.surface = binding;
  D3DDDI_ALLOCATIONINFO2 info = {};
  info.pPrivateDriverData = &layout;
  info.PrivateDriverDataSize = sizeof(layout);
  info.Flags.Primary = primary;
  D3DDDICB_ALLOCATE request = {};
  request.NumAllocations = 1;
  request.hResource = resource;
  request.pAllocationInfo2 = &info;
  binding->pendingAllocate = &request;
  HRESULT hr = callbacks.pfnAllocateCb(device, &request);
  binding->pendingAllocate = nullptr;
  if (binding->owner != this) return DXGI_ERROR_DEVICE_REMOVED;
  if (info.hAllocation) {
    binding->resource = resource;
    binding->handle = info.hAllocation;
  }
  if (hr == S_OK && binding->handle) return S_OK;
  out.release();
  return FAILED(hr) ? hr : E_FAIL;
}
HRESULT RuntimeMemory::adopt(RuntimeAllocation& out, D3DKMT_HANDLE allocation, const qcdx_alloc& layout) {
  if (!available() || out.surface || !allocation || !validSurface(layout)) return E_INVALIDARG;
  auto binding = std::make_shared<VidmmSurface>();
  surfaces.push_front(binding);
  binding->owner = this;
  binding->layout = layout;
  binding->handle = allocation;
  out.surface = std::move(binding);
  return S_OK;
}
HRESULT RuntimeMemory::release(const std::shared_ptr<VidmmSurface>& binding) {
  if (binding->owner != this) return S_OK;
  if (auto request = std::exchange(binding->pendingAllocate, nullptr)) {
    if (request->pAllocationInfo2->hAllocation) {
      binding->handle = request->pAllocationInfo2->hAllocation;
      binding->resource = request->hResource;
    }
  }
  if (auto request = std::exchange(binding->pendingLock, nullptr))
    binding->locked = request->pData != nullptr;
  HRESULT hr = S_OK;
  if (std::exchange(binding->locked, false)) {
    D3DDDICB_UNLOCK2 unlock = {};
    unlock.hAllocation = binding->handle;
    hr = callbacks.pfnUnlock2Cb(device, &unlock);
    if (binding->owner != this) return hr;
  }
  binding->owner = nullptr;
  binding->handle = 0;
  auto resource = std::exchange(binding->resource, nullptr);
  surfaces.remove(binding);
  if (resource && device) {
    D3DDDICB_DEALLOCATE2 destroy = {};
    destroy.hResource = resource;
    // VidMM owns the backing pages and tracks outstanding presents. Ordinary
    // surface release can return while VidMM retires its last reference.
    destroy.Flags.SynchronousDestroy = closing || FAILED(hr);
    HRESULT result = callbacks.pfnDeallocate2Cb(device, &destroy);
    if (FAILED(result)) return result;
  }
  return hr;
}
template<typename GetPixels>
HRESULT RuntimeMemory::transfer(RuntimeAllocation& out, bool publish, SurfaceTransfer region, SharedReadGuard guard, GetPixels getPixels) {
  auto binding = out.surface;
  if (!available() || !binding || binding->owner != this || !binding->handle) return E_INVALIDARG;
  if (binding->locked || binding->pendingLock) return DXGI_ERROR_WAS_STILL_DRAWING;
  region.bounds = out.clip(region.bounds);
  if (region.keep.contains(region.bounds)) return S_OK;
  D3DDDICB_LOCK2 map = {};
  map.hAllocation = binding->handle;
  binding->pendingLock = &map;
  HRESULT hr = callbacks.pfnLock2Cb(device, &map);
  binding->pendingLock = nullptr;
  if (binding->owner != this) return DXGI_ERROR_DEVICE_REMOVED;
  if (FAILED(hr)) return hr;
  binding->locked = true;
  // Lock2 may reenter the DDI. Synchronize the renderable image only once
  // the runtime has returned and consume its pixels before calling Unlock2.
  D3D11_MAPPED_SUBRESOURCE pixels = {};
  hr = (map.pData && hr == S_OK) ? getPixels(pixels) : E_FAIL;
  if (binding->owner != this) return DXGI_ERROR_DEVICE_REMOVED;
  if (hr == S_OK && !guard.current()) hr = DXGI_ERROR_WAS_STILL_DRAWING;
  if (hr == S_OK && (!pixels.pData || pixels.RowPitch < rowBytes(binding->layout)))
    hr = E_INVALIDARG;
  if (hr == S_OK) {
    const auto& layout = binding->layout;
    region.forEach([&](SurfaceRect area) {
      if (publish)
        copySurfaceRect(map.pData, layout.pitch, pixels.pData, pixels.RowPitch, pixelBytes(layout), area);
      else
        copySurfaceRect(pixels.pData, pixels.RowPitch, map.pData, layout.pitch, pixelBytes(layout), area);
    });
  }
  D3DDDICB_UNLOCK2 unlock = {};
  unlock.hAllocation = binding->handle;
  binding->locked = false;
  auto result = callbacks.pfnUnlock2Cb(device, &unlock);
  if (binding->owner != this) return DXGI_ERROR_DEVICE_REMOVED;
  return FAILED(result) ? result : hr;
}
HRESULT RuntimeMemory::upload(RuntimeAllocation& out, const void* data, UINT pitch, SurfaceRect area) {
  if (!data || !out.surface || pitch < rowBytes(out.surface->layout)) return E_INVALIDARG;
  return transfer(out, true, {area, {}}, {}, [&](D3D11_MAPPED_SUBRESOURCE& pixels) {
    pixels = {const_cast<void*>(data), pitch, 0};
    return S_OK;
  });
}
HRESULT RuntimeMemory::download(RuntimeAllocation& out, void* data, UINT pitch, SurfaceTransfer region, SharedReadGuard guard) {
  if (!data || !out.surface || pitch < rowBytes(out.surface->layout)) return E_INVALIDARG;
  return transfer(out, false, region, guard, [&](D3D11_MAPPED_SUBRESOURCE& pixels) {
    pixels = {data, pitch, 0};
    return S_OK;
  });
}
HRESULT RuntimeMemory::transferImage(RuntimeAllocation& out, ID3D11DeviceContext* context,
                                    ID3D11Texture2D* image, bool publish, SurfaceTransfer region, SharedReadGuard guard) {
  return transfer(out, publish, region, guard, [&](D3D11_MAPPED_SUBRESOURCE& pixels) {
    return getSharedImageMapping(context, image, publish, &pixels);
  });
}
HRESULT RuntimeMemory::setDisplayMode(RuntimeAllocation& out, const Live& live) {
  if (!available() || !live()) return DXGI_ERROR_DEVICE_REMOVED;
  if (!out.primary() || !out.handle()) return E_INVALIDARG;
  D3DDDICB_SETDISPLAYMODE mode = {};
  mode.hPrimaryAllocation = out.handle();
  return callbacks.pfnSetDisplayModeCb(device, &mode);
}
HRESULT RuntimeMemory::ensureContext(const Live& live) {
  if (!available() || !live()) return DXGI_ERROR_DEVICE_REMOVED;
  if (!context) {
    if (pendingContext) return DXGI_ERROR_WAS_STILL_DRAWING;
    D3DDDICB_CREATECONTEXT create = {};
    create.EngineAffinity = 1;
    pendingContext = &create;
    auto hr = callbacks.pfnCreateContextCb(device, &create);
    pendingContext = nullptr;
    if (!available()) return DXGI_ERROR_DEVICE_REMOVED;
    if (hr != S_OK || !create.hContext) return FAILED(hr) ? hr : E_FAIL;
    context = create.hContext;
    if (!live()) return DXGI_ERROR_DEVICE_REMOVED;
  }
  return S_OK;
}
HRESULT RuntimeMemory::present9(RuntimeAllocation& source, const Live& live) {
  HRESULT hr = ensureContext(live);
  if (FAILED(hr)) return hr;
  D3DDDICB_PRESENT request = {};
  request.hSrcAllocation = source.handle();
  request.hContext = context;
  return callbacks.pfnPresentCb(device, &request);
}
HRESULT RuntimeMemory::present(RuntimeAllocation& source, const DXGI_DDI_ARG_PRESENT& args,
                              const Live& live, D3DKMT_HANDLE destination) {
  HRESULT hr = ensureContext(live);
  if (FAILED(hr)) return hr;
  DXGIDDICB_PRESENT present = {};
  present.hSrcAllocation = source.handle();
  present.hDstAllocation = destination;
  present.hContext = context;
  present.pDXGIContext = args.pDXGIContext;
  return dxgi->pfnPresentCb(device, &present);
}
HRESULT RuntimeMemory::queryResidency(const D3DKMT_HANDLE* handles, UINT count,
                                      D3DDDI_RESIDENCYSTATUS* output, const Live& live) {
  if (!available() || !callbacks.pfnQueryResidencyCb) return DXGI_ERROR_UNSUPPORTED;
  return residencyTransaction(handles, count, output, [&](auto ids, auto status, UINT size) {
    D3DDDICB_QUERYRESIDENCY request = {};
    request.HandleList = ids;
    request.NumAllocations = size;
    request.pResidencyStatus = status;
    return callbacks.pfnQueryResidencyCb(device, &request);
  }, live, [](auto value) {
    return value == D3DDDI_RESIDENCYSTATUS_RESIDENTINGPUMEMORY ||
           value == D3DDDI_RESIDENCYSTATUS_RESIDENTINSHAREDMEMORY ||
           value == D3DDDI_RESIDENCYSTATUS_NOTRESIDENT;
  }, HRESULT(E_INVALIDARG), HRESULT(DXGI_ERROR_UNSUPPORTED), HRESULT(E_FAIL), HRESULT(DXGI_ERROR_DEVICE_REMOVED));
}
HRESULT RuntimeMemory::setPriority(D3DKMT_HANDLE handle, UINT priority, const Live& live) {
  if (!available() || !live()) return DXGI_ERROR_DEVICE_REMOVED;
  if (!callbacks.pfnSetPriorityCb) return DXGI_ERROR_UNSUPPORTED;
  D3DDDICB_SETPRIORITY request = {};
  request.HandleList = &handle;
  request.NumAllocations = 1;
  request.pPriorities = &priority;
  return callbacks.pfnSetPriorityCb(device, &request);
}
HRESULT RuntimeMemory::closeDeviceAllocations() {
  closing = true;
  HRESULT result = S_OK;
  while (!surfaces.empty()) {
    auto binding = surfaces.front();
    auto hr = release(binding);
    if (FAILED(hr)) result = hr;
  }
  if (auto request = std::exchange(pendingContext, nullptr))
    context = request->hContext;
  if (context) {
    D3DDDICB_DESTROYCONTEXT destroy = {};
    destroy.hContext = std::exchange(context, nullptr);
    auto hr = callbacks.pfnDestroyContextCb(device, &destroy);
    if (FAILED(hr)) result = hr;
  }
  device = nullptr;
  return result;
}
}
