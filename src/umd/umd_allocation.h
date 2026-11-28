#pragma once
#include "umd_ddi.h"
#include "umd_shared_policy.h"
#include <d3d11.h>
#include "qcdx_escape.h"
#include <functional>
#include <memory>
#include <list>

namespace dxvk::umd {
class RuntimeMemory;
struct VidmmSurface {
  RuntimeMemory* owner = nullptr;
  HANDLE resource = nullptr;
  D3DKMT_HANDLE handle = 0;
  qcdx_alloc layout = {};
  D3DKMT_HANDLE publishSource = 0;
  void* publishAddress = nullptr;
  UINT publishPitch = 0;
  D3DDDI_ALLOCATIONINFO2* pendingSource = nullptr;
  bool locked = false, publishing = false;
  // Runtime callbacks can reenter teardown after assigning an output handle.
  D3DDDICB_ALLOCATE* pendingAllocate = nullptr;
  D3DDDICB_LOCK2* pendingLock = nullptr;
};
inline bool primaryAllocation(const qcdx_alloc& a) {
  return a.kind == QCDX_ALLOC_SHAREDPRIMARY || (a.flags & QCDX_ALLOC_F_PRIMARY);
}
DXGI_FORMAT allocationFormat(const qcdx_alloc& a);

class RuntimeAllocation {
public:
  RuntimeAllocation() = default;
  RuntimeAllocation(const RuntimeAllocation&) = delete;
  RuntimeAllocation& operator=(const RuntimeAllocation&) = delete;
  ~RuntimeAllocation();
  HRESULT release();
  void completeOpen(HANDLE resource) { surface->resource = resource; }
  D3DKMT_HANDLE handle() const { return surface ? surface->handle : 0; }
  bool primary() const { return surface && primaryAllocation(surface->layout); }
  SurfaceRect clip(SurfaceRect area) const {
    return surface ? area.clipped(surface->layout.width, surface->layout.height) : SurfaceRect{};
  }
  bool canRotateWith(const RuntimeAllocation& other) const;
  void swapIdentity(RuntimeAllocation& other);
private:
  friend class RuntimeMemory;
  std::shared_ptr<VidmmSurface> surface;
};

class RuntimeMemory {
public:
  RuntimeMemory() = default;
  RuntimeMemory(const RuntimeMemory&) = delete;
  RuntimeMemory& operator=(const RuntimeMemory&) = delete;
  ~RuntimeMemory();
  using Live = std::function<bool()>;
  void initialize(HANDLE device, const D3DDDI_DEVICECALLBACKS& callbacks,
                  const DXGI_DDI_BASE_CALLBACKS* dxgi, bool gpuPublish = false);
  bool available() const { return device && !closing && (dxgi ? dxgi->pfnPresentCb != nullptr : callbacks.pfnPresentCb != nullptr); }
  bool primaryAvailable() const { return available() && callbacks.pfnSetDisplayModeCb; }
  HRESULT allocate(RuntimeAllocation& out, HANDLE resource, UINT width, UINT height,
                   DXGI_FORMAT format, bool primary = false);
  HRESULT adopt(RuntimeAllocation& out, D3DKMT_HANDLE allocation, const qcdx_alloc& layout);
  HRESULT upload(RuntimeAllocation& out, const void* data, UINT pitch, SurfaceRect area = SurfaceRect::full());
  HRESULT download(RuntimeAllocation& out, void* data, UINT pitch, SurfaceTransfer region, SharedReadGuard guard);
  HRESULT transferImage(RuntimeAllocation& out, ID3D11DeviceContext* context,
                        ID3D11Texture2D* image, bool publish, SurfaceTransfer region, SharedReadGuard guard,
                        SharedReadGuard* published = nullptr);
  HRESULT setDisplayMode(RuntimeAllocation& primary, const Live& live);
  HRESULT present(RuntimeAllocation& source, const DXGI_DDI_ARG_PRESENT& args,
                  const Live& live, D3DKMT_HANDLE destination);
  HRESULT present9(RuntimeAllocation& source, const Live& live);
  HRESULT queryResidency(const D3DKMT_HANDLE* handles, UINT count,
                         D3DDDI_RESIDENCYSTATUS* status, const Live& live);
  HRESULT offer(const D3DDDICB_OFFERALLOCATIONS& request);
  HRESULT reclaim(const D3DDDICB_RECLAIMALLOCATIONS& request);
  HRESULT setPriority(D3DKMT_HANDLE allocation, UINT priority, const Live& live);
  HRESULT closeDeviceAllocations();
private:
  friend class RuntimeAllocation;
  HRESULT ensureContext(const Live& live);
  template<typename GetPixels>
  HRESULT transfer(RuntimeAllocation& out, bool publish, SurfaceTransfer region, SharedReadGuard guard, GetPixels getPixels,
                   SharedReadGuard* published = nullptr);
  HRESULT publishGpu(RuntimeAllocation& out, ID3D11DeviceContext* context, ID3D11Texture2D* image, SurfaceRect area);
  HRESULT createPublishSource(VidmmSurface& binding, const D3D11_MAPPED_SUBRESOURCE& pixels);
  HRESULT dropPublishSource(VidmmSurface& binding);
  HRESULT release(const std::shared_ptr<VidmmSurface>& surface);
  HANDLE device = nullptr, context = nullptr, copyEvent = nullptr;
  D3DDDICB_RENDER copyBuffers = {};
  bool gpuPublish = false, copying = false;
  D3DDDI_DEVICECALLBACKS callbacks = {};
  const DXGI_DDI_BASE_CALLBACKS* dxgi = nullptr;
  std::list<std::shared_ptr<VidmmSurface>> surfaces;
  D3DDDICB_CREATECONTEXT* pendingContext = nullptr;
  bool closing = false, tracing = false;
};
}
