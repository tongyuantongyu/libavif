// Copyright 2026 Yuan Tong. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#include "film_grain.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <list>
#include <mutex>
#include <string_view>
#include <vector>

#include "exception.h"

namespace avif_nvenc
{

void film_grain_params::to_nvenc(NV_ENC_FILM_GRAIN_PARAMS_AV1 * out) const
{
    *out = {};
    out->applyGrain = 1;
    out->chromaScalingFromLuma = chroma_scaling_from_luma;
    out->overlapFlag = overlap_flag;
    out->clipToRestrictedRange = clip_to_restricted_range;
    out->grainScalingMinus8 = scaling_shift - 8u;
    out->arCoeffLag = ar_coeff_lag;
    out->numYPoints = num_y_points;
    out->numCbPoints = num_cb_points;
    out->numCrPoints = num_cr_points;
    out->arCoeffShiftMinus6 = ar_coeff_shift - 6u;
    out->grainScaleShift = grain_scale_shift;
    for (size_t i = 0; i < scaling_points_y.size(); ++i) {
        out->pointYValue[i] = scaling_points_y[i][0];
        out->pointYScaling[i] = scaling_points_y[i][1];
    }
    for (size_t i = 0; i < scaling_points_cb.size(); ++i) {
        out->pointCbValue[i] = scaling_points_cb[i][0];
        out->pointCbScaling[i] = scaling_points_cb[i][1];
        out->pointCrValue[i] = scaling_points_cr[i][0];
        out->pointCrScaling[i] = scaling_points_cr[i][1];
    }
    for (size_t i = 0; i < ar_coeffs_y.size(); ++i) {
        out->arCoeffsYPlus128[i] = static_cast<uint8_t>(ar_coeffs_y[i] + 128);
    }
    for (size_t i = 0; i < ar_coeffs_cb.size(); ++i) {
        out->arCoeffsCbPlus128[i] = static_cast<uint8_t>(ar_coeffs_cb[i] + 128);
        out->arCoeffsCrPlus128[i] = static_cast<uint8_t>(ar_coeffs_cr[i] + 128);
    }
    out->cbMult = cb_mult;
    out->cbLumaMult = cb_luma_mult;
    out->cbOffset = cb_offset;
    out->crMult = cr_mult;
    out->crLumaMult = cr_luma_mult;
    out->crOffset = cr_offset;
}

film_grain_params make_dither_grain(int strength, bool limited_range)
{
    // Why film grain, and what it can do:
    //   * The grain is synthesized after decoding, so it costs no bitrate and cannot be smoothed away by the encoder,
    //     which is the usual failure of dithering before encoding.
    //   * It is added after the YUV quantization, so it cannot restore detail that the quantization removed. It masks
    //     the contours and randomizes the irregular steps that the 219 luma levels of limited range and the YUV -> RGB
    //     gains (luma 1.16, Cr -> R 1.6, Cb -> B 2.1 RGB LSBs per code) produce. That is why it targets 8-bit YUV.
    //     Measured on smooth gradients, 10-bit YUV from 8-bit RGB round-trips exactly and gains nothing.
    //
    // Parameters (see section 7.18.3 of the AV1 specification):
    //   * White noise (ar_coeff_lag 0, grain_scale_shift 0), overlap_flag on. The grain template has a standard
    //     deviation of 511.5, so the 8-bit grain has a std of 511.5 / 16 = 32. With scaling_shift 11 (the finest
    //     resolution) the noise std in 8-bit LSBs is scale * 32 / 2048 = scale / 64. The result is the same at higher
    //     bit depths, as the grain and the image scale together.
    //   * Luma: std 0.30 + 0.10 * (n - 1) LSBs for strength n (0.30 .. 2.60). In 8-bit output the noise is rounded to
    //     integers, so only ~10% of the pixels change at n = 1 and ~50% at n = 8. The error of the smoothed output
    //     against the ideal gradient was lowest at about 0.6 (n = 4) and flat above, so higher strengths only mask more.
    //     On a dark blurred image whose source itself has 1 LSB contours, the contours were still visible at n = 4-8
    //     and mostly hidden from n = 12; n = 24 is the most grain that was still considered reasonable.
    //   * Chroma: independent of luma and flat over the image content. Off at n = 1; 0.30 + 0.03 * (n - 2) LSBs
    //     (0.30 .. 0.96) above. Below ~0.25 the rounding to integers removes nearly all of the chroma noise (design
    //     0.15 measured 0.05) while 0.30 measured 0.34, so the range jumps from nothing to 0.30. Chroma noise is
    //     what reduces the error on tinted gradients, but it is also colored noise, so it stays low: one chroma LSB
    //     is 1.6 to 2.1 RGB LSBs.
    //   * The luma scaling function keeps full strength up to 25% of the signal range, then falls to 45% at 50%, 20%
    //     at 75% and 10% at 95% (banding was still hidden at about half of the strength), and to zero at the ends of
    //     the signal range: there the noise is clipped on one side, which would lift the black level and dull the
    //     white level.
    film_grain_params p;
    const double n = static_cast<double>(std::clamp(strength, 1, kMaxDitherGrainStrength) - 1);
    const auto luma_scale = static_cast<uint8_t>(std::lround((0.30 + 0.10 * n) * 64.0));
    const auto chroma_scale = static_cast<uint8_t>(strength >= 2 ? std::lround((0.30 + 0.03 * (n - 1.0)) * 64.0) : 0);

    p.scaling_shift = 11;
    p.ar_coeff_lag = 0;
    p.ar_coeff_shift = 6;
    p.grain_scale_shift = 0;
    p.overlap_flag = true;
    p.clip_to_restricted_range = limited_range;
    p.chroma_scaling_from_luma = false;

    // Luma scaling: banding is most visible in the shadows, and grain is most objectionable in the highlights, so the
    // strength falls with the brightness. The curve is given as (fraction of the signal range, fraction of the
    // strength) and ends in zero at both ends of the range, where noise would be clipped on one side. The range is
    // 16..235 for limited range and 0..255 for full range. Point x values must increase.
    struct curve_point
    {
        double range_position;
        double strength;
    };
    static constexpr curve_point kLumaCurve[] = {
        { 0.0, 0.0 }, { 0.04, 1.0 }, { 0.25, 1.0 }, { 0.50, 0.45 }, { 0.75, 0.20 }, { 0.95, 0.10 }, { 1.0, 0.0 },
    };
    const double lo = limited_range ? 16 : 0;
    const double hi = limited_range ? 235 : 255;
    std::vector<std::array<uint8_t, 2>> y_points;
    if (limited_range) {
        y_points.push_back({ 0, 0 });
    }
    for (const curve_point & point : kLumaCurve) {
        y_points.push_back({ static_cast<uint8_t>(std::lround(lo + point.range_position * (hi - lo))),
                             static_cast<uint8_t>(std::lround(luma_scale * point.strength)) });
    }
    if (limited_range) {
        y_points.push_back({ 255, 0 });
    }
    p.num_y_points = static_cast<uint8_t>(y_points.size());
    std::ranges::copy(y_points, p.scaling_points_y.begin());

    if (chroma_scale > 0) {
        p.num_cb_points = p.num_cr_points = 2;
        p.scaling_points_cb[0] = p.scaling_points_cr[0] = { 0, chroma_scale };
        p.scaling_points_cb[1] = p.scaling_points_cr[1] = { 255, chroma_scale };
    }
    // libaom's defaults: the scaling function index is the (average) luma. Irrelevant for a flat function.
    p.cb_mult = p.cr_mult = 128;
    p.cb_luma_mult = p.cr_luma_mult = 192;
    p.cb_offset = p.cr_offset = 256;
    return p;
}

namespace
{

constexpr size_t kCacheCapacity = 16;

class table_reader
{
public:
    explicit table_reader(const std::string & path) : in_(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary) {}

