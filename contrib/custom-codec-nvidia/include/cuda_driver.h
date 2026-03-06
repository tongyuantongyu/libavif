// Copyright 2026 Yuan Tong. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#ifndef AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_CUDA_DRIVER_H
#define AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_CUDA_DRIVER_H

#include "cuda_stub.h"
#include "dyn_loader.h"

namespace avif_nvenc::cuda {

extern module_loader module;

template <typename Ptr>
class cuda_loader : function_loader<Ptr>
{
  using Base = function_loader<Ptr>;

  public:
    explicit cuda_loader(std::string name) : Base(std::move(name)) {}
    using Base::get;
    using Base::operator*;
    using Base::operator();

  private:
    module_loader & get_module() final { return module; }
};

#pragma clang diagnostic push
#pragma ide diagnostic ignored "bugprone-macro-parentheses"
#define CUDA_API_DECLARE(name) extern cuda_loader<decltype(::name) *> name
#define CUDA_API_DECLARE_ALIAS(name, symbol) extern cuda_loader<decltype(::symbol) *> name
#pragma clang diagnostic pop

CUDA_API_DECLARE(cuGetErrorString);
CUDA_API_DECLARE(cuGetErrorName);
CUDA_API_DECLARE(cuInit);
CUDA_API_DECLARE(cuDeviceGet);
CUDA_API_DECLARE(cuDevicePrimaryCtxRetain);
CUDA_API_DECLARE_ALIAS(cuDevicePrimaryCtxRelease, cuDevicePrimaryCtxRelease_v2);

#undef CUDA_API_DECLARE
#undef CUDA_API_DECLARE_ALIAS

}

#endif //AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_CUDA_DRIVER_H
