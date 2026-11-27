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
static bool unsupportedCopy(HRESULT hr) {
  return hr == E_NOTIMPL || hr == E_INVALIDARG || hr == DXGI_ERROR_UNSUPPORTED;
}
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
      !surface->pendingLock && !other.surface->pendingLock && !surface->publishing && !other.surface->publishing &&
      a.width == b.width && a.height == b.height && a.format == b.format;
}
void RuntimeAllocation::swapIdentity(RuntimeAllocation& other) {
  if (!surface) return;
  std::swap(surface->handle, other.surface->handle);
  std::swap(surface->layout, other.surface->layout);
  // The runtime rotates its resource-to-allocation associations at return.
}
void RuntimeMemory::initialize(HANDLE rtDevice, const D3DDDI_DEVICECALLBACKS& cb,
                               const DXGI_DDI_BASE_CALLBACKS* dxgiCallbacks, bool allowPublish) {
  device = rtDevice;
  dxgi = dxgiCallbacks;
  if (allowPublish) {
    callbacks.pfnRenderCb = cb.pfnRenderCb;
    callbacks.pfnSignalSynchronizationObject2Cb = cb.pfnSignalSynchronizationObject2Cb;
    gpuPublish = cb.pfnRenderCb && cb.pfnSignalSynchronizationObject2Cb;
  }
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
  const HRESULT sourceResult = dropPublishSource(*binding);
  if (binding->owner != this) return sourceResult;
  if (FAILED(sourceResult)) hr = sourceResult;
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
HRESULT RuntimeMemory::dropPublishSource(VidmmSurface& binding) {
  if (auto pending = std::exchange(binding.pendingSource, nullptr))
    binding.publishSource = pending->hAllocation;
  auto handle = std::exchange(binding.publishSource, 0);
  if (!handle) return S_OK;
  D3DDDICB_DEALLOCATE2 drop = {};
  drop.NumAllocations = 1;
  drop.HandleList = &handle;
  drop.Flags.SynchronousDestroy = TRUE;
  return callbacks.pfnDeallocate2Cb(device, &drop);
}

HRESULT RuntimeMemory::createPublishSource(VidmmSurface& binding, const D3D11_MAPPED_SUBRESOURCE& pixels) {
  auto hr = dropPublishSource(binding);
  if (binding.owner != this) return DXGI_ERROR_DEVICE_REMOVED;
  if (FAILED(hr)) return hr;
  qcdx_alloc src = {};
  src.magic = QCDX_ALLOC_MAGIC;
  src.kind = QCDX_ALLOC_UMD;
  src.flags = QCDX_ALLOC_F_PUBLISH_SOURCE;
  src.width = binding.layout.width;
  src.height = binding.layout.height;
  src.format = binding.layout.format;
  src.pitch = pixels.RowPitch;
  src.size = pixels.RowPitch * src.height;
  const auto address = uint64_t(reinterpret_cast<uintptr_t>(pixels.pData));
  src.reserved[0] = static_cast<UINT>(address);
  src.reserved[1] = static_cast<UINT>(address >> 32);
  D3DDDI_ALLOCATIONINFO2 info = {};
  info.pSystemMem = pixels.pData;
  info.pPrivateDriverData = &src;
  info.PrivateDriverDataSize = sizeof(src);
  D3DDDICB_ALLOCATE create = {};
  create.NumAllocations = 1;
  create.pAllocationInfo2 = &info;
  binding.pendingSource = &info;
  hr = callbacks.pfnAllocateCb(device, &create);
  binding.pendingSource = nullptr;
  if (binding.owner != this) return DXGI_ERROR_DEVICE_REMOVED;
  binding.publishSource = info.hAllocation;
  if (hr != S_OK || !info.hAllocation) {
    const HRESULT dropped = dropPublishSource(binding);
    if (binding.owner != this) return DXGI_ERROR_DEVICE_REMOVED;
    if (FAILED(dropped)) return dropped;
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG)
      return hr;
    return S_FALSE;
  }
  binding.publishAddress = pixels.pData;
  binding.publishPitch = pixels.RowPitch;
  return S_OK;
}

