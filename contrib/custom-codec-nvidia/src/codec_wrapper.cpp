// Copyright 2026 Yuan Tong. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#include "codec_wrapper.h"

#include <cstring>
#include <new>
#include <string>

#include "codec_options.h"
#include "exception.h"
#include "runtime.h"
#include "session.h"

namespace avif_nvenc
{

struct codec_state
{
    avifBool submitted_image = AVIF_FALSE;
};

namespace
{

avifResult nvenc_codec_encode_image(avifCodec * codec,
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

    result = avifCodecEncodeOutputAddSample(output, encoded.bytes.data(), encoded.bytes.size(), encoded.sync);
    if (result != AVIF_RESULT_OK) {
        return result;
    }

    internal->submitted_image = AVIF_TRUE;
    return AVIF_RESULT_OK;
}

avifBool nvenc_codec_encode_finish(avifCodec * codec, avifCodecEncodeOutput * output)
{
    (void)codec;
    (void)output;
    return AVIF_TRUE;
}

void nvenc_codec_destroy_internal(avifCodec * codec)
{
    const auto * internal = reinterpret_cast<codec_state *>(codec->internal);
    delete internal;
    codec->internal = nullptr;
}

} // namespace

const char * codec_version()
{
    static const std::string version = "NVENC API " + std::to_string(NVENCAPI_MAJOR_VERSION) + "." + std::to_string(NVENCAPI_MINOR_VERSION);
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