    [[nodiscard]] bool is_open() const { return in_.is_open(); }

    void read_magic()
    {
        char magic[8] = {};
        in_.read(magic, sizeof(magic));
        if (in_.gcount() != sizeof(magic) || std::memcmp(magic, "filmgrn1", sizeof(magic)) != 0) {
            throw msg_exception("unable to read (or invalid) file magic");
        }
    }

    // Returns false at the end of the file.
    bool next_entry(std::optional<film_grain_params> * params_out, uint16_t * seed_out, bool * covers_time_zero)
    {
        std::string tag;
        if (!(in_ >> tag)) {
            return false;
        }
        if (tag != "E") {
            throw msg_exception(std::format("expected entry tag 'E', got '{}'", tag));
        }
        const int64_t start_time = read_int(INT64_MIN, INT64_MAX, "start time");
        const int64_t end_time = read_int(INT64_MIN, INT64_MAX, "end time");
        const bool apply_grain = read_int(0, 1, "apply_grain") != 0;
        const auto random_seed = static_cast<uint16_t>(read_int(0, UINT16_MAX, "random_seed"));
        const bool update_parameters = read_int(0, 1, "update_parameters") != 0;
        *covers_time_zero = start_time <= 0 && 0 < end_time;

        film_grain_params p;
        if (update_parameters) {
            read_parameters(&p);
        }
        if (*covers_time_zero) {
            if (!apply_grain) {
                params_out->reset();
            } else if (!update_parameters) {
                throw msg_exception("the entry covering time 0 has apply_grain set but no parameters (update_parameters is 0)");
            } else {
                *params_out = p;
                *seed_out = random_seed;
            }
        }
        return true;
    }

private:
    int64_t read_int(int64_t min, int64_t max, const char * what)
    {
        int64_t v = 0;
        if (!(in_ >> v)) {
            throw msg_exception(std::format("unable to read {}", what));
        }
        if (v < min || v > max) {
            throw msg_exception(std::format("{} is out of range: {}", what, v));
        }
        return v;
    }

