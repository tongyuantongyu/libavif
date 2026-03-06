// Copyright 2026 Yuan Tong. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#include "codec_options.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <string>
#include <string_view>

#include "avif_nvenc_codec.h"
#include "exception.h"

#include <format>

namespace avif_nvenc
{
namespace
{

std::string ascii_lower(const std::string_view value)
{
    std::string lowered(value);
    std::ranges::transform(lowered, lowered.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered;
}

template <typename Int>
bool parse_integer(const std::string_view value, Int * parsed)
{
    if (value.empty()) {
        return false;
    }
    Int out = 0;
    const auto * begin = value.data();
    const auto * end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, out);
    if (result.ec != std::errc() || result.ptr != end) {
        return false;
    }
    *parsed = out;
    return true;
}

bool parse_preset(std::string_view value, GUID * preset_guid)
{
    const std::string lowered = ascii_lower(value);
    if (lowered == "p1") {
        *preset_guid = NV_ENC_PRESET_P1_GUID;
    } else if (lowered == "p2") {
        *preset_guid = NV_ENC_PRESET_P2_GUID;
    } else if (lowered == "p3") {
        *preset_guid = NV_ENC_PRESET_P3_GUID;
    } else if (lowered == "p4") {
        *preset_guid = NV_ENC_PRESET_P4_GUID;
    } else if (lowered == "p5") {
        *preset_guid = NV_ENC_PRESET_P5_GUID;
    } else if (lowered == "p6") {
        *preset_guid = NV_ENC_PRESET_P6_GUID;
    } else if (lowered == "p7") {
        *preset_guid = NV_ENC_PRESET_P7_GUID;
    } else {
        return false;
    }
    return true;
}

bool parse_bool(std::string_view value, bool * parsed)
{
    const std::string lowered = ascii_lower(value);
    if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") {
        *parsed = true;
    } else if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") {
        *parsed = false;
    } else {
        return false;
    }
    return true;
}

GUID preset_guid_for_speed(int speed)
{
    switch (std::clamp(speed, AVIF_SPEED_SLOWEST, AVIF_SPEED_FASTEST)) {
        case 0:
            return NV_ENC_PRESET_P7_GUID;
        case 1:
        case 2:
            return NV_ENC_PRESET_P6_GUID;
        case 3:
        case 4:
            return NV_ENC_PRESET_P5_GUID;
        case 5:
        case 6:
            return NV_ENC_PRESET_P4_GUID;
        case 7:
            return NV_ENC_PRESET_P3_GUID;
        case 8:
        case 9:
            return NV_ENC_PRESET_P2_GUID;
        case 10:
            return NV_ENC_PRESET_P1_GUID;
    }
    return NV_ENC_PRESET_P4_GUID;
}

bool parse_tuning(std::string_view value, NV_ENC_TUNING_INFO * tuning)
{
    const std::string lowered = ascii_lower(value);
    if (lowered == "hq" || lowered == "highquality") {
        *tuning = NV_ENC_TUNING_INFO_HIGH_QUALITY;
    } else if (lowered == "ll" || lowered == "lowlatency") {
        *tuning = NV_ENC_TUNING_INFO_LOW_LATENCY;
    } else if (lowered == "ull" || lowered == "ultralowlatency") {
        *tuning = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    } else if (lowered == "uhq" || lowered == "ultrahighquality") {
        *tuning = NV_ENC_TUNING_INFO_ULTRA_HIGH_QUALITY;
    } else if (lowered == "lossless") {
        *tuning = NV_ENC_TUNING_INFO_LOSSLESS;
    } else {
        return false;
    }
    return true;
}

bool parse_av1_part_size(std::string_view value, NV_ENC_AV1_PART_SIZE * part_size)
{
    const std::string lowered = ascii_lower(value);
    if (lowered == "auto") {
        *part_size = NV_ENC_AV1_PART_SIZE_AUTOSELECT;
    } else if (lowered == "4" || lowered == "4x4") {
        *part_size = NV_ENC_AV1_PART_SIZE_4x4;
    } else if (lowered == "8" || lowered == "8x8") {
        *part_size = NV_ENC_AV1_PART_SIZE_8x8;
    } else if (lowered == "16" || lowered == "16x16") {
        *part_size = NV_ENC_AV1_PART_SIZE_16x16;
    } else if (lowered == "32" || lowered == "32x32") {
        *part_size = NV_ENC_AV1_PART_SIZE_32x32;
    } else if (lowered == "64" || lowered == "64x64") {
        *part_size = NV_ENC_AV1_PART_SIZE_64x64;
    } else {
        return false;
    }
    return true;
}

