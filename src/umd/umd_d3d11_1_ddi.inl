// D3D11.1 immediate-context entries, including the discard issued by DWM.
template<bool Extended, dxvk::umd::ShaderStage Stage>
constexpr auto constantBufferEntry() {
  if constexpr (Extended) return deviceEntry<setConstantBuffers1<Stage>>;
  else return deviceEntry<setConstantBuffers<Stage>>;
}

void APIENTRY checkDirectFlip(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, D3D10DDI_HRESOURCE, UINT, BOOL* supported) {
  if (supported) *supported = FALSE;
}

void APIENTRY discardView(D3D10DDI_HDEVICE h, D3D11DDI_HANDLETYPE type, void* handle,
    const RECT* rects, UINT count) {
  // Discard is a hint. Only a full shared RTV needs to bypass a VidMM import.
  if (type != D3D10DDI_HT_RENDERTARGETVIEW) return;
  auto device = get(h);
  auto target = static_cast<RenderTarget*>(handle);
  if (!owned(device, target)) return;
  const auto surface = target->shared;
  if (!surface || surface->transferring || device->rotationActive || device->suppressCommands || (count && !rects)) return;
  const auto extent = surface->allocation.clip(SurfaceRect::full());
  if (extent.empty()) return;
  if (count) {
    const auto covers = [&](const RECT& r) {
      return r.left <= 0 && r.top <= 0 && r.right >= LONG(extent.right) && r.bottom >= LONG(extent.bottom);
    };
    if (!std::any_of(rects, rects + count, covers)) return;
  }
  // A rotated cache may still contain another allocation's pixels. Start
  // with a defined private image; discard alone owes no write to VidMM.
  const FLOAT zero[4] = {};
  const auto view = target->backend;
  const auto context = device->context;
  const auto epoch = device->sharedEpoch;
  const dxvk::umd::SharedReadGuard guard{&surface->state, surface->state.writeSerial};
  context->ClearRenderTargetView(view.Get(), zero);
  if (device->retired || epoch != device->sharedEpoch || !guard.current()) return;
  ++surface->state.writeSerial;
  surface->state.damage = {};
  dxvk::umd::sharedRefreshed(surface->state, device->sharedEpoch);
}

void APIENTRY clearView1(D3D10DDI_HDEVICE h, D3D11DDI_HANDLETYPE type, void* handle,
    const FLOAT color[4], const RECT* rects, UINT count) {
  auto device = get(h);
  if (!handle || !color || (count && !rects)) { device->error(E_INVALIDARG); return; }
  const auto context = device->context1;
  ComPtr<ID3D11View> view;
  if (type == D3D10DDI_HT_RENDERTARGETVIEW) {
    auto target = static_cast<RenderTarget*>(handle);
    if (!owned(device, target)) return;
    view = target->backend;
    const auto surface = target->shared;
    if (surface && count) {
      SurfaceRect bounds;
      for (UINT i = 0; i < count; ++i) {
        const auto& r = rects[i];
        if (r.right > 0 && r.bottom > 0)
          bounds.include({UINT(std::max(0L, r.left)), UINT(std::max(0L, r.top)), UINT(r.right), UINT(r.bottom)});
      }
      if (!wroteSharedRegion(device, surface, surface->allocation.clip(bounds))) return;
    } else wroteSharedSurface(device, surface);
  } else if (type == D3D10DDI_HT_DEPTHSTENCILVIEW) {
    auto depth = static_cast<DepthView*>(handle);
    if (depth->owner == device) view = depth->backend;
  } else if (type == D3D11DDI_HT_UNORDEREDACCESSVIEW) {
    auto uav = static_cast<UnorderedView*>(handle);
    if (uav->owner == device) view = uav->backend;
  }
  if (!view) { device->error(E_INVALIDARG); return; }
  if (!device->retired) context->ClearView(view.Get(), color, rects, count);
}

template<typename Request>
HRESULT APIENTRY offerOrReclaim(Request* args) {
  if (!args || !args->hDevice || !args->Resources || !args->pResources) return E_INVALIDARG;
  const auto request = *args;
  DeviceOperation operation(reinterpret_cast<void*>(request.hDevice));
  if (!operation.owner) return DXGI_ERROR_DEVICE_REMOVED;
  dxvk::umd::RuntimeService::Scope scope(operation.owner->service.get());
  try {
    return operation.owner->service->run([&]() -> HRESULT {
      DeviceOperation worker(operation.storage, operation.owner);
      auto device = operation.owner.get();
      std::vector<AllocationParticipant> participants;
      HRESULT hr = captureAllocations(device, request.pResources, request.Resources, participants, true);
      if (hr != S_OK) return hr;
      std::vector<D3DKMT_HANDLE> handles;
      for (const auto& p : participants) {
        if (p.handle) handles.push_back(p.handle);
      }
      std::vector<BOOL> discarded;
      if (!handles.empty()) {
        const UINT count = UINT(handles.size());
        if constexpr (std::is_same_v<Request, DXGI_DDI_ARG_OFFERRESOURCES>) {
          hr = device->memory.offer({nullptr, handles.data(), count, request.Priority});
        } else {
          discarded.resize(count);
          hr = device->memory.reclaim({nullptr, handles.data(), discarded.data(), count});
        }
        if (hr != S_OK) return hr;
      }
      std::lock_guard<std::mutex> lock(resourceStorageMutex);
      if (!allocationsLiveLocked(device, participants)) return DXGI_ERROR_DEVICE_REMOVED;
      if constexpr (std::is_same_v<Request, DXGI_DDI_ARG_RECLAIMRESOURCES>) {
        size_t nextHandle = 0;
        for (size_t i = 0; i < participants.size(); ++i) {
          const auto& p = participants[i];
          const BOOL lost = p.handle ? discarded[nextHandle++] : FALSE;
          auto resource = static_cast<Resource*>(p.storage);
          if (lost && resource->shared) dxvk::umd::invalidateSharedSurface(*resource->shared);
          if (request.pDiscarded) request.pDiscarded[i] = lost;
        }
      }
      return S_OK;
    });
  } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}

HRESULT APIENTRY overlayCaps(DXGI_DDI_ARG_GETMULTIPLANEOVERLAYCAPS* args) {
  if (!args) return E_INVALIDARG;
  args->MultiplaneOverlayCaps = {};
  return S_OK;
}

HRESULT APIENTRY checkOverlay(DXGI_DDI_ARG_CHECKMULTIPLANEOVERLAYSUPPORT* args) {
  if (!args) return E_INVALIDARG;
  args->Supported = FALSE;
  return S_OK;
}
