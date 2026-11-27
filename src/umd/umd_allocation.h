#pragma once
#include "umd_ddi.h"
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
  bool locked = false;
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
  bool canRotateWith(const RuntimeAllocation& other) const;
  void swapIdentity(RuntimeAllocation& other);
private:
  friend class RuntimeMemory;
  std::shared_ptr<VidmmSurface> surface;
};

class RuntimeMemory {
public:
  using Live = std::function<bool()>;
  void initialize(HANDLE device, const D3DDDI_DEVICECALLBACKS& callbacks,
                  const DXGI_DDI_BASE_CALLBACKS* dxgi);
  bool available() const { return device && !closing && (dxgi ? dxgi->pfnPresentCb != nullptr : callbacks.pfnPresentCb != nullptr); }
  bool primaryAvailable() const { return available() && callbacks.pfnSetDisplayModeCb; }
  HRESULT allocate(RuntimeAllocation& out, HANDLE resource, UINT width, UINT height,
                   DXGI_FORMAT format, bool primary = false);
  HRESULT adopt(RuntimeAllocation& out, D3DKMT_HANDLE allocation, const qcdx_alloc& layout);
  HRESULT upload(RuntimeAllocation& out, const void* data, UINT pitch);
  HRESULT download(RuntimeAllocation& out, void* data, UINT pitch);
  HRESULT setDisplayMode(RuntimeAllocation& primary, const Live& live);
  HRESULT present(RuntimeAllocation& source, const DXGI_DDI_ARG_PRESENT& args,
                  const Live& live, D3DKMT_HANDLE destination);
  HRESULT present9(RuntimeAllocation& source, const Live& live);
  HRESULT queryResidency(const D3DKMT_HANDLE* handles, UINT count,
                         D3DDDI_RESIDENCYSTATUS* status, const Live& live);
  HRESULT setPriority(D3DKMT_HANDLE allocation, UINT priority, const Live& live);
  HRESULT closeDeviceAllocations();
private:
  friend class RuntimeAllocation;
  HRESULT ensureContext(const Live& live);
  HRESULT transfer(RuntimeAllocation& out, void* data, UINT pitch, bool upload);
  HRESULT release(const std::shared_ptr<VidmmSurface>& surface);
  HANDLE device = nullptr, context = nullptr;
  D3DDDI_DEVICECALLBACKS callbacks = {};
  const DXGI_DDI_BASE_CALLBACKS* dxgi = nullptr;
  std::list<std::shared_ptr<VidmmSurface>> surfaces;
  D3DDDICB_CREATECONTEXT* pendingContext = nullptr;
  bool closing = false;
};
}