bool parse_split_encode_mode(std::string_view value, NV_ENC_SPLIT_ENCODE_MODE * mode)
{
    const std::string lowered = ascii_lower(value);
    if (lowered == "0" || lowered == "auto") {
        *mode = NV_ENC_SPLIT_AUTO_MODE;
    } else if (lowered == "1" || lowered == "auto-forced" || lowered == "forced-auto" || lowered == "forced") {
        *mode = NV_ENC_SPLIT_AUTO_FORCED_MODE;
    } else if (lowered == "2" || lowered == "two" || lowered == "two-forced") {
        *mode = NV_ENC_SPLIT_TWO_FORCED_MODE;
    } else if (lowered == "3" || lowered == "three" || lowered == "three-forced") {
        *mode = NV_ENC_SPLIT_THREE_FORCED_MODE;
    } else if (lowered == "4" || lowered == "four" || lowered == "four-forced") {
        *mode = NV_ENC_SPLIT_FOUR_FORCED_MODE;
    } else if (lowered == "15" || lowered == "disabled" || lowered == "disable" || lowered == "off") {
        *mode = NV_ENC_SPLIT_DISABLE_MODE;
    } else {
        return false;
    }
    return true;
}

uint8_t clamp_qp(int qp)
{
    return static_cast<uint8_t>(std::clamp(qp, 0, 63));
}

bool parse_int8(std::string_view value, int8_t * parsed)
{
    int parsed_value = 0;
    if (!parse_integer<int>(value, &parsed_value) || parsed_value < std::numeric_limits<int8_t>::min() ||
        parsed_value > std::numeric_limits<int8_t>::max()) {
        return false;
    }
    *parsed = static_cast<int8_t>(parsed_value);
    return true;
}

void apply_encoder_defaults(const avifEncoder * encoder, codec_options * options)
{
    if ((encoder->speed != AVIF_SPEED_DEFAULT) && !options->preset_was_explicit) {
        // AVIF speed gets slower as quality improves, while NVENC presets get
        // slower as we move from P1 to P7.
        options->preset_guid = preset_guid_for_speed(encoder->speed);
    }
    if (!options->tuning_was_explicit) {
        options->tuning = NV_ENC_TUNING_INFO_HIGH_QUALITY;
    }
}

int quality_to_qp(int quality)
{
    return std::clamp(((100 - quality) * 255 + 50) / 100, 0, 255);
}

NV_ENC_BUFFER_FORMAT color_buffer_format_for(const avifImage * image)
{
    switch (image->yuvFormat) {
        case AVIF_PIXEL_FORMAT_YUV420:
            return (image->depth > 8) ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT : NV_ENC_BUFFER_FORMAT_NV12;
        case AVIF_PIXEL_FORMAT_YUV422:
            return (image->depth > 8) ? NV_ENC_BUFFER_FORMAT_P210 : NV_ENC_BUFFER_FORMAT_NV16;
        case AVIF_PIXEL_FORMAT_YUV444:
            return (image->depth > 8) ? NV_ENC_BUFFER_FORMAT_YUV444_10BIT : NV_ENC_BUFFER_FORMAT_YUV444;
        default:
            return NV_ENC_BUFFER_FORMAT_UNDEFINED;
    }
}

NV_ENC_BUFFER_FORMAT buffer_format_for(const avifImage * image, avifBool alpha)
{
    if (alpha == AVIF_TRUE) {
        // Keep alpha on the proven YUV420-compatible upload path regardless of
        // the color plane format.
        return (image->depth > 8) ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT : NV_ENC_BUFFER_FORMAT_NV12;
    }
    return color_buffer_format_for(image);
}

bool image_has_clli(const avifImage * image)
{
    return image && (image->clli.maxCLL != 0 || image->clli.maxPALL != 0);
}

content_light_level clli_for_image(const avifImage * image)
{
    return { image->clli.maxCLL, image->clli.maxPALL };
}

