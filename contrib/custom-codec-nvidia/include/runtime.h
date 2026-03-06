// Copyright 2026 Yuan Tong. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#ifndef AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_RUNTIME_H
#define AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_RUNTIME_H

#include <string_view>

#include "avif/avif.h"
#include "codec_options.h"
#include "session.h"

namespace avif_nvenc
{

class encoder_pool;

class pool_frame
{
public:
    pool_frame() = default;
    pool_frame(const pool_frame &) = delete;
    pool_frame & operator=(const pool_frame &) = delete;
    pool_frame(pool_frame && other) noexcept;
    pool_frame & operator=(pool_frame && other) noexcept;
    ~pool_frame();

    explicit operator bool() const noexcept;
    AVIF_NODISCARD avifResult upload_image(const session_config & config, const avifImage * image, avifDiagnostics * diag);
    AVIF_NODISCARD avifResult submit_frame(const session_config & config, nvenc_session::encoded_frame * encoded, avifDiagnostics * diag);
    void reset();

private:
    friend class encoder_pool;

    void attach(encoder_pool * pool, nvenc_session * session, nvenc_session::frame_buffer frame) noexcept;

    encoder_pool * pool_ = nullptr;
    nvenc_session * session_ = nullptr;
    nvenc_session::frame_buffer frame_;
};

AVIF_NODISCARD avifResult create_pool(const registered_pool_options & options, avifDiagnostics * diag);
AVIF_NODISCARD avifResult shutdown_pool(std::string_view pool_id, avifDiagnostics * diag);
AVIF_NODISCARD avifResult acquire_pool_frame(const session_request & request, pool_frame * frame, avifDiagnostics * diag);

} // namespace avif_nvenc

#endif // AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_RUNTIME_H
