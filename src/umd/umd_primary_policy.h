#pragma once
#include <cstdint>
#include <cstddef>

namespace dxvk::umd {

// Numeric fields are deliberately separate from the Windows ABI. The native
// boundary checks these values against the original SDK constants.
struct PrimaryShape {
  uint32_t flags = 0, source = 0, width = 0, height = 0, format = 0;
  uint32_t modeWidth = 0, modeHeight = 0, modeFormat = 0;
  uint32_t numerator = 0, denominator = 0, scanline = 0, rotation = 0, scaling = 0;
};
enum class PrimaryStatus { Valid, Invalid, Unsupported };
struct PrimaryPlan {
  bool scanout = false;
  uint32_t driverFlags = 0;
};
inline PrimaryStatus primaryPlan(const PrimaryShape& shape, PrimaryPlan& output) {
  if ((shape.flags & ~15u) || !shape.width || !shape.height ||
      shape.width > 16384 || shape.height > 16384 || shape.scanline > 3 ||
      shape.rotation > 4 || shape.scaling > 2 || (shape.numerator && !shape.denominator))
    return PrimaryStatus::Invalid;
  const bool bgra = shape.format == 87 || shape.format == 88
                 || shape.format == 91 || shape.format == 93;
  const bool copy = shape.format == 28 || shape.format == 29
                 || shape.format == 10 || shape.format == 24;
  if ((!bgra && !copy) || (shape.flags & 12u)) return PrimaryStatus::Unsupported;
  // The display owns one unrotated BGRA source. An optional backbuffer
  // can still be copied when its dimensions differ from the mode.
  PrimaryPlan plan;
  // NONPREROTATED requests storage in the supplied orientation, including
  // when the output mode reports a rotation. Keep the native dimensions.
  const bool scanoutOrientation = shape.rotation <= 1 || (shape.flags & 2u);
  plan.scanout = bgra && !shape.source && shape.width == shape.modeWidth
      && shape.height == shape.modeHeight && shape.format == shape.modeFormat
      && scanoutOrientation && shape.scanline <= 1;
  if (!plan.scanout && !(shape.flags & 1u)) return PrimaryStatus::Unsupported;
  plan.driverFlags = plan.scanout ? 0 : 1;
  output = plan;
  return PrimaryStatus::Valid;
}

}
