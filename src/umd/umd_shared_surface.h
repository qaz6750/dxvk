#pragma once
// SPDX-License-Identifier: MIT
#include "umd_allocation.h"
#include "umd_api.h"
#include "umd_shared_policy.h"
#include "umd_private_transfer.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <atomic>

namespace dxvk::umd {

// A shared texture has a renderable Turnip image and a linear VidMM allocation
// that other D3D devices can open. Publish dirty pixels at handoff and refresh
// the renderable image on the first read in each ownership epoch.
struct SharedSurface {
  // The renderable copy. This is the same object as the owning Resource's
  // backend; the surface holds it so a view can outlive the resource DDI.
  Microsoft::WRL::ComPtr<ID3D11Texture2D> cache;
  RuntimeAllocation allocation;
  // CPU-visible transfer texture, created on first use.
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  // When this cache last read the allocation and whether it owes it a write.
  // The rules live in umd_shared_policy.h, where they are tested without a
  // device; this struct only carries them alongside the D3D objects.
  SharedState state;
  // A runtime LockCb can reenter Flush/Resolve while this staging image is
  // mapped. Reject that second transfer instead of recursively mapping it.
  std::atomic<bool> transferring{false};
};

// Lazily create the transfer buffer matching the cache.
inline HRESULT sharedStaging(ID3D11Device* device, SharedSurface& surface) {
  if (surface.staging) return S_OK;
  if (!device || !surface.cache) return E_INVALIDARG;
  D3D11_TEXTURE2D_DESC desc = {};
  surface.cache->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.MiscFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
  return device->CreateTexture2D(&desc, nullptr, &surface.staging);
}

// Ownership transfers are internal work, independent of an application's
// predicate and bindings. Keep Map on the immediate context: deferred Map
// cannot read/write STAGING, and the synchronized Map is our completion barrier.
inline HRESULT copySharedSurface(ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11Texture2D* destination, ID3D11Texture2D* source, SurfaceTransfer region) {
  if (!device || !context || !source || !destination
      || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return E_INVALIDARG;
  UnpredicatedTransfer transfer(context);
  region.forEach([&](SurfaceRect area) {
    const D3D11_BOX box{area.left, area.top, 0, area.right, area.bottom, 1};
    context->CopySubresourceRegion(destination, 0, area.left, area.top, 0, source, 0, &box);
  });
  return device->GetDeviceRemovedReason();
}

// One body for both directions, for the same reason RuntimeMemory has one:
// the staging hop, the synchronized map that acts as the GPU barrier and the
// balancing unmap are identical, and only the order of the two copies differs.
inline HRESULT transferSharedSurface(ID3D11Device* device, ID3D11DeviceContext* context,
    RuntimeMemory& memory, SharedSurface& surface, bool publish, SurfaceTransfer region, SharedReadGuard guard = {}) {
  if (!device || !context || !surface.cache || !surface.allocation.handle())
    return E_INVALIDARG;
  region.bounds = surface.allocation.clip(region.bounds);
  if (region.keep.contains(region.bounds)) return S_OK;
  if (surface.transferring.exchange(true)) return DXGI_ERROR_WAS_STILL_DRAWING;
  struct TransferScope {
    std::atomic<bool>& active;
    ~TransferScope() { active = false; }
  } transferScope{surface.transferring};
  if (hasSharedImageMapping(surface.cache.Get())) {
    HRESULT hr = memory.transferImage(surface.allocation, context, surface.cache.Get(), publish, region, guard);
    if (hr == S_OK && publish) {
      static const bool reported = [] {
        OutputDebugStringA("[qcdx_dxvk] Published mapped linear image to VidMM\n");
        return true;
      }();
      (void)reported;
    }
    return hr;
  }
  HRESULT hr = sharedStaging(device, surface);
  if (FAILED(hr)) return hr;
  if (publish) {
    hr = copySharedSurface(device, context, surface.staging.Get(), surface.cache.Get(), region);
    if (FAILED(hr)) return hr;
  }
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  // The synchronized map is the completion barrier in both directions: it
  // waits for the readback just queued. A refresh write preserves the staging
  // bytes still consumed by an earlier GPU read; the backend can wait or
  // rename that storage. STAGING cannot take MAP_WRITE_DISCARD, so keep the
  // ordinary synchronized Map and its ordering/storage-isolation contract.
  hr = context->Map(surface.staging.Get(), 0,
    publish ? D3D11_MAP_READ : D3D11_MAP_WRITE, 0, &mapped);
  if (FAILED(hr)) return hr;
  hr = publish ? memory.upload(surface.allocation, mapped.pData, mapped.RowPitch, region.bounds)
               : memory.download(surface.allocation, mapped.pData, mapped.RowPitch, region, guard);
  context->Unmap(surface.staging.Get(), 0);
  if (hr != S_OK) return hr;
  if (!guard.current()) return DXGI_ERROR_WAS_STILL_DRAWING;
  if (!publish) return copySharedSurface(device, context, surface.cache.Get(), surface.staging.Get(), region);
  return S_OK;
}

// Write this device's pixels back out. A no-op unless the cache is dirty.
inline HRESULT publishSharedSurface(ID3D11Device* device, ID3D11DeviceContext* context,
    RuntimeMemory& memory, SharedSurface& surface, uint64_t epoch) {
  if (surface.state.damage.empty()) return S_OK;
  // Replay a write made by a lock callback once. If callbacks keep writing,
  // leave the accumulated damage intact and let the caller retry the handoff.
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    const auto serial = surface.state.writeSerial;
    const HRESULT hr = transferSharedSurface(device, context, memory, surface, true, {surface.state.damage, {}});
    if (hr != S_OK) return hr;
    if (serial == surface.state.writeSerial) {
      sharedPublished(surface.state, epoch);
      return S_OK;
    }
  }
  return DXGI_ERROR_WAS_STILL_DRAWING;
}

// Grow the valid rectangle on demand. Never replace pixels already drawn by
// this device. All missing strips share one allocation lock and image map.
inline HRESULT refreshSharedSurface(ID3D11Device* device, ID3D11DeviceContext* context,
    RuntimeMemory& memory, SharedSurface& surface, uint64_t epoch, SurfaceRect needed = SurfaceRect::full()) {
  needed = surface.allocation.clip(needed);
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    const auto region = sharedReadPlan(surface.state, epoch, needed);
    if (region.keep.contains(region.bounds)) return S_OK;
    SharedReadGuard guard{&surface.state, surface.state.writeSerial};
    const HRESULT hr = transferSharedSurface(device, context, memory, surface, false, region, guard);
    if (!guard.current() && (hr == S_OK || hr == DXGI_ERROR_WAS_STILL_DRAWING)) continue;
    if (hr == S_OK) sharedRefreshed(surface.state, epoch, region.bounds);
    return hr;
  }
  return DXGI_ERROR_WAS_STILL_DRAWING;
}

// Drop the memoized read after re-pairing this surface with a different
// allocation. See sharedInvalidate in umd_shared_policy.h for why a rotation
// that skips this shows the previous frame, and why callers must publish
// before they rotate rather than after.
inline void invalidateSharedSurface(SharedSurface& surface) {
  sharedInvalidate(surface.state);
}

}
