// Copyright 2026 Yuan Tong. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#include "codec_wrapper.h"

#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <vector>

#include "codec_options.h"
#include "exception.h"
#include "obu_rewrite.h"
#include "runtime.h"
#include "session.h"

namespace avif_nvenc
{

struct codec_state
{
    avifBool submitted_image = AVIF_FALSE;
    const avifEncoder * owner = nullptr;
    bool alpha = false;
    bool failed = false;
    bool finished = false;
    uint32_t layer_count = 0;
    image_size final_size;
    std::shared_ptr<layered_group> group;
    std::map<std::string, std::string> options;
    std::vector<session_request> layers;
};

namespace
{

std::mutex groups_mutex;
std::map<const avifEncoder *, std::weak_ptr<layered_group>> groups;

// libavif clears csOptions after each AddImage. Store explicit options, never
// resolved automatic defaults, so each layer gets its own QP-dependent policy.
class persistent_options
{
public:
    persistent_options(codec_state & state, avifEncoder * encoder) : encoder_(encoder), saved_(encoder->csOptions)
    {
        if (saved_) {
            for (uint32_t i = 0; i < saved_->count; ++i) {
                state.options[saved_->entries[i].key] = saved_->entries[i].value;
            }
        }
        for (auto & [key, value] : state.options) {
            entries_.push_back({ const_cast<char *>(key.data()), value.data() });
        }
        options_.entries = entries_.data();
        options_.count = static_cast<uint32_t>(entries_.size());
        options_.elementSize = sizeof(avifCodecSpecificOption);
        options_.capacity = options_.count;
        encoder_->csOptions = &options_;
    }