    void expect(std::string_view expected)
    {
        std::string tag;
        if (!(in_ >> tag) || tag != expected) {
            throw msg_exception(std::format("expected '{}'", expected));
        }
    }

    template <size_t N>
    uint8_t read_points(std::string_view tag, std::array<std::array<uint8_t, 2>, N> * points)
    {
        expect(tag);
        const auto n = static_cast<uint8_t>(read_int(0, N, "number of scaling points"));
        for (uint8_t i = 0; i < n; ++i) {
            (*points)[i][0] = static_cast<uint8_t>(read_int(0, 255, "scaling point x"));
            (*points)[i][1] = static_cast<uint8_t>(read_int(0, 255, "scaling point y"));
        }
        return n;
    }

    template <size_t N>
    void read_coeffs(std::string_view tag, size_t count, std::array<int8_t, N> * coeffs)
    {
        expect(tag);
        for (size_t i = 0; i < count; ++i) {
            (*coeffs)[i] = static_cast<int8_t>(read_int(-128, 127, "auto-regressive coefficient"));
        }
    }

    void read_parameters(film_grain_params * p)
    {
        expect("p");
        p->ar_coeff_lag = static_cast<uint8_t>(read_int(0, 3, "ar_coeff_lag"));
        p->ar_coeff_shift = static_cast<uint8_t>(read_int(6, 9, "ar_coeff_shift"));
        p->grain_scale_shift = static_cast<uint8_t>(read_int(0, 3, "grain_scale_shift"));
        p->scaling_shift = static_cast<uint8_t>(read_int(8, 11, "scaling_shift"));
        p->chroma_scaling_from_luma = read_int(0, 1, "chroma_scaling_from_luma") != 0;
        p->overlap_flag = read_int(0, 1, "overlap_flag") != 0;
        p->cb_mult = static_cast<uint8_t>(read_int(0, 255, "cb_mult"));
        p->cb_luma_mult = static_cast<uint8_t>(read_int(0, 255, "cb_luma_mult"));
        p->cb_offset = static_cast<uint16_t>(read_int(0, 511, "cb_offset"));
        p->cr_mult = static_cast<uint8_t>(read_int(0, 255, "cr_mult"));
        p->cr_luma_mult = static_cast<uint8_t>(read_int(0, 255, "cr_luma_mult"));
        p->cr_offset = static_cast<uint16_t>(read_int(0, 511, "cr_offset"));
        p->num_y_points = read_points("sY", &p->scaling_points_y);
        p->num_cb_points = read_points("sCb", &p->scaling_points_cb);
        p->num_cr_points = read_points("sCr", &p->scaling_points_cr);
        const size_t n = 2u * p->ar_coeff_lag * (p->ar_coeff_lag + 1u);
        read_coeffs("cY", n, &p->ar_coeffs_y);
        read_coeffs("cCb", n + 1, &p->ar_coeffs_cb);
        read_coeffs("cCr", n + 1, &p->ar_coeffs_cr);
    }

