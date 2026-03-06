// Copyright 2026 Yuan Tong. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#ifndef AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_SESSION_H
#define AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_SESSION_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "avif/codec.h"
#include "codec_options.h"
#include "cuda_stub.h"
#include "nvEncodeAPI.h"

namespace avif_nvenc
{

class nvenc_session
{
public:
    static constexpr size_t kFrameCapacity = 32;

    struct encoded_frame
    {
        std::vector<uint8_t> bytes;
        avifBool sync = AVIF_FALSE;
    };

    class frame_buffer
    {
    public:
        frame_buffer() = default;
        frame_buffer(const frame_buffer &) = delete;
        frame_buffer & operator=(const frame_buffer &) = delete;
        frame_buffer(frame_buffer && other) noexcept;
        frame_buffer & operator=(frame_buffer && other) noexcept;
        ~frame_buffer();

        explicit operator bool() const noexcept;

    private:
        friend class nvenc_session;

        explicit frame_buffer(nvenc_session * session, size_t slot_index) noexcept;
        void detach() noexcept;

        nvenc_session * session_ = nullptr;
        size_t slot_index_ = static_cast<size_t>(-1);
        uint32_t pitch_ = 0;
    };

    nvenc_session(const session_key & key,
                  const session_config & config,
                  CUcontext cuda_context,
                  const NV_ENCODE_API_FUNCTION_LIST & api_functions);
    ~nvenc_session();

    AVIF_NODISCARD avifResult acquire_frame(frame_buffer * frame, avifDiagnostics * diag);
    AVIF_NODISCARD avifResult upload_image(frame_buffer * frame,
                                          const session_config & config,
                                          const avifImage * image,
                                          avifDiagnostics * diag) const;
    AVIF_NODISCARD avifResult submit_frame(frame_buffer * frame,
                                          const session_config & config,
                                          encoded_frame * encoded,
                                          avifDiagnostics * diag) const;

private:
    friend class frame_buffer;

    void release_frame(frame_buffer * frame) const noexcept;

    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace avif_nvenc

#endif // AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_SESSION_H