    ~persistent_options() { encoder_->csOptions = saved_; }

private:
    avifEncoder * encoder_;
    avifCodecSpecificOptions * saved_;
    std::vector<avifCodecSpecificOption> entries_;
    avifCodecSpecificOptions options_ {};
};

bool fixed_layer_settings_match(const session_request & first, const session_request & next)
{
    // libavif already keeps layer count, dimensions, depth, format, color
    // properties, and each codec's color/alpha role consistent. Only compare
    // NVENC-specific settings that cannot change within a reference sequence.
    const auto & a = first.config;
    const auto & b = next.config;
    return first.pool_id == next.pool_id && compare_guid(a.preset_guid, b.preset_guid) == 0 && a.tuning == b.tuning &&
           a.spatial_aq == b.spatial_aq && a.min_part_size == b.min_part_size && a.max_part_size == b.max_part_size &&
           a.split_encode_mode == b.split_encode_mode && a.clli == b.clli && a.film_grain.has_value() == b.film_grain.has_value();
}

avifResult encode_single_image(avifCodec * codec,
                               avifEncoder * encoder,
                               const avifImage * image,
                               avifBool alpha,
                               int tile_rows_log2,
                               int tile_cols_log2,
                               int quality,
                               avifEncoderChanges encoder_changes,
                               avifBool disable_lagged_output,
                               avifAddImageFlags add_image_flags,
                               avifCodecEncodeOutput * output)
{
    (void)encoder_changes;
    (void)disable_lagged_output;

    auto * internal = reinterpret_cast<codec_state *>(codec->internal);
    if (internal->submitted_image) {
        set_diagnostic(codec->diag, "NVENC custom codec MVP supports one still image per codec instance");
        return AVIF_RESULT_NOT_IMPLEMENTED;
    }

    session_request request;
    avifResult result =
        build_session_request(encoder, image, alpha, tile_rows_log2, tile_cols_log2, quality, add_image_flags, &request, codec->diag);
    if (result != AVIF_RESULT_OK) {
        return result;
    }

    pool_frame frame;
    nvenc_session::encoded_frame encoded;

    result = acquire_pool_frame(request, &frame, codec->diag);
    if (result != AVIF_RESULT_OK) {
        return result;
    }

    result = frame.upload_image(request.config, image, codec->diag);
    if (result != AVIF_RESULT_OK) {
        return result;
    }

    result = frame.submit_frame(request.config, &encoded, codec->diag);
    if (result != AVIF_RESULT_OK) {
        return result;
    }

    // NVENC cannot emit reduced_still_picture_header. Each bitstream holds exactly one frame, so rewrite it.
    // The sequence header's max frame size is the session's bucket size; use the actual image size.
    // NVENC chooses its own film grain seed; use the one from the table instead.
    std::vector<uint8_t> reduced;
    const std::optional<uint16_t> grain_seed = request.config.film_grain.has_value() ? std::optional(request.film_grain_seed)
                                                                                     : std::nullopt;
    result = rewrite_to_reduced_still_picture(encoded.bytes,
                                              request.config.encode_width,
                                              request.config.encode_height,
                                              grain_seed,
                                              &reduced,
                                              codec->diag);
    if (result != AVIF_RESULT_OK) {
        return result;
    }

    result = avifCodecEncodeOutputAddSample(output, reduced.data(), reduced.size(), encoded.sync);
    if (result != AVIF_RESULT_OK) {
        return result;
    }

    internal->submitted_image = AVIF_TRUE;
    return AVIF_RESULT_OK;
}

avifResult encode_layered_image(avifCodec * codec,
                                avifEncoder * encoder,
                                const avifImage * image,
                                avifBool alpha,
                                int rows,
                                int cols,
                                int quality,
                                avifAddImageFlags flags)
{
    auto & state = *reinterpret_cast<codec_state *>(codec->internal);
    if (state.failed || state.finished) {
        set_diagnostic(codec->diag, "NVENC layered codec is already finished or failed");
        return AVIF_RESULT_INVALID_ARGUMENT;
    }

    persistent_options options(state, encoder);
    session_request request;
    auto result = build_session_request(encoder, image, alpha, rows, cols, quality, flags, &request, codec->diag);
    if (result != AVIF_RESULT_OK) {
        state.failed = true;
        if (state.group) {
            state.group->cancel();
        }
        return result;
    }

    const auto index = state.layers.size();
    const auto expected = encoder->extraLayerCount + 1;
    // libavif validates layer sizes, but does not reject excess inputs here.
    if (index >= expected) {
        set_diagnostic(codec->diag, "NVENC received more layers than the declared layer count");
        state.failed = true;
        if (state.group) {
            state.group->cancel();
        }
        return AVIF_RESULT_INVALID_ARGUMENT;
    }

    if (index && !fixed_layer_settings_match(state.layers.front(), request)) {
        set_diagnostic(codec->diag, "NVENC session settings and sequence-level grain enablement must remain fixed within a layered image");
        state.failed = true;
        state.group->cancel();
        return AVIF_RESULT_CANNOT_CHANGE_SETTING;
    }

    if (!state.group) {
        state.owner = encoder;
        state.alpha = bool(alpha);
        state.layer_count = expected;
        state.final_size = { encoder->width, encoder->height };
        state.layers.reserve(expected);
        if (alpha) {
            // libavif encodes color first; its codec owns the admission group.
            std::scoped_lock lock(groups_mutex);
            state.group = groups.at(encoder).lock();
        } else {
            session_request alpha_request;
            if (image->alphaPlane) {
                result = build_session_request(encoder, image, AVIF_TRUE, rows, cols, quality, flags, &alpha_request, codec->diag);
                if (result != AVIF_RESULT_OK) {
                    return result;
                }
            }
            result = acquire_layered_group(request,
                                           image->alphaPlane ? &alpha_request : nullptr,
                                           expected,
                                           state.final_size,
                                           &state.group,
                                           codec->diag);
            if (result != AVIF_RESULT_OK) {
                return result;
            }
            std::scoped_lock lock(groups_mutex);
            groups[encoder] = state.group;
        }
    }

    result = state.group->acquire_upload_image(bool(alpha), request, image, codec->diag);
    if (result != AVIF_RESULT_OK) {
        state.failed = true;
        state.group->cancel();
        return result;
    }

    state.layers.push_back(std::move(request));
    return AVIF_RESULT_OK;
}

avifResult nvenc_codec_encode_image(avifCodec * codec,
                                    avifEncoder * encoder,
                                    const avifImage * image,
                                    avifBool alpha,
                                    int rows,
                                    int cols,
                                    int quality,
                                    avifEncoderChanges changes,
                                    avifBool disable_lagged_output,
                                    avifAddImageFlags flags,
                                    avifCodecEncodeOutput * output)
{
    auto & state = *reinterpret_cast<codec_state *>(codec->internal);
    try {
        if (!encoder->extraLayerCount && !state.layer_count) {
            return encode_single_image(codec, encoder, image, alpha, rows, cols, quality, changes, disable_lagged_output, flags, output);
        }
        return encode_layered_image(codec, encoder, image, alpha, rows, cols, quality, flags);
    } catch (const std::bad_alloc &) {
        set_diagnostic(codec->diag, "Out of memory encoding layered NVENC image");
    } catch (const std::exception & e) {
        set_diagnostic(codec->diag, "{}", e.what());
    }
    state.failed = true;
    if (state.group) {
        state.group->cancel();
    }
    return AVIF_RESULT_UNKNOWN_ERROR;
}

avifBool nvenc_codec_encode_finish(avifCodec * codec, avifCodecEncodeOutput * output)
{
    auto & state = *reinterpret_cast<codec_state *>(codec->internal);
    if (!state.layer_count) {
        return state.failed ? AVIF_FALSE : AVIF_TRUE;
    }

    if (state.failed || state.finished || state.layers.size() != state.layer_count) {
        set_diagnostic(codec->diag, "Incomplete or already finished NVENC layered image");
        if (state.group) {
            state.group->cancel();
        }
        return AVIF_FALSE;
    }

    try {
        std::vector<nvenc_session::encoded_frame> encoded;
        if (state.group->submit_job(state.alpha, &encoded, codec->diag) != AVIF_RESULT_OK) {
            return AVIF_FALSE;
        }

        std::vector<std::vector<uint8_t>> packets;
        std::vector<std::pair<uint32_t, uint32_t>> sizes;
        std::vector<std::optional<uint16_t>> grain_seeds;
        for (size_t i = 0; i < encoded.size(); ++i) {
            packets.push_back(std::move(encoded[i].bytes));
            const auto & layer = state.layers[i];
            sizes.emplace_back(layer.config.encode_width, layer.config.encode_height);
            grain_seeds.push_back(layer.config.film_grain ? std::optional(layer.film_grain_seed) : std::nullopt);
        }

        std::vector<std::vector<uint8_t>> rewritten;
        if (rewrite_layered_picture(packets,
                                    sizes,
                                    state.final_size.width,
                                    state.final_size.height,
                                    grain_seeds,
                                    &rewritten,
                                    codec->diag) != AVIF_RESULT_OK) {
            return AVIF_FALSE;
        }

        for (size_t i = 0; i < rewritten.size(); ++i) {
            if (avifCodecEncodeOutputAddSample(output, rewritten[i].data(), rewritten[i].size(), encoded[i].sync) != AVIF_RESULT_OK) {
                return AVIF_FALSE;
            }
        }
        state.finished = true;
        return AVIF_TRUE;
    } catch (const std::exception & e) {
        set_diagnostic(codec->diag, "{}", e.what());
        state.failed = true;
        state.group->cancel();
        return AVIF_FALSE;
    }
}

void nvenc_codec_destroy_internal(avifCodec * codec)
{
    const auto * internal = reinterpret_cast<codec_state *>(codec->internal);
    if (internal->group && !internal->finished) {
        internal->group->cancel();
    }
    if (internal->owner) {
        std::scoped_lock lock(groups_mutex);
        auto it = groups.find(internal->owner);
        if (it != groups.end() && it->second.lock() == internal->group) {
            groups.erase(it);
        }
    }
    delete internal;
    codec->internal = nullptr;
}

} // namespace

const char * codec_version()
{
    static const std::string version =
        "NVENC API " + std::to_string(NVENCAPI_MAJOR_VERSION) + "." + std::to_string(NVENCAPI_MINOR_VERSION);
    return version.c_str();
}

avifCodec * create_codec()
{
    auto * codec = static_cast<avifCodec *>(avifAlloc(sizeof(avifCodec)));
    if (!codec) {
        return nullptr;
    }
    std::memset(codec, 0, sizeof(avifCodec));

    auto * internal = new (std::nothrow) codec_state();
    if (!internal) {
        avifFree(codec);
        return nullptr;
    }

    codec->internal = reinterpret_cast<avifCodecInternal *>(internal);
    codec->encodeImage = nvenc_codec_encode_image;
    codec->encodeFinish = nvenc_codec_encode_finish;
    codec->destroyInternal = nvenc_codec_destroy_internal;
    return codec;
}

} // namespace avif_nvenc