avifResult parse_codec_options_internal(const avifCodecSpecificOptions * cs_options, codec_options * options, avifDiagnostics * diag)
{
    if (!cs_options) {
        return AVIF_RESULT_OK;
    }
    for (uint32_t i = 0; i < cs_options->count; ++i) {
        const avifCodecSpecificOption & option = cs_options->entries[i];
        const std::string key = ascii_lower(option.key ? option.key : "");
        const std::string_view value = option.value ? std::string_view(option.value) : std::string_view();
        if (key == AVIF_NVENC_OPTION_POOL) {
            if (value.empty()) {
                set_diagnostic(diag, "Invalid NVENC pool option: {}", value);
                return AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION;
            }
            options->pool_id = std::string(value);
        } else if (key == AVIF_NVENC_OPTION_PRESET) {
            if (!parse_preset(value, &options->preset_guid)) {
                set_diagnostic(diag, "Invalid NVENC preset option: {}", value);
                return AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION;
            }
            options->preset_was_explicit = true;
        } else if (key == AVIF_NVENC_OPTION_TUNING) {
            if (!parse_tuning(value, &options->tuning)) {
                set_diagnostic(diag, "Invalid NVENC tuning option: {}", value);
                return AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION;
            }
            options->tuning_was_explicit = true;
        } else if (key == AVIF_NVENC_OPTION_QP) {
            uint32_t qp = 0;
            if (!parse_integer<uint32_t>(value, &qp) || qp > 255) {
                set_diagnostic(diag, "Invalid NVENC cq option: {}", value);
                return AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION;
            }
            options->qp = static_cast<uint8_t>(qp);
        } else if (key == AVIF_NVENC_OPTION_AQ) {
            if (!parse_bool(value, &options->spatial_aq)) {
                set_diagnostic(diag, "Invalid NVENC aq option: {}", value);
                return AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION;
            }
            options->spatial_aq_was_explicit = true;
        } else if (key == AVIF_NVENC_OPTION_AQ_STRENGTH) {
            uint32_t aq_strength = 0;
            if (!parse_integer<uint32_t>(value, &aq_strength) || aq_strength < 1 || aq_strength > 15) {
                set_diagnostic(diag, "Invalid NVENC aq-strength option: {}", value);
                return AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION;
            }
            options->aq_strength = static_cast<uint8_t>(aq_strength);
        } else if (key == AVIF_NVENC_OPTION_MIN_PART_SIZE) {
            NV_ENC_AV1_PART_SIZE part_size = NV_ENC_AV1_PART_SIZE_AUTOSELECT;
            if (!parse_av1_part_size(value, &part_size)) {
                set_diagnostic(diag, "Invalid NVENC min-part-size option: {}", value);
                return AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION;
            }
            options->min_part_size = part_size;
        } else if (key == AVIF_NVENC_OPTION_MAX_PART_SIZE) {
            NV_ENC_AV1_PART_SIZE part_size = NV_ENC_AV1_PART_SIZE_AUTOSELECT;
            if (!parse_av1_part_size(value, &part_size)) {
                set_diagnostic(diag, "Invalid NVENC max-part-size option: {}", value);
                return AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION;
            }
            options->max_part_size = part_size;
        } else if (key == AVIF_NVENC_OPTION_Y_DC_QP_OFFSET) {
            int8_t offset = 0;
            if (!parse_int8(value, &offset)) {
                set_diagnostic(diag, "Invalid NVENC y-dc-qp-offset option: {}", value);
                return AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION;
            }
            options->y_dc_qp_offset = offset;
        } else if (key == AVIF_NVENC_OPTION_U_DC_QP_OFFSET) {
            int8_t offset = 0;
            if (!parse_int8(value, &offset)) {
                set_diagnostic(diag, "Invalid NVENC u-dc-qp-offset option: {}", value);
                return AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION;
            }
            options->u_dc_qp_offset = offset;
        } else if (key == AVIF_NVENC_OPTION_V_DC_QP_OFFSET) {
            int8_t offset = 0;
            if (!parse_int8(value, &offset)) {
                set_diagnostic(diag, "Invalid NVENC v-dc-qp-offset option: {}", value);
                return AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION;
            }
            options->v_dc_qp_offset = offset;
        } else if (key == AVIF_NVENC_OPTION_CB_QP_OFFSET) {
            int8_t offset = 0;
            if (!parse_int8(value, &offset)) {
                set_diagnostic(diag, "Invalid NVENC cb-qp-offset option: {}", value);
                return AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION;
            }
            options->cb_qp_offset = offset;
        } else if (key == AVIF_NVENC_OPTION_CR_QP_OFFSET) {
            int8_t offset = 0;
            if (!parse_int8(value, &offset)) {
                set_diagnostic(diag, "Invalid NVENC cr-qp-offset option: {}", value);
                return AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION;
            }
            options->cr_qp_offset = offset;
        } else if (key == AVIF_NVENC_OPTION_SPLIT_ENCODE_MODE) {
            if (!parse_split_encode_mode(value, &options->split_encode_mode)) {
                set_diagnostic(diag, "Invalid NVENC split-encode-mode option: {}", value);
                return AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION;
            }
        } else {
            set_diagnostic(diag, "Unknown NVENC codec-specific option: {}", option.key ? option.key : "<null>");
            return AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION;
        }
    }
    if (options->aq_strength.has_value()) {
        if (options->spatial_aq_was_explicit && !options->spatial_aq) {
            set_diagnostic(diag, "NVENC aq-strength requires aq to be enabled");
            return AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION;
        }
        options->spatial_aq = true;
    }
    if (options->min_part_size.has_value() && options->max_part_size.has_value() &&
        options->min_part_size.value() != NV_ENC_AV1_PART_SIZE_AUTOSELECT &&
        options->max_part_size.value() != NV_ENC_AV1_PART_SIZE_AUTOSELECT &&
        static_cast<int>(options->min_part_size.value()) > static_cast<int>(options->max_part_size.value())) {
        set_diagnostic(diag, "NVENC min-part-size cannot exceed max-part-size");
        return AVIF_RESULT_INVALID_CODEC_SPECIFIC_OPTION;
    }
    return AVIF_RESULT_OK;
}

} // namespace

