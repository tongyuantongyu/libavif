// Copyright 2026 Yuan Tong. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#include "obu_rewrite.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>

#include "exception.h"

// Section numbers refer to the AV1 bitstream specification.

namespace avif_nvenc
{

namespace
{

constexpr uint32_t kObuSequenceHeader = 1;
constexpr uint32_t kObuFrameHeader = 3;
constexpr uint32_t kObuFrame = 6;
constexpr uint32_t kObuRedundantFrameHeader = 7;

constexpr uint32_t kSelectScreenContentTools = 2;
constexpr uint32_t kSelectIntegerMv = 2;

constexpr uint32_t kMaxTileWidth = 4096;
constexpr uint32_t kMaxTileArea = 4096 * 2304;
constexpr uint32_t kMaxTileRows = 64;
constexpr uint32_t kMaxTileCols = 64;

constexpr int kMaxSegments = 8;
constexpr int kSegLvlMax = 8;
constexpr int kSegLvlAltQ = 0;
constexpr int kTotalRefsPerFrame = 8;

class bit_reader
{
public:
    bit_reader(const uint8_t * data, size_t size) : data_(data), size_bits_(size * 8) {}

    uint32_t read(int n)
    {
        if (pos_ + static_cast<size_t>(n) > size_bits_) {
            throw msg_exception("AV1 bitstream truncated");
        }
        uint64_t v = 0;
        for (int i = 0; i < n; ++i, ++pos_) {
            v = (v << 1) | ((data_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1);
        }
        return static_cast<uint32_t>(v);
    }

    [[nodiscard]] size_t pos() const { return pos_; }

private:
    const uint8_t * data_;
    size_t size_bits_;
    size_t pos_ = 0;
};

class bit_writer
{
public:
    void put(uint32_t v, int n)
    {
        for (int i = n - 1; i >= 0; --i) {
            if ((nbits_ & 7) == 0) {
                bytes_.push_back(0);
            }
            bytes_.back() |= static_cast<uint8_t>(((v >> i) & 1) << (7 - (nbits_ & 7)));
            ++nbits_;
        }
    }

    void trailing_bits()
    {
        put(1, 1);
        byte_align();
    }

    void byte_align() { nbits_ = (nbits_ + 7) & ~static_cast<size_t>(7); }

    std::vector<uint8_t> & bytes() { return bytes_; }

private:
    std::vector<uint8_t> bytes_;
    size_t nbits_ = 0;
};

// Reads from the source bitstream and mirrors every bit to the destination.
struct tee
{
    bit_reader & r;
    bit_writer & w;

    uint32_t copy(int n)
    {
        const uint32_t v = r.read(n);
        w.put(v, n);
        return v;
    }

    // Copies any number of bits, e.g. a whole array of film grain points.
    void copy_bulk(int n)
    {
        for (; n > 16; n -= 16) {
            copy(16);
        }
        copy(n);
    }

    // ns(n), section 4.10.7
    uint32_t copy_ns(uint32_t n)
    {
        int bits = 0;
        for (uint32_t x = n; x != 0; x >>= 1) {
            ++bits;
        }
        const uint32_t m = (1u << bits) - n;
        const uint32_t v = copy(bits - 1);
        if (v < m) {
            return v;
        }
        return (v << 1) - m + copy(1);
    }
};

int bit_length(uint32_t v)
{
    int n = 0;
    for (; v != 0; v >>= 1) {
        ++n;
    }
    return n;
}

int tile_log2(uint32_t blk_size, uint32_t target)
{
    int k = 0;
    while ((static_cast<uint64_t>(blk_size) << k) < target) {
        ++k;
    }
    return k;
}

int32_t sign_extend(uint32_t v, int n)
{
    return (v & (1u << (n - 1))) ? static_cast<int32_t>(v) - (1 << n) : static_cast<int32_t>(v);
}

uint32_t read_uvlc(bit_reader & r)
{
    int leading_zeros = 0;
    while (!r.read(1)) {
        if (++leading_zeros >= 32) {
            return UINT32_MAX;
        }
    }
    return r.read(leading_zeros) + ((1u << leading_zeros) - 1);
}

struct sequence_info
{
    bool still_picture = false;
    size_t operating_point_pos = 0;
    uint32_t operating_point_count = 0;
    size_t max_width_pos = 0;
    size_t max_height_pos = 0;
    bool reduced_still_picture_header = false;
    int width_bits = 0;
    int height_bits = 0;
    uint32_t max_width = 0;
    uint32_t max_height = 0;
    bool frame_id_numbers_present = false;
    int id_len = 0;
    int delta_id_len = 0;
    bool enable_warped_motion = false;
    bool enable_ref_frame_mvs = false;
    bool use_128x128 = false;
    bool enable_superres = false;
    bool enable_cdef = false;
    bool enable_restoration = false;
    uint32_t force_screen_content_tools = kSelectScreenContentTools;
    uint32_t force_integer_mv = kSelectIntegerMv;
    int order_hint_bits = 0;
    bool mono_chrome = false;
    bool subsampling_x = false;
    bool subsampling_y = false;
    bool separate_uv_delta_q = false;
    bool film_grain_params_present = false;

