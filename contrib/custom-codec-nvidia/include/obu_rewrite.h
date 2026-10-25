// Copyright 2026 Yuan Tong. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#ifndef AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_OBU_REWRITE_H
#define AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_OBU_REWRITE_H

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "avif/avif.h"

namespace avif_nvenc
{

// Normal AV1 headers are preserved. Only layer signalling, maximum dimensions,
// and optional per-layer deterministic grain seeds are rewritten; tile bytes are intact.
AVIF_NODISCARD avifResult rewrite_layered_picture(const std::vector<std::vector<uint8_t>> & packets,
                                                  const std::vector<std::pair<uint32_t, uint32_t>> & sizes,
                                                  uint32_t final_width,
                                                  uint32_t final_height,
                                                  const std::vector<std::optional<uint16_t>> & grain_seeds,
                                                  std::vector<std::vector<uint8_t>> * output,
                                                  avifDiagnostics * diag);

// Rewrites a single-frame AV1 bitstream (low overhead format, as produced by NVENC) so that its
// sequence header uses reduced_still_picture_header. The input must contain exactly one shown
// key frame. `width`/`height` are the actual frame size; they replace max_frame_width/height of
// the sequence header (which NVENC sets to the session's bucket size) and must match the size
// coded in the frame header. Tile data is copied verbatim.
//
// If the frame has film grain parameters and `grain_seed` is set, grain_seed is replaced by it.
// NVENC picks its own seed (libaom's sequence 7391, +3381 per frame since the last reconfigure),
// which makes the output depend on session history.
AVIF_NODISCARD avifResult rewrite_to_reduced_still_picture(const std::vector<uint8_t> & in,
                                                           uint32_t width,
                                                           uint32_t height,
                                                           std::optional<uint16_t> grain_seed,
                                                           std::vector<uint8_t> * out,
                                                           avifDiagnostics * diag);

} // namespace avif_nvenc

#endif // AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_OBU_REWRITE_H