bool session_key::operator==(const session_key & other) const
{
    return gpu == other.gpu && max_encode_width == other.max_encode_width && max_encode_height == other.max_encode_height &&
           depth == other.depth && buffer_format == other.buffer_format;
}

bool session_key::operator<(const session_key & other) const
{
    if (gpu != other.gpu) {
        return gpu < other.gpu;
    }
    if (max_encode_width != other.max_encode_width) {
        return max_encode_width < other.max_encode_width;
    }
    if (max_encode_height != other.max_encode_height) {
        return max_encode_height < other.max_encode_height;
    }
    if (depth != other.depth) {
        return depth < other.depth;
    }
    if (buffer_format != other.buffer_format) {
        return buffer_format < other.buffer_format;
    }
    return false;
}

bool session_config::operator==(const session_config & other) const
{
    return encode_width == other.encode_width && encode_height == other.encode_height && tile_columns == other.tile_columns &&
           tile_rows == other.tile_rows && is_alpha == other.is_alpha && min_qp == other.min_qp && max_qp == other.max_qp &&
           color_primaries == other.color_primaries && transfer_characteristics == other.transfer_characteristics &&
           matrix_coefficients == other.matrix_coefficients && yuv_range == other.yuv_range &&
           chroma_sample_position == other.chroma_sample_position && compare_guid(preset_guid, other.preset_guid) == 0 &&
           tuning == other.tuning && qp == other.qp && spatial_aq == other.spatial_aq && aq_strength == other.aq_strength &&
           min_part_size == other.min_part_size && max_part_size == other.max_part_size &&
           y_dc_qp_offset == other.y_dc_qp_offset && u_dc_qp_offset == other.u_dc_qp_offset &&
           v_dc_qp_offset == other.v_dc_qp_offset && cb_qp_offset == other.cb_qp_offset &&
           cr_qp_offset == other.cr_qp_offset && split_encode_mode == other.split_encode_mode && clli == other.clli;
}

const char * pool_mode_to_string(pool_mode mode)
{
    switch (mode) {
        case pool_mode::on_demand:
            return "on-demand";
        case pool_mode::omni:
            return "omni";
        case pool_mode::bucket:
            return "bucket";
    }
    return "unknown";
}

