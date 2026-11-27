#pragma once
// SPDX-License-Identifier: MIT
#include <cstdint>

namespace dxvk::umd {

struct SharedState {
  // Epoch whose contents the cache holds. Zero means never read, which no live
  // epoch equals, so a surface starts out owing a read.
  uint64_t refreshed = 0;
  // The cache holds writes the allocation has not seen.
  bool dirty = false;
};

// An ownership epoch is the boundary at which another process's writes may
// become visible, and the granularity a read is memoized against. Flush and
// Present open a new one; nothing else does.
inline uint64_t nextSharedEpoch(uint64_t epoch) { return epoch + 1; }

// Does a read of this surface have to go back to the allocation?
//
// No when the cache was already read this epoch, and no when the cache is
// dirty -- unpublished local writes are strictly newer than the allocation, so
// reading it back would discard them. That second case is why dirty is checked
// here and not only at publish time.
inline bool sharedNeedsRefresh(const SharedState& state, uint64_t epoch) {
  return state.refreshed != epoch && !state.dirty;
}

// Record a completed read.
inline void sharedRefreshed(SharedState& state, uint64_t epoch) {
  state.refreshed = epoch;
}

// A write that defines every pixel: a full-view clear, a whole-resource copy,
// a box-less update. The cache becomes authoritative with no read at all,
// which is the one case this policy is cheaper than reading before every use
// rather than merely later.
inline void sharedWroteWhole(SharedState& state, uint64_t epoch) {
  state.dirty = true;
  state.refreshed = epoch;
}

// A write that leaves some pixels as the other process last wrote them: a
// region copy, a boxed update, or any draw, since blending, a partial viewport
// and a scissor all preserve what was underneath. The caller must refresh
// first; this only records the result.
inline void sharedWroteRegion(SharedState& state) { state.dirty = true; }

// Record a completed publish. Cache and allocation now agree, so this epoch
// owes no read.
inline void sharedPublished(SharedState& state, uint64_t epoch) {
  state.dirty = false;
  state.refreshed = epoch;
}

// Rotation re-pairs the resource with another allocation. The caller publishes
// outstanding writes before rotating and invalidates the previous image here.
inline void sharedInvalidate(SharedState& state) {
  state.refreshed = 0;
  state.dirty = false;
}

}
