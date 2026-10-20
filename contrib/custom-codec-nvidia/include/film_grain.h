// Copyright 2026 Yuan Tong. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#ifndef AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_FILM_GRAIN_H
#define AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_FILM_GRAIN_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "avif/avif.h"
#include "nvEncodeAPI.h"

namespace avif_nvenc
{

// AV1 film grain synthesis parameters (section 5.9.30), in their natural (not minus-N) form.
struct film_grain_params
{
    bool chroma_scaling_from_luma = false;
    bool overlap_flag = false;
    bool clip_to_restricted_range = false;
    uint8_t scaling_shift = 8;  // 8..11
    uint8_t ar_coeff_lag = 0;   // 0..3
    uint8_t ar_coeff_shift = 6; // 6..9
    uint8_t grain_scale_shift = 0;
    uint8_t num_y_points = 0;  // <= 14
    uint8_t num_cb_points = 0; // <= 10
    uint8_t num_cr_points = 0; // <= 10
    std::array<std::array<uint8_t, 2>, 14> scaling_points_y{};
    std::array<std::array<uint8_t, 2>, 10> scaling_points_cb{};
    std::array<std::array<uint8_t, 2>, 10> scaling_points_cr{};
    std::array<int8_t, 24> ar_coeffs_y{};
    std::array<int8_t, 25> ar_coeffs_cb{};
    std::array<int8_t, 25> ar_coeffs_cr{};
    uint8_t cb_mult = 0;
    uint8_t cb_luma_mult = 0;
    uint16_t cb_offset = 0; // 9 bits
    uint8_t cr_mult = 0;
    uint8_t cr_luma_mult = 0;
    uint16_t cr_offset = 0; // 9 bits

    bool operator==(const film_grain_params & other) const = default;

    void to_nvenc(NV_ENC_FILM_GRAIN_PARAMS_AV1 * out) const;
};

// Highest value of the film-grain-dither-strength option.
constexpr int kMaxDitherGrainStrength = 24;
// Fixed seed so the same strength always produces the same grain.
constexpr uint16_t kDitherGrainSeed = 0x5eed;

// Film grain preset that dithers away banding. `strength` is 1..kMaxDitherGrainStrength.
// See film_grain.cpp for how the parameters are derived.
film_grain_params make_dither_grain(int strength, bool limited_range);

// Loads the film grain parameters that apply at time 0 from an AOM grain table file (the format
// written by `aomenc --film-grain-test`/`--film-grain-table` and read by libaom's
// aom_film_grain_table_read). Only the entry covering time 0 is parsed. `*params` is left empty
// if that entry has apply_grain == 0.
//
// The table's random_seed is returned separately in `*random_seed`: NV_ENC_FILM_GRAIN_PARAMS_AV1 has
// no field for it, so it has to be patched into the encoded bitstream (see obu_rewrite.h).
//
// Results are kept in a small LRU cache keyed by path and revalidated against the file's size and
// modification time, so the same table is not parsed again for every image.
AVIF_NODISCARD avifResult load_film_grain_table(const std::string & path,
                                                std::optional<film_grain_params> * params,
                                                uint16_t * random_seed,
                                                avifDiagnostics * diag);

} // namespace avif_nvenc

#endif // AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_FILM_GRAIN_H