HRESULT RuntimeMemory::publishGpu(RuntimeAllocation& out, ID3D11DeviceContext* ctx,
                                  ID3D11Texture2D* image, SurfaceRect area) {
  auto binding = out.surface;
  const auto layout = binding->layout;
  if (area.empty()) return S_OK;
  if ((layout.pitch & 31) || binding->locked || binding->pendingLock) return S_FALSE;
  struct CopyScope {
    bool& deviceBusy;
    VidmmSurface& surface;
    ~CopyScope() { deviceBusy = false; surface.publishing = false; }
  } scope{copying, *binding};
  copying = binding->publishing = true;
  const auto destination = binding->handle;
  const auto live = [&] { return available() && binding->owner == this && binding->handle == destination; };
  HRESULT hr = ensureContext(live);
  if (!live()) return DXGI_ERROR_DEVICE_REMOVED;
  if (hr != S_OK) return unsupportedCopy(hr) ? S_FALSE : hr;
  if (!copyBuffers.pNewCommandBuffer || copyBuffers.NewCommandBufferSize < sizeof(qcdx_region_command) ||
      !copyBuffers.pNewAllocationList || copyBuffers.NewAllocationListSize < 2)
    return S_FALSE;
  if (!copyEvent) copyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!copyEvent) return S_FALSE;

  D3D11_MAPPED_SUBRESOURCE pixels = {};
  hr = getSharedImageMapping(ctx, image, true, &pixels);
  if (!live()) return DXGI_ERROR_DEVICE_REMOVED;
  if (hr != S_OK) return hr;
  const auto address = uint64_t(reinterpret_cast<uintptr_t>(pixels.pData));
  const auto bytes = uint64_t(pixels.RowPitch) * layout.height;
  const bool alternateRows = ((pixels.RowPitch | layout.pitch) & 63) != 0;
  const UINT maxPitch = 0x7fff * (alternateRows ? 32u : 64u);
  if (!address || (address & 4095) || (pixels.RowPitch & 31) || (alternateRows && layout.width + 8 > 0x4000) ||
      std::max(pixels.RowPitch, layout.pitch) > maxPitch || pixels.RowPitch < rowBytes(layout) ||
      bytes > maxSurfaceSize) return S_FALSE;

  if (!binding->publishSource || binding->publishAddress != pixels.pData || binding->publishPitch != pixels.RowPitch) {
    hr = createPublishSource(*binding, pixels);
    if (!live()) return DXGI_ERROR_DEVICE_REMOVED;
    if (hr != S_OK) return hr;
    // AllocateCb may have run new draws. Drain them before the first copy
    // from this alias and leave a changed mapping for the CPU path.
    hr = getSharedImageMapping(ctx, image, true, &pixels);
    if (!live()) return DXGI_ERROR_DEVICE_REMOVED;
    if (hr != S_OK) return hr;
    if (binding->publishAddress != pixels.pData || binding->publishPitch != pixels.RowPitch) return S_FALSE;
  }
  qcdx_region_command command = {};
  command.header.magic = QCDX_ESC_MAGIC;
  command.header.operation = QCDX_RENDER_COPY_REGION;
  command.region = {area.left, area.top, area.left, area.top, area.right - area.left, area.bottom - area.top};
  std::memcpy(copyBuffers.pNewCommandBuffer, &command, sizeof(command));
  auto list = copyBuffers.pNewAllocationList;
  list[0] = {};
  list[0].hAllocation = binding->publishSource;
  list[1] = {};
  list[1].hAllocation = destination;
  list[1].WriteOperation = TRUE;
  D3DDDICB_RENDER render = {};
  render.hContext = context;
  render.NumAllocations = 2;
  render.CommandLength = sizeof(command);
  hr = callbacks.pfnRenderCb(device, &render);
  if (!available()) return DXGI_ERROR_DEVICE_REMOVED;
  copyBuffers = render;
  if (!live()) return DXGI_ERROR_DEVICE_REMOVED;
  if (unsupportedCopy(hr)) {
    gpuPublish = false;
    const HRESULT retired = dropPublishSource(*binding);
    if (!live() || FAILED(retired)) return DXGI_ERROR_DEVICE_REMOVED;
    return S_FALSE;
  }
  if (hr != S_OK) return FAILED(hr) ? hr : E_FAIL;

  D3DDDICB_SIGNALSYNCHRONIZATIONOBJECT2 signal = {};
  signal.hContext = context;
  signal.Flags.EnqueueCpuEvent = TRUE;
  signal.CpuEventHandle = copyEvent;
  hr = callbacks.pfnSignalSynchronizationObject2Cb(device, &signal);
  if (!live()) return DXGI_ERROR_DEVICE_REMOVED;
  if (hr == S_OK && WaitForSingleObject(copyEvent, 5000) == WAIT_OBJECT_0) return S_OK;
  // The copy was accepted. Retire its source synchronously before the
  // producer can reuse it, including when the runtime rejects the signal.
  gpuPublish = false;
  const HRESULT retired = dropPublishSource(*binding);
  if (!live() || FAILED(retired)) return DXGI_ERROR_DEVICE_REMOVED;
  if (unsupportedCopy(hr)) return S_OK;
  return FAILED(hr) ? hr : DXGI_ERROR_DEVICE_HUNG;
}

HRESULT RuntimeMemory::transferImage(RuntimeAllocation& out, ID3D11DeviceContext* context,
                                    ID3D11Texture2D* image, bool publish, SurfaceTransfer region, SharedReadGuard guard) {
  if (publish) {
    const auto allocation = out.handle();
    if (!available() || !allocation) return DXGI_ERROR_DEVICE_REMOVED;
    // Submit the producer's draws before taking the compositor's allocation
    // lock. Lock2 can call back into the DDI, so map the image after it returns.
    const HRESULT hr = flushRuntimeSubmission(context);
    if (!available() || !out.handle()) return DXGI_ERROR_DEVICE_REMOVED;
    if (hr != S_OK) return hr;
    if (out.handle() != allocation) return DXGI_ERROR_WAS_STILL_DRAWING;
    if (gpuPublish && !copying && out.surface->layout.format == 21 && region.keep.empty()) {
      const HRESULT copied = publishGpu(out, context, image, out.clip(region.bounds));
      if (copied != S_FALSE) return copied;
    }
  }
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
    copyBuffers.pNewCommandBuffer = create.pCommandBuffer;
    copyBuffers.NewCommandBufferSize = create.CommandBufferSize;
    copyBuffers.pNewAllocationList = create.pAllocationList;
    copyBuffers.NewAllocationListSize = create.AllocationListSize;
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
  if (copyEvent) CloseHandle(std::exchange(copyEvent, nullptr));
  device = nullptr;
  return result;
}
}
