// Copyright 2026 Yuan Tong. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#ifndef AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_EXCEPTION_H
#define AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_EXCEPTION_H

#include <exception>
#include <format>
#include <string>
#include <string_view>

#include "cuda_stub.h"
#include "nvEncodeAPI.h"

#include "avif/avif.h"

namespace avif_nvenc
{

class msg_exception : public std::exception
{
public:
    explicit msg_exception(const std::string & msg_) : msg(msg_) {}
    explicit msg_exception(std::string && msg_) : msg(std::move(msg_)) {}

protected:
    msg_exception() = default;

    [[nodiscard]] const char * what() const noexcept override { return msg.c_str(); }

    std::string msg;
};

class nvenc_exception : public msg_exception
{
    static std::string_view nvenc_error_msg(NVENCSTATUS status);

public:
    explicit nvenc_exception(NVENCSTATUS status_)
        : msg_exception(std::format("nvenc error {}: {}", static_cast<int>(status_), nvenc_error_msg(status_))), status(status_)
    {
    }

    [[nodiscard]] NVENCSTATUS get_status() const noexcept { return status; }

    [[nodiscard]] const char * what() const noexcept override { return msg.c_str(); }

private:
    NVENCSTATUS status;
};

class cuda_exception : public msg_exception
{
public:
    explicit cuda_exception(CUresult result_);

    [[nodiscard]] CUresult get_status() const noexcept { return result; }

    [[nodiscard]] const char * what() const noexcept override { return msg.c_str(); }

private:
    CUresult result;
};

template <typename... Args>
void set_diagnostic(avifDiagnostics * diag, std::format_string<Args...> fmt, Args &&... args)
{
    if (!diag || *diag->error) {
        return;
    }
    constexpr size_t buf_size = AVIF_DIAGNOSTICS_ERROR_BUFFER_SIZE - 1;
    auto result = std::format_to_n(diag->error, buf_size, fmt, std::forward<Args>(args)...);
    *result.out = '\0';
}

} // namespace avif_nvenc

#endif //AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_EXCEPTION_H
