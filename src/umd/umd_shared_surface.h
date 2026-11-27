#pragma once
// SPDX-License-Identifier: MIT
#include "umd_allocation.h"
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
  RuntimeAllocation allocation;
  // The renderable copy. This is the same object as the owning Resource's
  // backend; the surface holds it so a view can outlive the resource DDI.
  Microsoft::WRL::ComPtr<ID3D11Texture2D> cache;
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
    ID3D11Texture2D* destination, ID3D11Texture2D* source) {
  if (!device || !context || !source || !destination
      || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return E_INVALIDARG;
  UnpredicatedTransfer transfer(context);
  context->CopyResource(destination, source);
  return device->GetDeviceRemovedReason();
}

// One body for both directions, for the same reason RuntimeMemory has one:
// the staging hop, the synchronized map that acts as the GPU barrier and the
// balancing unmap are identical, and only the order of the two copies differs.
inline HRESULT transferSharedSurface(ID3D11Device* device, ID3D11DeviceContext* context,
    RuntimeMemory& memory, SharedSurface& surface, bool publish) {
  if (!device || !context || !surface.cache || !surface.allocation.handle())
    return E_INVALIDARG;
  if (surface.transferring.exchange(true)) return DXGI_ERROR_WAS_STILL_DRAWING;
  struct TransferScope {
    std::atomic<bool>& active;
    ~TransferScope() { active = false; }
  } transferScope{surface.transferring};
  HRESULT hr = sharedStaging(device, surface);
  if (FAILED(hr)) return hr;
  if (publish) {
    hr = copySharedSurface(device, context, surface.staging.Get(), surface.cache.Get());
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
  hr = publish ? memory.upload(surface.allocation, mapped.pData, mapped.RowPitch)
               : memory.download(surface.allocation, mapped.pData, mapped.RowPitch);
  context->Unmap(surface.staging.Get(), 0);
  if (FAILED(hr)) return hr;
  if (!publish) return copySharedSurface(device, context, surface.cache.Get(), surface.staging.Get());
  return S_OK;
}

// Read the owning process's pixels into the cache, at most once per epoch.
inline HRESULT refreshSharedSurface(ID3D11Device* device, ID3D11DeviceContext* context,
    RuntimeMemory& memory, SharedSurface& surface, uint64_t epoch) {
  if (!sharedNeedsRefresh(surface.state, epoch)) {
    sharedRefreshed(surface.state, epoch);
    return S_OK;
  }
  const HRESULT hr = transferSharedSurface(device, context, memory, surface, false);
  if (SUCCEEDED(hr)) sharedRefreshed(surface.state, epoch);
  return hr;
}

// Write this device's pixels back out. A no-op unless the cache is dirty.
inline HRESULT publishSharedSurface(ID3D11Device* device, ID3D11DeviceContext* context,
    RuntimeMemory& memory, SharedSurface& surface, uint64_t epoch) {
  if (!surface.state.dirty) return S_OK;
  const HRESULT hr = transferSharedSurface(device, context, memory, surface, true);
  if (SUCCEEDED(hr)) sharedPublished(surface.state, epoch);
  return hr;
}

// Drop the memoized read after re-pairing this surface with a different
// allocation. See sharedInvalidate in umd_shared_policy.h for why a rotation
// that skips this shows the previous frame, and why callers must publish
// before they rotate rather than after.
inline void invalidateSharedSurface(SharedSurface& surface) {
  sharedInvalidate(surface.state);
}

}