    std::ifstream in_;
};

struct parsed_entry
{
    std::optional<film_grain_params> params;
    uint16_t random_seed = 0;
};

// Returns the parameters of the entry covering time 0.
parsed_entry parse_table(const std::string & path)
{
    table_reader reader(path);
    if (!reader.is_open()) {
        throw msg_exception("unable to open file");
    }
    reader.read_magic();

    parsed_entry result;
    bool covers_time_zero = false;
    while (!covers_time_zero && reader.next_entry(&result.params, &result.random_seed, &covers_time_zero)) {
    }
    if (!covers_time_zero) {
        throw msg_exception("no entry covers time 0");
    }
    return result;
}

struct cache_entry
{
    std::string path;
    uintmax_t file_size = 0;
    std::filesystem::file_time_type mtime;
    parsed_entry parsed;
};

std::mutex g_cache_mutex;
std::list<cache_entry> g_cache; // most recently used first

} // namespace

avifResult load_film_grain_table(const std::string & path,
                                std::optional<film_grain_params> * params,
                                uint16_t * random_seed,
                                avifDiagnostics * diag)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path fs_path(std::u8string(path.begin(), path.end()));
    const std::string key = fs::absolute(fs_path, ec).string();
    const uintmax_t file_size = fs::file_size(fs_path, ec);
    if (ec) {
        set_diagnostic(diag, "NVENC film grain table '{}': {}", path, ec.message());
        return AVIF_RESULT_INVALID_ARGUMENT;
    }
    const fs::file_time_type mtime = fs::last_write_time(fs_path, ec);
    if (ec) {
        set_diagnostic(diag, "NVENC film grain table '{}': {}", path, ec.message());
        return AVIF_RESULT_INVALID_ARGUMENT;
    }

    {
        std::lock_guard lock(g_cache_mutex);
        for (auto it = g_cache.begin(); it != g_cache.end(); ++it) {
            if (it->path != key) {
                continue;
            }
            if (it->file_size == file_size && it->mtime == mtime) {
                g_cache.splice(g_cache.begin(), g_cache, it);
                *params = it->parsed.params;
                *random_seed = it->parsed.random_seed;
                return AVIF_RESULT_OK;
            }
            g_cache.erase(it); // stale
            break;
        }
    }

    // Parse without holding the lock. Concurrent misses for the same file may parse it twice.
    parsed_entry parsed;
    try {
        parsed = parse_table(path);
    } catch (const std::exception & e) {
        set_diagnostic(diag, "NVENC film grain table '{}': {}", path, e.what());
        return AVIF_RESULT_INVALID_ARGUMENT;
    }

    {
        std::lock_guard lock(g_cache_mutex);
        std::erase_if(g_cache, [&key](const cache_entry & e) { return e.path == key; });
        g_cache.push_front({ key, file_size, mtime, parsed });
        while (g_cache.size() > kCacheCapacity) {
            g_cache.pop_back();
        }
    }
    *params = parsed.params;
    *random_seed = parsed.random_seed;
    return AVIF_RESULT_OK;
}

} // namespace avif_nvenc
