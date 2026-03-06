// Copyright 2026 Yuan Tong. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#ifndef AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_H
#define AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_H

#include "nvEncodeAPI.h"
#include "dyn_loader.h"

namespace avif_nvenc::nvenc
{

extern module_loader module;


template <typename Ptr>
class nvenc_loader : function_loader<Ptr>
{
    using Base = function_loader<Ptr>;

public:
    explicit nvenc_loader(std::string name) : Base(std::move(name)) {}
    using Base::get;
    using Base::operator*;
    using Base::operator();

private:
    module_loader & get_module() final { return module; }
};

#pragma clang diagnostic push
#pragma ide diagnostic ignored "bugprone-macro-parentheses"
#define NVENC_API_DECLARE(name) extern nvenc_loader<decltype(::name) *> name
#pragma clang diagnostic pop

NVENC_API_DECLARE(NvEncodeAPIGetMaxSupportedVersion);
NVENC_API_DECLARE(NvEncodeAPICreateInstance);


}

#endif //AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_H