avifResult build_session_request(avifEncoder * encoder,
                                 const avifImage * image,
                                 avifBool alpha,
                                 int tile_rows_log2,
                                 int tile_cols_log2,
                                 int quality,
                                 avifAddImageFlags add_image_flags,
                                 session_request * request,
                                 avifDiagnostics * diag)
{
    if (!(add_image_flags & AVIF_ADD_IMAGE_FLAG_SINGLE)) {
        set_diagnostic(diag, "NVENC custom codec MVP supports still images only");
        return AVIF_RESULT_NOT_IMPLEMENTED;
    }
    if (encoder->extraLayerCount > 0) {
        set_diagnostic(diag, "NVENC custom codec MVP does not support layered images");
        return AVIF_RESULT_NOT_IMPLEMENTED;
    }
    const NV_ENC_BUFFER_FORMAT buffer_format = buffer_format_for(image, alpha);
    if (buffer_format == NV_ENC_BUFFER_FORMAT_UNDEFINED) {
        set_diagnostic(diag, "NVENC custom codec supports YUV420, YUV422, and YUV444 input only");
        return AVIF_RESULT_NOT_IMPLEMENTED;
    }
    if (image->depth != 8 && image->depth != 10) {
        set_diagnostic(diag, "NVENC custom codec MVP supports 8-bit and 10-bit images only");
        return AVIF_RESULT_NOT_IMPLEMENTED;
    }
    if (alpha && !image->alphaPlane) {
        set_diagnostic(diag, "NVENC custom codec alpha encode requires an alpha plane");
        return AVIF_RESULT_NO_CONTENT;
    }

    codec_options options;
    const avifResult options_result = parse_codec_options_internal(encoder->csOptions, &options, diag);
    if (options_result != AVIF_RESULT_OK) {
        return options_result;
    }
    apply_encoder_defaults(encoder, &options);
    request->pool_id = options.pool_id;
    request->depth = image->depth;
    request->buffer_format = buffer_format;
    request->config.encode_width = image->width;
    request->config.encode_height = image->height;
    request->config.tile_columns = 1u << std::clamp(tile_cols_log2, 0, 6);
    request->config.tile_rows = 1u << std::clamp(tile_rows_log2, 0, 6);
    request->config.is_alpha = (alpha == AVIF_TRUE);
    // NVENC AV1 QP range is 0-255
    request->config.min_qp = clamp_qp(alpha ? encoder->minQuantizerAlpha : encoder->minQuantizer) * 4;
    request->config.max_qp = clamp_qp(alpha ? encoder->maxQuantizerAlpha : encoder->maxQuantizer) * 4;
    request->config.color_primaries = alpha ? AVIF_COLOR_PRIMARIES_UNSPECIFIED : image->colorPrimaries;
    request->config.transfer_characteristics =
        alpha ? AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED : image->transferCharacteristics;
    request->config.matrix_coefficients = alpha ? AVIF_MATRIX_COEFFICIENTS_UNSPECIFIED : image->matrixCoefficients;
    request->config.yuv_range = alpha ? AVIF_RANGE_FULL : image->yuvRange;
    request->config.chroma_sample_position =
        alpha ? AVIF_CHROMA_SAMPLE_POSITION_UNKNOWN : image->yuvChromaSamplePosition;
    request->config.preset_guid = options.preset_guid;
    request->config.tuning = options.tuning;
    request->config.qp = options.qp.value_or(quality_to_qp(quality));
    request->config.spatial_aq = options.spatial_aq;
    request->config.aq_strength = options.aq_strength;
    request->config.min_part_size = options.min_part_size;
    request->config.max_part_size = options.max_part_size;
    request->config.y_dc_qp_offset = options.y_dc_qp_offset;
    request->config.u_dc_qp_offset = options.u_dc_qp_offset;
    request->config.v_dc_qp_offset = options.v_dc_qp_offset;
    request->config.cb_qp_offset = options.cb_qp_offset;
    request->config.cr_qp_offset = options.cr_qp_offset;
    request->config.split_encode_mode = options.split_encode_mode;
    if (!request->config.is_alpha && image_has_clli(image)) {
        request->config.clli = clli_for_image(image);
    } else {
        request->config.clli.reset();
    }
    return AVIF_RESULT_OK;
}

} // namespace avif_nvenc
