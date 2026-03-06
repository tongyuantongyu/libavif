// Copyright 2026 Yuan Tong. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#ifndef AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_CODEC_OPTIONS_H
#define AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_CODEC_OPTIONS_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "avif/codec.h"
#include "nvEncodeAPI.h"

namespace avif_nvenc
{

enum class pool_mode
{
    on_demand,
    omni,
    bucket,
};

struct image_size
{
    uint32_t width = 0;
    uint32_t height = 0;
};

struct content_light_level
{
    uint16_t max_content_light_level = 0;
    uint16_t max_pic_average_light_level = 0;

    bool operator==(const content_light_level & other) const = default;
};

struct codec_options
{
    std::string pool_id;
    GUID preset_guid = NV_ENC_PRESET_P4_GUID;
    bool preset_was_explicit = false;
    NV_ENC_TUNING_INFO tuning = NV_ENC_TUNING_INFO_HIGH_QUALITY;
    bool tuning_was_explicit = false;
    std::optional<uint8_t> qp;
    bool spatial_aq = false;
    bool spatial_aq_was_explicit = false;
    std::optional<uint8_t> aq_strength;
    std::optional<NV_ENC_AV1_PART_SIZE> min_part_size;
    std::optional<NV_ENC_AV1_PART_SIZE> max_part_size;
    std::optional<int8_t> y_dc_qp_offset;
    std::optional<int8_t> u_dc_qp_offset;
    std::optional<int8_t> v_dc_qp_offset;
    std::optional<int8_t> cb_qp_offset;
    std::optional<int8_t> cr_qp_offset;
    NV_ENC_SPLIT_ENCODE_MODE split_encode_mode = NV_ENC_SPLIT_AUTO_MODE;
};

struct registered_pool_options
{
    std::string id;
    int gpu = 0;
    pool_mode mode = pool_mode::on_demand;
    size_t max_sessions = 1;

    // Only for `pool_mode::omni`
    std::optional<image_size> max_size;

    // Only for `pool_mode::bucket`
    std::vector<image_size> bucket_sizes;
};

inline int compare_guid(const GUID & lhs, const GUID & rhs)
{
    return std::memcmp(&lhs, &rhs, sizeof(GUID));
}

// Only immutable NVENC session allocation constraints belong in the pool key.
struct session_key
{
    int gpu = 0;
    uint32_t max_encode_width = 0;
    uint32_t max_encode_height = 0;
    uint32_t depth = 8;
    NV_ENC_BUFFER_FORMAT buffer_format = NV_ENC_BUFFER_FORMAT_UNDEFINED;

    bool operator==(const session_key & other) const;
    bool operator<(const session_key & other) const;
};

struct session_config
{
    uint32_t encode_width = 0;
    uint32_t encode_height = 0;
    uint32_t tile_columns = 0;
    uint32_t tile_rows = 0;
    bool is_alpha = false;
    uint8_t min_qp = 0;
    uint8_t max_qp = 0;
    avifColorPrimaries color_primaries = AVIF_COLOR_PRIMARIES_UNSPECIFIED;
    avifTransferCharacteristics transfer_characteristics = AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED;
    avifMatrixCoefficients matrix_coefficients = AVIF_MATRIX_COEFFICIENTS_UNSPECIFIED;
    avifRange yuv_range = AVIF_RANGE_LIMITED;
    avifChromaSamplePosition chroma_sample_position = AVIF_CHROMA_SAMPLE_POSITION_UNKNOWN;
    GUID preset_guid = NV_ENC_PRESET_P4_GUID;
    NV_ENC_TUNING_INFO tuning = NV_ENC_TUNING_INFO_HIGH_QUALITY;
    uint8_t qp = 0;
    bool spatial_aq = false;
    std::optional<uint8_t> aq_strength;
    std::optional<NV_ENC_AV1_PART_SIZE> min_part_size;
    std::optional<NV_ENC_AV1_PART_SIZE> max_part_size;
    std::optional<int8_t> y_dc_qp_offset;
    std::optional<int8_t> u_dc_qp_offset;
    std::optional<int8_t> v_dc_qp_offset;
    std::optional<int8_t> cb_qp_offset;
    std::optional<int8_t> cr_qp_offset;
    NV_ENC_SPLIT_ENCODE_MODE split_encode_mode = NV_ENC_SPLIT_AUTO_MODE;
    std::optional<content_light_level> clli;

    bool operator==(const session_config & other) const;
};

struct session_request
{
    std::string pool_id;
    uint32_t depth = 8;
    NV_ENC_BUFFER_FORMAT buffer_format = NV_ENC_BUFFER_FORMAT_UNDEFINED;
    session_config config;
};

const char * pool_mode_to_string(pool_mode mode);
AVIF_NODISCARD avifResult build_session_request(avifEncoder * encoder,
                                                const avifImage * image,
                                                avifBool alpha,
                                                int tile_rows_log2,
                                                int tile_cols_log2,
                                                int quality,
                                                avifAddImageFlags add_image_flags,
                                                session_request * request,
                                                avifDiagnostics * diag);

} // namespace avif_nvenc

#endif // AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_CODEC_OPTIONS_H
