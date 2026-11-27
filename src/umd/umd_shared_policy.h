#pragma once
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dxvk::umd {

struct SurfaceRect {
  uint32_t left = 0, top = 0, right = 0, bottom = 0;

  static constexpr SurfaceRect full() { return {0, 0, UINT32_MAX, UINT32_MAX}; }
  bool empty() const { return left >= right || top >= bottom; }
  bool isFull() const { return !left && !top && right == UINT32_MAX && bottom == UINT32_MAX; }
  bool contains(SurfaceRect r) const {
    return r.empty() || (left <= r.left && top <= r.top && right >= r.right && bottom >= r.bottom);
  }

  void include(SurfaceRect other) {
    if (other.empty()) return;
    if (empty()) { *this = other; return; }
    left = std::min(left, other.left);
    top = std::min(top, other.top);
    right = std::max(right, other.right);
    bottom = std::max(bottom, other.bottom);
  }

  SurfaceRect clipped(uint32_t width, uint32_t height) const {
    return {left, top, std::min(right, width), std::min(bottom, height)};
  }
};

// A rectangular transfer with an optional region already present in the cache.
struct SurfaceTransfer {
  SurfaceRect bounds, keep;

  template<typename Copy>
  void forEach(Copy copy) const {
    if (bounds.empty()) return;
    SurfaceRect overlap{std::max(bounds.left, keep.left), std::max(bounds.top, keep.top),
      std::min(bounds.right, keep.right), std::min(bounds.bottom, keep.bottom)};
    if (overlap.empty()) { copy(bounds); return; }
    const SurfaceRect strips[] = {
      {bounds.left, bounds.top, bounds.right, overlap.top},
      {bounds.left, overlap.bottom, bounds.right, bounds.bottom},
      {bounds.left, overlap.top, overlap.left, overlap.bottom},
      {overlap.right, overlap.top, bounds.right, overlap.bottom},
    };
    for (auto strip : strips)
      if (!strip.empty()) copy(strip);
  }
};

// Both pointers address the start of a surface. The caller clips the area to
// the common pixel dimensions before locking either image.
inline void copySurfaceRect(void* dest, size_t dstPitch, const void* source, size_t srcPitch,
    uint32_t pixelBytes, SurfaceRect area) {
  if (area.empty()) return;
  const auto x = size_t(area.left) * pixelBytes;
  const auto bytes = size_t(area.right - area.left) * pixelBytes;
  const auto rows = area.bottom - area.top;
  auto dst = static_cast<uint8_t*>(dest) + area.top * dstPitch + x;
  auto src = static_cast<const uint8_t*>(source) + area.top * srcPitch + x;
  if (srcPitch == bytes && dstPitch == bytes) {
    std::memcpy(dst, src, bytes * rows);
  } else {
    for (uint32_t y = 0; y < rows; ++y)
      std::memcpy(dst + y * dstPitch, src + y * srcPitch, bytes);
  }
}

struct SharedState {
  // Only pixels inside valid are current in this ownership epoch.
  uint64_t refreshed = 0;
  SurfaceRect valid;
  SurfaceRect damage;
  // Lock2 and Unlock2 can reenter the DDI while a publish is in progress.
  uint64_t writeSerial = 0;
};

// A runtime lock or unlock may reenter the DDI and change the cache before
// the import can commit. Validate the snapshot before touching its pixels.
struct SharedReadGuard {
  const SharedState* state = nullptr;
  uint64_t serial = 0;
  bool current() const { return !state || state->writeSerial == serial; }
};

// An ownership epoch is the boundary at which another process's writes may
// become visible, and the granularity a read is memoized against. Flush and
// Present open a new one; nothing else does.
inline uint64_t nextSharedEpoch(uint64_t epoch) { return epoch + 1; }

// On a new epoch the previous owner's pixels must be fetched again. The
// device's outstanding writes remain authoritative until it publishes them.
inline SurfaceTransfer sharedReadPlan(const SharedState& state, uint64_t epoch, SurfaceRect needed) {
  const auto valid = state.refreshed == epoch ? state.valid : state.damage;
  needed.include(valid);
  return {needed, valid};
}

// Record a completed read.
inline void sharedRefreshed(SharedState& state, uint64_t epoch, SurfaceRect area = SurfaceRect::full()) {
  state.refreshed = epoch;
  state.valid = area;
}

// A write that defines every pixel: a full-view clear, a whole-resource copy,
// a box-less update. The cache becomes authoritative with no read at all,
// which is the one case this policy is cheaper than reading before every use
// rather than merely later.
inline void sharedWroteWhole(SharedState& state, uint64_t epoch) {
  state.damage = SurfaceRect::full();
  ++state.writeSerial;
  sharedRefreshed(state, epoch);
}

// A write that leaves some pixels as the other process last wrote them: a
// region copy, a boxed update, or any draw, since blending, a partial viewport
// and a scissor all preserve what was underneath. The caller must refresh
// first; this only records the result.
inline void sharedWroteRegion(SharedState& state, SurfaceRect area) {
  if (!area.empty()) {
    state.damage.include(area);
    ++state.writeSerial;
  }
}

// A partial publish cannot validate untouched pixels from an older epoch.
inline void sharedPublished(SharedState& state, uint64_t epoch) {
  if (state.damage.isFull()) sharedRefreshed(state, epoch);
  state.damage = {};
}

// Rotation re-pairs the resource with another allocation. The caller publishes
// outstanding writes before rotating and invalidates the previous image here.
inline void sharedInvalidate(SharedState& state) {
  state.refreshed = 0;
  state.valid = {};
  state.damage = {};
  ++state.writeSerial;
}

}