    [[nodiscard]] int num_planes() const { return mono_chrome ? 1 : 3; }
};

// Section 5.5.2. Parses `r` and writes an equivalent sequence header using
// reduced_still_picture_header to `w`. max_frame_width/height become width x height.
// Nothing is written if the input already is reduced.
void rewrite_sequence_header(bit_reader & r, bit_writer & w, uint32_t width, uint32_t height, sequence_info * seq)
{
    const uint32_t seq_profile = r.read(3);
    seq->still_picture = r.read(1);
    seq->reduced_still_picture_header = r.read(1);
    if (seq->reduced_still_picture_header) {
        return;
    }

    uint32_t seq_level_idx0 = 0;
    if (r.read(1)) {     // timing_info_present_flag
        r.read(32);      // num_units_in_display_tick
        r.read(32);      // time_scale
        if (r.read(1)) { // equal_picture_interval
            read_uvlc(r);
        }
        if (r.read(1)) { // decoder_model_info_present_flag
            // Would add buffer_removal_time fields to the frame header.
            throw msg_exception("AV1 decoder model info is not supported");
        }
    }
    const bool initial_display_delay_present = r.read(1);
    const uint32_t operating_points_cnt = r.read(5) + 1;
    seq->operating_point_count = operating_points_cnt;
    for (uint32_t i = 0; i < operating_points_cnt; ++i) {
        if (i == 0) {
            seq->operating_point_pos = r.pos();
        }
        r.read(12); // operating_point_idc
        const uint32_t level = r.read(5);
        const uint32_t tier = (level > 7) ? r.read(1) : 0;
        if (initial_display_delay_present && r.read(1)) {
            r.read(4); // initial_display_delay_minus_1
        }
        if (i == 0) {
            seq_level_idx0 = level;
            if (tier != 0) {
                throw msg_exception("AV1 high tier is not representable with reduced_still_picture_header");
            }
        }
    }

    seq->width_bits = static_cast<int>(r.read(4)) + 1;
    seq->height_bits = static_cast<int>(r.read(4)) + 1;
    // max_frame_width/height may be the bucket size rather than the actual frame size.
    seq->max_width_pos = r.pos();
    seq->max_width = r.read(seq->width_bits) + 1;
    seq->max_height_pos = r.pos();
    seq->max_height = r.read(seq->height_bits) + 1;
    seq->frame_id_numbers_present = r.read(1);
    if (seq->frame_id_numbers_present) {
        const int delta_frame_id_length = static_cast<int>(r.read(4)) + 2;
        seq->delta_id_len = delta_frame_id_length;
        const int additional_frame_id_length = static_cast<int>(r.read(3)) + 1;
        seq->id_len = delta_frame_id_length + additional_frame_id_length;
    }
    seq->use_128x128 = r.read(1);
    const uint32_t enable_filter_intra = r.read(1);
    const uint32_t enable_intra_edge_filter = r.read(1);
    r.read(2); // enable_interintra_compound, enable_masked_compound
    seq->enable_warped_motion = r.read(1);
    r.read(1); // enable_dual_filter
    const bool enable_order_hint = r.read(1);
    if (enable_order_hint) {
        r.read(1); // enable_jnt_comp
        seq->enable_ref_frame_mvs = r.read(1);
    }
    if (r.read(1)) { // seq_choose_screen_content_tools
        seq->force_screen_content_tools = kSelectScreenContentTools;
    } else {
        seq->force_screen_content_tools = r.read(1);
    }
    if (seq->force_screen_content_tools > 0) {
        if (r.read(1)) { // seq_choose_integer_mv
            seq->force_integer_mv = kSelectIntegerMv;
        } else {
            seq->force_integer_mv = r.read(1);
        }
    } else {
        seq->force_integer_mv = kSelectIntegerMv;
    }
    seq->order_hint_bits = enable_order_hint ? static_cast<int>(r.read(3)) + 1 : 0;
    seq->enable_superres = r.read(1);
    seq->enable_cdef = r.read(1);
    seq->enable_restoration = r.read(1);

    // Reduced header: frame size bits are re-derived from the actual size.
    const int new_width_bits = std::max(bit_length(width - 1), 1);
    const int new_height_bits = std::max(bit_length(height - 1), 1);
    w.put(seq_profile, 3);
    w.put(1, 1); // still_picture
    w.put(1, 1); // reduced_still_picture_header
    w.put(seq_level_idx0, 5);
    w.put(static_cast<uint32_t>(new_width_bits - 1), 4);
    w.put(static_cast<uint32_t>(new_height_bits - 1), 4);
    w.put(width - 1, new_width_bits);
    w.put(height - 1, new_height_bits);
    w.put(seq->use_128x128, 1);
    w.put(enable_filter_intra, 1);
    w.put(enable_intra_edge_filter, 1);
    w.put(seq->enable_superres, 1);
    w.put(seq->enable_cdef, 1);
    w.put(seq->enable_restoration, 1);

    // color_config(), section 5.5.2. Identical in both forms.
    tee t { r, w };
    const bool high_bitdepth = t.copy(1);
    const bool twelve_bit = seq_profile == 2 && high_bitdepth && t.copy(1);
    seq->mono_chrome = (seq_profile == 1) ? false : t.copy(1);
    uint32_t color_primaries = 2, transfer_characteristics = 2, matrix_coefficients = 2; // unspecified
    if (t.copy(1)) {                                                                     // color_description_present_flag
        color_primaries = t.copy(8);
        transfer_characteristics = t.copy(8);
        matrix_coefficients = t.copy(8);
    }
    if (seq->mono_chrome) {
        t.copy(1); // color_range
        seq->subsampling_x = seq->subsampling_y = true;
    } else {
        if (color_primaries == 1 && transfer_characteristics == 13 && matrix_coefficients == 0) {
            // BT.709 / sRGB / identity: full range 4:4:4, color_range not coded.
            seq->subsampling_x = seq->subsampling_y = false;
        } else {
            t.copy(1); // color_range
            if (seq_profile == 0) {
                seq->subsampling_x = seq->subsampling_y = true;
            } else if (seq_profile == 1) {
                seq->subsampling_x = seq->subsampling_y = false;
            } else if (twelve_bit) {
                seq->subsampling_x = t.copy(1);
                seq->subsampling_y = seq->subsampling_x ? t.copy(1) : false;
            } else {
                seq->subsampling_x = true;
                seq->subsampling_y = false;
            }
            if (seq->subsampling_x && seq->subsampling_y) {
                t.copy(2); // chroma_sample_position
            }
        }
        seq->separate_uv_delta_q = t.copy(1);
    }
    seq->film_grain_params_present = t.copy(1);
    w.trailing_bits();
}

// Section 5.9.15
void copy_tile_info(tee & t, const sequence_info & seq, uint32_t mi_cols, uint32_t mi_rows)
{
    const uint32_t sb_cols = seq.use_128x128 ? ((mi_cols + 31) >> 5) : ((mi_cols + 15) >> 4);
    const uint32_t sb_rows = seq.use_128x128 ? ((mi_rows + 31) >> 5) : ((mi_rows + 15) >> 4);
    const int sb_size = (seq.use_128x128 ? 5 : 4) + 2;
    const uint32_t max_tile_width_sb = kMaxTileWidth >> sb_size;
    uint32_t max_tile_area_sb = kMaxTileArea >> (2 * sb_size);
    const int min_log2_tile_cols = tile_log2(max_tile_width_sb, sb_cols);
    const int max_log2_tile_cols = tile_log2(1, std::min(sb_cols, kMaxTileCols));
    const int max_log2_tile_rows = tile_log2(1, std::min(sb_rows, kMaxTileRows));
    const int min_log2_tiles = std::max(min_log2_tile_cols, tile_log2(max_tile_area_sb, sb_rows * sb_cols));

    int tile_cols_log2 = 0;
    int tile_rows_log2 = 0;
    if (t.copy(1)) { // uniform_tile_spacing_flag
        tile_cols_log2 = min_log2_tile_cols;
        while (tile_cols_log2 < max_log2_tile_cols && t.copy(1)) {
            ++tile_cols_log2;
        }
        tile_rows_log2 = std::max(min_log2_tiles - tile_cols_log2, 0);
        while (tile_rows_log2 < max_log2_tile_rows && t.copy(1)) {
            ++tile_rows_log2;
        }
    } else {
        uint32_t widest_tile_sb = 0;
        uint32_t tile_cols = 0;
        for (uint32_t start_sb = 0; start_sb < sb_cols; ++tile_cols) {
            const uint32_t size_sb = t.copy_ns(std::min(sb_cols - start_sb, max_tile_width_sb)) + 1;
            widest_tile_sb = std::max(size_sb, widest_tile_sb);
            start_sb += size_sb;
        }
        tile_cols_log2 = tile_log2(1, tile_cols);

        max_tile_area_sb = (min_log2_tiles > 0) ? ((sb_rows * sb_cols) >> (min_log2_tiles + 1)) : (sb_rows * sb_cols);
        const uint32_t max_tile_height_sb = std::max(max_tile_area_sb / widest_tile_sb, 1u);
        uint32_t tile_rows = 0;
        for (uint32_t start_sb = 0; start_sb < sb_rows; ++tile_rows) {
            start_sb += t.copy_ns(std::min(sb_rows - start_sb, max_tile_height_sb)) + 1;
        }
        tile_rows_log2 = tile_log2(1, tile_rows);
    }
    if (tile_cols_log2 > 0 || tile_rows_log2 > 0) {
        t.copy(tile_rows_log2 + tile_cols_log2); // context_update_tile_id
        t.copy(2);                               // tile_size_bytes_minus_1
    }
}

// film_grain_params(), section 5.9.30, for a shown key frame (update_grain is implied to be 1).
void copy_film_grain_params(tee & t, const sequence_info & seq, std::optional<uint16_t> grain_seed)
{
    if (!t.copy(1)) { // apply_grain
        return;
    }
    if (grain_seed.has_value()) {
        t.r.read(16);
        t.w.put(*grain_seed, 16);
    } else {
        t.copy(16); // grain_seed
    }
    const uint32_t num_y_points = t.copy(4);
    t.copy_bulk(static_cast<int>(num_y_points) * 16); // point_y_value, point_y_scaling
    const bool chroma_scaling_from_luma = seq.mono_chrome ? false : t.copy(1);
    uint32_t num_cb_points = 0;
    uint32_t num_cr_points = 0;
    if (!(seq.mono_chrome || chroma_scaling_from_luma || (seq.subsampling_x && seq.subsampling_y && num_y_points == 0))) {
        num_cb_points = t.copy(4);
        t.copy_bulk(static_cast<int>(num_cb_points) * 16);
        num_cr_points = t.copy(4);
        t.copy_bulk(static_cast<int>(num_cr_points) * 16);
    }
    t.copy(2); // grain_scaling_minus_8
    const uint32_t ar_coeff_lag = t.copy(2);
    const int num_pos_luma = static_cast<int>(2 * ar_coeff_lag * (ar_coeff_lag + 1));
    int num_pos_chroma = num_pos_luma;
    if (num_y_points) {
        num_pos_chroma = num_pos_luma + 1;
        t.copy_bulk(num_pos_luma * 8); // ar_coeffs_y_plus_128
    }
    if (chroma_scaling_from_luma || num_cb_points) {
        t.copy_bulk(num_pos_chroma * 8); // ar_coeffs_cb_plus_128
    }
    if (chroma_scaling_from_luma || num_cr_points) {
        t.copy_bulk(num_pos_chroma * 8); // ar_coeffs_cr_plus_128
    }
    t.copy(2); // ar_coeff_shift_minus_6
    t.copy(2); // grain_scale_shift
    if (num_cb_points) {
        t.copy(8 + 8 + 9); // cb_mult, cb_luma_mult, cb_offset
    }
    if (num_cr_points) {
        t.copy(8 + 8 + 9); // cr_mult, cr_luma_mult, cr_offset
    }
    t.copy(1); // overlap_flag
    t.copy(1); // clip_to_restricted_range
}

struct reference_header
{
    bool valid = false;
    uint32_t width = 0, height = 0;
    int order_hint = 0;
    std::array<bool, kMaxSegments> alt_q_enabled {};
    std::array<int32_t, kMaxSegments> alt_q {};
};

struct layered_frame_context
{
    uint32_t type = 0;
    reference_header current;
    std::array<int, 7> refs {};
    const std::array<reference_header, 8> * history = nullptr;
    bool intra = true;
    bool error_resilient = true;
    bool high_precision_mv = false;
    uint32_t primary = 7;
    uint32_t refresh = 255;
    std::optional<size_t> grain_seed_pos;
};

int relative_hint(int a, int b, int bits)
{
    if (!bits) {
        return 0;
    }
    const int sign = 1 << (bits - 1);
    const int diff = (a - b) & ((1 << bits) - 1);
    return (diff & (sign - 1)) - (diff & sign);
}

// Subexpression code length is independent of the reference/recentering value.
void copy_subexp(tee & t, uint32_t symbols)
{
    uint32_t consumed = 0;
    for (int i = 0; i < 32; ++i) {
        const int bits = i ? 2 + i : 3;
        const uint32_t block = 1u << bits;
        if (symbols <= consumed + 3 * block) {
            t.copy_ns(symbols - consumed);
            return;
        }
        if (!t.copy(1)) {
            t.copy(bits);
            return;
        }
        consumed += block;
    }
    throw msg_exception("Invalid AV1 global motion subexpression");
}

void copy_frame_tail(tee & t,
                     const sequence_info & seq,
                     uint32_t frame_width,
                     uint32_t upscaled_width,
                     uint32_t mi_cols,
                     uint32_t mi_rows,
                     uint32_t allow_intrabc,
                     std::optional<uint16_t> grain_seed,
                     layered_frame_context * frame = nullptr)
{
    copy_tile_info(t, seq, mi_cols, mi_rows);
    const uint32_t base_q_idx = t.copy(8);
    bool any_delta_q = false;
    const auto copy_delta_q = [&t, &any_delta_q]() {
        if (t.copy(1)) { // delta_coded
            if (t.copy(7) != 0) {
                any_delta_q = true;
            }
        }
    };
    copy_delta_q(); // DeltaQYDc
    if (seq.num_planes() > 1) {
        const bool diff_uv_delta = seq.separate_uv_delta_q ? t.copy(1) : false;
        copy_delta_q();
        copy_delta_q();
        if (diff_uv_delta) {
            copy_delta_q();
            copy_delta_q();
        }
    }
    if (t.copy(1)) { // using_qmatrix
        t.copy(4);   // qm_y
        t.copy(4);   // qm_u
        if (seq.separate_uv_delta_q) {
            t.copy(4); // qm_v
        }
    }

    // segmentation_params(), section 5.9.14.
    std::array<bool, kMaxSegments> seg_alt_q_enabled {};
    std::array<int32_t, kMaxSegments> seg_alt_q_data {};
    if (frame && frame->primary != 7) {
        const auto & previous = frame->history->at(frame->refs[frame->primary]);
        seg_alt_q_enabled = previous.alt_q_enabled;
        seg_alt_q_data = previous.alt_q;
    }
    const bool segmentation_enabled = t.copy(1);
    bool update_data = segmentation_enabled;
    if (segmentation_enabled && frame && frame->primary != 7) {
        if (t.copy(1)) {
            t.copy(1); // update_map, temporal_update
        }
        update_data = t.copy(1);
    }
    if (!segmentation_enabled || update_data) {
        seg_alt_q_enabled.fill(false);
        seg_alt_q_data.fill(0);
    }
    if (update_data) {
        static constexpr std::array<int, kSegLvlMax> feature_bits = { 8, 6, 6, 6, 6, 3, 0, 0 };
        static constexpr std::array<int, kSegLvlMax> feature_signed = { 1, 1, 1, 1, 1, 0, 0, 0 };
        for (int i = 0; i < kMaxSegments; ++i) {
            for (int j = 0; j < kSegLvlMax; ++j) {
                if (!t.copy(1)) { // feature_enabled
                    continue;
                }
                int32_t value;
                if (feature_signed[j]) {
                    value = sign_extend(t.copy(1 + feature_bits[j]), 1 + feature_bits[j]);
                } else {
                    value = static_cast<int32_t>(t.copy(feature_bits[j]));
                }
                if (j == kSegLvlAltQ) {
                    seg_alt_q_enabled[i] = true;
                    seg_alt_q_data[i] = std::clamp(value, -255, 255);
                }
            }
        }
    }
    if (frame) {
        frame->current.alt_q_enabled = seg_alt_q_enabled;
        frame->current.alt_q = seg_alt_q_data;
    }

    // delta_q_params() and delta_lf_params()
    bool delta_q_present = false;
    if (base_q_idx > 0) {
        delta_q_present = t.copy(1);
    }
    if (delta_q_present) {
        t.copy(2);                         // delta_q_res
        if (!allow_intrabc && t.copy(1)) { // delta_lf_present
            t.copy(2);                     // delta_lf_res
            t.copy(1);                     // delta_lf_multi
        }
    }

    bool coded_lossless = !any_delta_q;
    if (coded_lossless) {
        for (int i = 0; i < kMaxSegments; ++i) {
            // get_qindex( 1, segmentId )
            const int32_t qindex = (segmentation_enabled && seg_alt_q_enabled[i])
                                       ? std::clamp(static_cast<int32_t>(base_q_idx) + seg_alt_q_data[i], 0, 255)
                                       : static_cast<int32_t>(base_q_idx);
            if (qindex != 0) {
                coded_lossless = false;
                break;
            }
        }
    }
    const bool all_lossless = coded_lossless && frame_width == upscaled_width;

    // loop_filter_params(), section 5.9.11
    if (!coded_lossless && !allow_intrabc) {
        const uint32_t level0 = t.copy(6);
        const uint32_t level1 = t.copy(6);
        if (seq.num_planes() > 1 && (level0 || level1)) {
            t.copy(12); // loop_filter_level[ 2 ], loop_filter_level[ 3 ]
        }
        t.copy(3);           // loop_filter_sharpness
        if (t.copy(1)) {     // loop_filter_delta_enabled
            if (t.copy(1)) { // loop_filter_delta_update
                for (int i = 0; i < kTotalRefsPerFrame; ++i) {
                    if (t.copy(1)) {
                        t.copy(7);
                    }
                }
                for (int i = 0; i < 2; ++i) {
                    if (t.copy(1)) {
                        t.copy(7);
                    }
                }
            }
        }
    }

    // cdef_params(), section 5.9.19
    if (!coded_lossless && !allow_intrabc && seq.enable_cdef) {
        t.copy(2); // cdef_damping_minus_3
        const uint32_t cdef_bits = t.copy(2);
        for (uint32_t i = 0; i < (1u << cdef_bits); ++i) {
            t.copy(6); // cdef_y_pri_strength, cdef_y_sec_strength
            if (seq.num_planes() > 1) {
                t.copy(6); // cdef_uv_pri_strength, cdef_uv_sec_strength
            }
        }
    }

    // lr_params(), section 5.9.20
    if (!all_lossless && !allow_intrabc && seq.enable_restoration) {
        bool uses_lr = false;
        bool uses_chroma_lr = false;
        for (int i = 0; i < seq.num_planes(); ++i) {
            if (t.copy(2) != 0) { // lr_type, 0 is RESTORE_NONE
                uses_lr = true;
                uses_chroma_lr |= i > 0;
            }
        }
        if (uses_lr) {
            if (seq.use_128x128) {
                t.copy(1); // lr_unit_shift
            } else if (t.copy(1)) {
                t.copy(1); // lr_unit_extra_shift
            }
            if (seq.subsampling_x && seq.subsampling_y && uses_chroma_lr) {
                t.copy(1); // lr_uv_shift
            }
        }
    }

    // read_tx_mode()
    if (!coded_lossless) {
        t.copy(1); // tx_mode_select
    }
    if (frame && !frame->intra) {
        const bool select = t.copy(1); // reference_select
        if (select && seq.order_hint_bits) {
            int closest_forward = -1000000;
            bool backward = false;
            for (int index : frame->refs) {
                const int distance = relative_hint(frame->history->at(index).order_hint, frame->current.order_hint, seq.order_hint_bits);
                if (distance < 0) {
                    closest_forward = std::max(closest_forward, distance);
                }
                backward |= distance > 0;
            }
            bool second_forward = false;
            for (int index : frame->refs) {
                const int distance = relative_hint(frame->history->at(index).order_hint, frame->current.order_hint, seq.order_hint_bits);
                second_forward |= distance < closest_forward;
            }
            if (closest_forward != -1000000 && (backward || second_forward)) {
                t.copy(1); // skip_mode_present
            }
        }
        if (!frame->error_resilient && seq.enable_warped_motion) {
            t.copy(1);
        }
    }
    t.copy(1); // reduced_tx_set

    if (frame && !frame->intra) {
        for (int ref = 0; ref < 7; ++ref) {
            if (!t.copy(1)) {
                continue; // IDENTITY
            }
            int type = 2; // ROTZOOM
            if (!t.copy(1)) {
                type = t.copy(1) ? 1 : 3; // TRANSLATION or AFFINE
            }
            if (type >= 2) {
                copy_subexp(t, (1u << 13) + 1);
                copy_subexp(t, (1u << 13) + 1);
                if (type == 3) {
                    copy_subexp(t, (1u << 13) + 1);
                    copy_subexp(t, (1u << 13) + 1);
                }
            }
            const int bits = type == 1 ? 9 - !frame->high_precision_mv : 12;
            copy_subexp(t, (1u << (bits + 1)) + 1);
            copy_subexp(t, (1u << (bits + 1)) + 1);
        }
    }

    if (seq.film_grain_params_present) {
        if (frame) {
            // grain_seed precedes update_grain and remains present when the
            // rest of the parameters are inherited from a reference frame.
            if (t.copy(1)) {
                frame->grain_seed_pos = t.r.pos();
            }
        } else {
            copy_film_grain_params(t, seq, grain_seed);
        }
    }
}

// Section 5.9.2, for a shown key frame. Reads the frame header from `r`, validates that it
// describes width x height, and writes it with reduced_still_picture_header semantics to `w`.
// Stops after the last header bit (before byte_alignment).
void rewrite_frame_header(bit_reader & r, bit_writer & w, const sequence_info & seq, uint32_t width, uint32_t height, std::optional<uint16_t> grain_seed)
{
    if (r.read(1)) { // show_existing_frame
        throw msg_exception("AV1 show_existing_frame is not supported");
    }
    if (r.read(2) != 0) { // frame_type
        throw msg_exception("AV1 frame is not a key frame");
    }
    if (!r.read(1)) { // show_frame
        throw msg_exception("AV1 key frame is not shown");
    }
    // showable_frame and error_resilient_mode are implied for a shown key frame.

    const uint32_t disable_cdf_update = r.read(1);
    uint32_t allow_screen_content_tools = seq.force_screen_content_tools;
    if (seq.force_screen_content_tools == kSelectScreenContentTools) {
        allow_screen_content_tools = r.read(1);
    }
    uint32_t force_integer_mv = 1; // always 1 for intra frames
    if (allow_screen_content_tools && seq.force_integer_mv == kSelectIntegerMv) {
        r.read(1);
    }
    if (seq.frame_id_numbers_present) {
        r.read(seq.id_len); // current_frame_id
    }
    const bool frame_size_override_flag = r.read(1);
    r.read(seq.order_hint_bits); // order_hint
    // primary_ref_frame, refresh_frame_flags and ref_order_hint are not coded for shown intra frames.

    // frame_size()
    uint32_t upscaled_width = 0;
    uint32_t frame_height = 0;
    if (frame_size_override_flag) {
        upscaled_width = r.read(seq.width_bits) + 1;
        frame_height = r.read(seq.height_bits) + 1;
    } else {
        upscaled_width = seq.max_width;
        frame_height = seq.max_height;
    }
    if (upscaled_width != width || frame_height != height) {
        throw msg_exception(
            std::format("AV1 frame size {}x{} does not match image size {}x{}", upscaled_width, frame_height, width, height));
    }
    // superres_params()
    uint32_t use_superres = 0;
    uint32_t coded_denom = 0;
    if (seq.enable_superres) {
        use_superres = r.read(1);
        if (use_superres) {
            coded_denom = r.read(3);
        }
    }
    const uint32_t superres_denom = use_superres ? coded_denom + 9 : 8;
    const uint32_t frame_width = (upscaled_width * 8 + (superres_denom / 2)) / superres_denom;
    const uint32_t mi_cols = 2 * ((frame_width + 7) >> 3);
    const uint32_t mi_rows = 2 * ((frame_height + 7) >> 3);
    // render_size()
    const uint32_t render_size_different = r.read(1);
    uint32_t render_size_bits = 0;
    if (render_size_different) {
        render_size_bits = r.read(32);
    }
    uint32_t allow_intrabc = 0;
    if (allow_screen_content_tools && upscaled_width == frame_width) {
        allow_intrabc = r.read(1);
    }
    if (!disable_cdf_update) {
        r.read(1); // disable_frame_end_update_cdf; implied to be 1 by reduced_still_picture_header
    }

    // Emit the reduced form. With reduced_still_picture_header, show_existing_frame, frame_type,
    // show_frame, frame_size_override_flag, order_hint and disable_frame_end_update_cdf are not coded,
    // the frame size is the max frame size from the sequence header, and
    // seq_force_screen_content_tools/seq_force_integer_mv are always SELECT.
    w.put(disable_cdf_update, 1);
    w.put(allow_screen_content_tools, 1);
    if (allow_screen_content_tools) {
        w.put(force_integer_mv, 1);
    }
    if (seq.enable_superres) {
        w.put(use_superres, 1);
        if (use_superres) {
            w.put(coded_denom, 3);
        }
    }
    w.put(render_size_different, 1);
    if (render_size_different) {
        w.put(render_size_bits, 32);
    }
    if (allow_screen_content_tools && upscaled_width == frame_width) {
        w.put(allow_intrabc, 1);
    }

    tee t { r, w };
    copy_frame_tail(t, seq, frame_width, upscaled_width, mi_cols, mi_rows, allow_intrabc, grain_seed);
}

void set_short_refs(layered_frame_context & frame, int last, int golden, const sequence_info & seq)
{
    std::array<int, 8> hints {};
    std::array<bool, 8> used {};
    const int current = 1 << (seq.order_hint_bits - 1);
    for (int i = 0; i < 8; ++i) {
        hints[i] = current + relative_hint(frame.history->at(i).order_hint, frame.current.order_hint, seq.order_hint_bits);
    }
    frame.refs.fill(-1);
    frame.refs[0] = last;
    frame.refs[3] = golden;
    used[last] = used[golden] = true;
    const auto choose = [&](bool backward, bool latest) {
        int best = -1;
        for (int i = 0; i < 8; ++i) {
            if (!used[i] && ((hints[i] >= current) == backward) &&
                (best < 0 || (latest ? hints[i] >= hints[best] : hints[i] < hints[best]))) {
                best = i;
            }
        }
        if (best >= 0) {
            used[best] = true;
        }
        return best;
    };
    frame.refs[6] = choose(true, true);
    frame.refs[4] = choose(true, false);
    frame.refs[5] = choose(true, false);
    for (int i : { 1, 2, 4, 5, 6 }) {
        if (frame.refs[i] < 0) {
            frame.refs[i] = choose(false, true);
        }
    }
    const int earliest = int(std::min_element(hints.begin(), hints.end()) - hints.begin());
    for (auto & index : frame.refs) {
        if (index < 0) {
            index = earliest;
        }
    }
}

// Parse normal, shown KEY/INTER/INTRA_ONLY frame headers. The reference state
// belongs to this picture, never to the pooled hardware session's previous job.
layered_frame_context read_layered_frame(bit_reader & r,
                                         const sequence_info & seq,
                                         std::array<reference_header, 8> & history,
                                         uint32_t expected_width,
                                         uint32_t expected_height)
{
    layered_frame_context frame;
    frame.history = &history;
    if (r.read(1)) {
        throw msg_exception("Unexpected show_existing_frame in layered NVENC output");
    }
    const auto type = r.read(2);
    frame.type = type;
    if (type == 3 || !r.read(1)) {
        throw msg_exception("Unexpected hidden/switch frame in layered NVENC output");
    }
    frame.intra = type != 1;
    frame.error_resilient = type == 0 ? true : bool(r.read(1));
    if (type == 0) {
        history = {};
    }
    const bool disable_cdf_update = r.read(1);
    const bool screen = seq.force_screen_content_tools == 2 ? bool(r.read(1)) : bool(seq.force_screen_content_tools);
    bool integer_mv = false;
    if (screen) {
        integer_mv = seq.force_integer_mv == 2 ? bool(r.read(1)) : bool(seq.force_integer_mv);
    }
    if (frame.intra) {
        integer_mv = true;
    }
    if (seq.frame_id_numbers_present) {
        r.read(seq.id_len);
    }
    const bool override_size = r.read(1);
    frame.current.order_hint = r.read(seq.order_hint_bits);
    frame.primary = frame.intra || frame.error_resilient ? 7 : r.read(3);
    frame.refresh = type == 0 ? 255 : r.read(8);
    if ((!frame.intra || frame.refresh != 255) && frame.error_resilient && seq.order_hint_bits) {
        for (auto & ref : history) {
            if (r.read(seq.order_hint_bits) != uint32_t(ref.order_hint)) {
                ref.valid = false;
            }
        }
    }
    if (!frame.intra) {
        const bool short_refs = seq.order_hint_bits ? bool(r.read(1)) : false;
        if (short_refs) {
            const int last = r.read(3), golden = r.read(3);
            set_short_refs(frame, last, golden, seq);
        }
        for (auto & index : frame.refs) {
            if (!short_refs) {
                index = r.read(3);
            }
            if (seq.frame_id_numbers_present) {
                r.read(seq.delta_id_len);
            }
            if (!history[index].valid) {
                throw msg_exception("Invalid layered AV1 reference frame");
            }
        }
    }
    bool size_from_ref = false;
    if (!frame.intra && override_size && !frame.error_resilient) {
        for (int index : frame.refs) {
            if (r.read(1)) {
                frame.current.width = history[index].width;
                frame.current.height = history[index].height;
                size_from_ref = true;
                break;
            }
        }
    }
    if (!size_from_ref) {
        frame.current.width = override_size ? r.read(seq.width_bits) + 1 : seq.max_width;
        frame.current.height = override_size ? r.read(seq.height_bits) + 1 : seq.max_height;
    }
    const auto upscaled_width = frame.current.width;
    uint32_t denominator = 8;
    if (seq.enable_superres && r.read(1)) {
        denominator = r.read(3) + 9;
    }
    const auto coded_width = (upscaled_width * 8 + denominator / 2) / denominator;
    if (!size_from_ref && r.read(1)) {
        r.read(32); // render_size
    }
    if (upscaled_width != expected_width || frame.current.height != expected_height) {
        throw msg_exception("Layered AV1 coded size does not match the uploaded layer");
    }
    bool intrabc = false;
    if (frame.intra) {
        if (screen && coded_width == upscaled_width) {
            intrabc = r.read(1);
        }
    } else {
        if (!integer_mv) {
            frame.high_precision_mv = r.read(1);
        }
        if (!r.read(1)) {
            r.read(2); // interpolation filter
        }
        r.read(1); // is_motion_mode_switchable
        if (!frame.error_resilient && seq.enable_ref_frame_mvs) {
            r.read(1);
        }
    }
    if (!disable_cdf_update) {
        r.read(1);
    }
    if (seq.film_grain_params_present) {
        bit_writer ignored;
        tee t { r, ignored };
        copy_frame_tail(t,
                        seq,
                        coded_width,
                        upscaled_width,
                        2 * ((coded_width + 7) >> 3),
                        2 * ((frame.current.height + 7) >> 3),
                        intrabc,
                        std::nullopt,
                        &frame);
    }
    frame.current.valid = true;
    for (int i = 0; i < 8; ++i) {
        if (frame.refresh & (1u << i)) {
            history[i] = frame.current;
        }
    }
    return frame;
}

void patch_bits(std::vector<uint8_t> & bytes, size_t pos, int count, uint32_t value)
{
    if (pos + count > bytes.size() * 8) {
        throw msg_exception("AV1 bit patch exceeds OBU payload");
    }
    for (int i = 0; i < count; ++i) {
        const auto mask = uint8_t(1u << (7 - ((pos + i) & 7)));
        auto & byte = bytes[(pos + i) >> 3];
        byte = (byte & ~mask) | (((value >> (count - i - 1)) & 1) ? mask : 0);
    }
}

uint32_t read_leb128(const std::vector<uint8_t> & in, size_t * pos)
{
    uint64_t value = 0;
    bool terminated = false;
    for (int i = 0; i < 8; ++i) {
        if (*pos >= in.size()) {
            throw msg_exception("AV1 bitstream truncated");
        }
        const uint8_t byte = in[(*pos)++];
        value |= static_cast<uint64_t>(byte & 0x7f) << (i * 7);
        if (!(byte & 0x80)) {
            terminated = true;
            break;
        }
    }
    if (!terminated || value > UINT32_MAX) {
        throw msg_exception("AV1 OBU too large");
    }
    return static_cast<uint32_t>(value);
}

void append_leb128(std::vector<uint8_t> & out, uint32_t value)
{
    do {
        uint8_t byte = value & 0x7f;
        value >>= 7;
        if (value) {
            byte |= 0x80;
        }
        out.push_back(byte);
    } while (value);
}

void append_obu(std::vector<uint8_t> & out,
                uint32_t type,
                const uint8_t * extension,
                const std::vector<uint8_t> & payload_head,
                const uint8_t * payload_tail,
                size_t payload_tail_size)
{
    out.push_back(static_cast<uint8_t>((type << 3) | (extension ? 4 : 0) | 2)); // obu_has_size_field
    if (extension) {
        out.push_back(*extension);
    }
    append_leb128(out, static_cast<uint32_t>(payload_head.size() + payload_tail_size));
    out.insert(out.end(), payload_head.begin(), payload_head.end());
    if (payload_tail_size) {
        out.insert(out.end(), payload_tail, payload_tail + payload_tail_size);
    }
}

void rewrite_still_impl(const std::vector<uint8_t> & in,
                        uint32_t width,
                        uint32_t height,
                        std::optional<uint16_t> grain_seed,
                        std::vector<uint8_t> * out)
{
    std::vector<uint8_t> result;
    sequence_info seq;
    bool have_sequence_header = false;
    bool have_frame = false;

    size_t pos = 0;
    while (pos < in.size()) {
        const size_t obu_start = pos;
        const uint8_t header = in[pos++];
        const uint32_t type = (header >> 3) & 0xf;
        const bool has_extension = (header >> 2) & 1;
        if ((header & 0x80) || !(header & 2)) {
            throw msg_exception("AV1 OBU without size field is not supported");
        }
        uint8_t extension = 0;
        if (has_extension) {
            if (pos >= in.size()) {
                throw msg_exception("AV1 bitstream truncated");
            }
            extension = in[pos++];
        }
        const uint32_t obu_size = read_leb128(in, &pos);
        if (obu_size > in.size() - pos) {
            throw msg_exception("AV1 bitstream truncated");
        }
        const uint8_t * payload = in.data() + pos;
        const size_t obu_end = pos + obu_size;
        pos = obu_end;

        if (type == kObuSequenceHeader) {
            bit_reader r(payload, obu_size);
            bit_writer w;
            rewrite_sequence_header(r, w, width, height, &seq);
            if (seq.reduced_still_picture_header) {
                *out = in; // nothing to do
                return;
            }
            have_sequence_header = true;
            append_obu(result, type, nullptr, w.bytes(), nullptr, 0);
        } else if (type == kObuFrame || type == kObuFrameHeader) {
            if (!have_sequence_header) {
                throw msg_exception("AV1 frame OBU without preceding sequence header");
            }
            if (have_frame) {
                throw msg_exception("AV1 bitstream contains more than one frame");
            }
            have_frame = true;
            bit_reader r(payload, obu_size);
            bit_writer w;
            rewrite_frame_header(r, w, seq, width, height, grain_seed);
            if (type == kObuFrame) {
                w.byte_align();
                const size_t tile_group_offset = (r.pos() + 7) / 8; // byte_alignment()
                append_obu(result, type, has_extension ? &extension : nullptr, w.bytes(), payload + tile_group_offset, obu_size - tile_group_offset);
            } else {
                w.trailing_bits();
                append_obu(result, type, has_extension ? &extension : nullptr, w.bytes(), nullptr, 0);
            }
        } else if (type == kObuRedundantFrameHeader) {
            // frame_header_copy() is meaningless for a reduced still picture.
        } else {
            result.insert(result.end(),
                          in.begin() + static_cast<std::ptrdiff_t>(obu_start),
                          in.begin() + static_cast<std::ptrdiff_t>(obu_end));
        }
    }
    if (!have_sequence_header || !have_frame) {
        throw msg_exception("AV1 bitstream lacks a sequence header or a frame");
    }
    *out = std::move(result);
}

void rewrite_layered_impl(const std::vector<std::vector<uint8_t>> & packets,
                          const std::vector<std::pair<uint32_t, uint32_t>> & sizes,
                          uint32_t final_width,
                          uint32_t final_height,
                          const std::vector<std::optional<uint16_t>> & grain_seeds,
                          std::vector<std::vector<uint8_t>> * output)
{
    std::vector<std::vector<uint8_t>> result(packets.size());
    std::array<reference_header, 8> history {};
    sequence_info seq;
    std::vector<uint8_t> first_sequence;
    for (size_t layer = 0; layer < packets.size(); ++layer) {
        const auto & in = packets[layer];
        size_t pos = 0;
        bool have_frame = false;
        while (pos < in.size()) {
            const auto header = in[pos++];
            const uint32_t type = (header >> 3) & 15;
            if ((header & 0x81) || !(header & 2)) {
                throw msg_exception("Invalid sized AV1 OBU header");
            }
            if (header & 4) {
                if (pos == in.size()) {
                    throw msg_exception("Truncated AV1 extension header");
                }
                ++pos;
            }
            const auto size = read_leb128(in, &pos);
            if (size > in.size() - pos) {
                throw msg_exception("Truncated layered AV1 OBU");
            }
            std::vector<uint8_t> payload(in.begin() + pos, in.begin() + pos + size);
            pos += size;
            if (type == 2 || type == 15 || type == kObuRedundantFrameHeader) {
                continue;
            }
            if (type == kObuSequenceHeader) {
                bit_reader reader(payload.data(), payload.size());
                bit_writer ignored;
                sequence_info next;
                rewrite_sequence_header(reader, ignored, final_width, final_height, &next);
                if (next.still_picture || next.reduced_still_picture_header || next.operating_point_count != 1 ||
                    next.max_width < final_width || next.max_height < final_height) {
                    throw msg_exception("Unsupported layered AV1 sequence header");
                }
                patch_bits(payload, next.operating_point_pos, 12, (((1u << packets.size()) - 1) << 8) | 1);
                patch_bits(payload, next.max_width_pos, next.width_bits, final_width - 1);
                patch_bits(payload, next.max_height_pos, next.height_bits, final_height - 1);
                if (first_sequence.empty()) {
                    if (layer || have_frame) {
                        throw msg_exception("Layered AV1 sequence header must precede the first frame");
                    }
                    first_sequence = payload;
                    seq = next;
                    append_obu(result[layer], type, nullptr, payload, nullptr, 0);
                } else if (payload != first_sequence) {
                    throw msg_exception("NVENC changed sequence syntax within a layered image");
                } else {
                    seq = next; // Parse frame dimensions using the original, unmodified header.
                }
            } else if (type == kObuFrame || type == kObuFrameHeader) {
                if (first_sequence.empty() || have_frame) {
                    throw msg_exception("Expected one frame per NVENC layer");
                }
                bit_reader reader(payload.data(), payload.size());
                const auto frame = read_layered_frame(reader, seq, history, sizes[layer].first, sizes[layer].second);
                if (!layer && frame.type != 0) {
                    throw msg_exception("First NVENC layer must be a keyframe");
                }
                if (grain_seeds[layer] && frame.grain_seed_pos) {
                    patch_bits(payload, *frame.grain_seed_pos, 16, *grain_seeds[layer]);
                }
                const uint8_t extension = static_cast<uint8_t>(layer << 3);
                append_obu(result[layer], type, &extension, payload, nullptr, 0);
                have_frame = true;
            } else {
                const uint8_t extension = static_cast<uint8_t>(layer << 3);
                append_obu(result[layer], type, type == 4 ? &extension : nullptr, payload, nullptr, 0);
            }
        }
        if (!have_frame) {
            throw msg_exception("Missing NVENC layer frame");
        }
    }
    *output = std::move(result);
}

} // namespace

avifResult rewrite_layered_picture(const std::vector<std::vector<uint8_t>> & packets,
                                   const std::vector<std::pair<uint32_t, uint32_t>> & sizes,
                                   uint32_t final_width,
                                   uint32_t final_height,
                                   const std::vector<std::optional<uint16_t>> & grain_seeds,
                                   std::vector<std::vector<uint8_t>> * output,
                                   avifDiagnostics * diag)
{
    if (!output || packets.size() < 2 || packets.size() > 4 || packets.size() != sizes.size() || packets.size() != grain_seeds.size() ||
        !final_width || !final_height || sizes.back() != std::pair(final_width, final_height)) {
        set_diagnostic(diag, "Invalid layered AV1 rewrite arguments");
        return AVIF_RESULT_INVALID_ARGUMENT;
    }
    try {
        rewrite_layered_impl(packets, sizes, final_width, final_height, grain_seeds, output);
        return AVIF_RESULT_OK;
    } catch (const std::bad_alloc &) {
        set_diagnostic(diag, "Out of memory rewriting layered AV1");
        return AVIF_RESULT_OUT_OF_MEMORY;
    } catch (const std::exception & e) {
        set_diagnostic(diag, "Failed to rewrite layered AV1: {}", e.what());
        return AVIF_RESULT_UNKNOWN_ERROR;
    }
}

avifResult rewrite_to_reduced_still_picture(const std::vector<uint8_t> & in,
                                            uint32_t width,
                                            uint32_t height,
                                            std::optional<uint16_t> grain_seed,
                                            std::vector<uint8_t> * out,
                                            avifDiagnostics * diag)
{
    if (width == 0 || height == 0) {
        set_diagnostic(diag, "invalid image size for AV1 bitstream rewrite");
        return AVIF_RESULT_INVALID_ARGUMENT;
    }
    try {
        rewrite_still_impl(in, width, height, grain_seed, out);
    } catch (const std::exception & e) {
        set_diagnostic(diag, "failed to rewrite AV1 bitstream to reduced_still_picture_header: {}", e.what());
        return AVIF_RESULT_UNKNOWN_ERROR;
    }
    return AVIF_RESULT_OK;
}

} // namespace avif_nvenc
