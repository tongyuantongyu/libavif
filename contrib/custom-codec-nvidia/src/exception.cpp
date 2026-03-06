// Copyright 2026 Yuan Tong. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#include "exception.h"
#include "cuda_driver.h"

namespace avif_nvenc
{

std::string_view nvenc_exception::nvenc_error_msg(NVENCSTATUS status)
{
    switch (status) {
        case NV_ENC_SUCCESS:
            return "";
        case NV_ENC_ERR_NO_ENCODE_DEVICE:
            return "no encode capable devices";
        case NV_ENC_ERR_UNSUPPORTED_DEVICE:
            return "device not supported";
        case NV_ENC_ERR_INVALID_ENCODERDEVICE:
            return "encoder device is invalid";
        case NV_ENC_ERR_INVALID_DEVICE:
            return "device is invalid";
        case NV_ENC_ERR_DEVICE_NOT_EXIST:
            return "device gone";
        case NV_ENC_ERR_INVALID_PTR:
            return "invalid pointer";
        case NV_ENC_ERR_INVALID_EVENT:
            return "invalid event";
        case NV_ENC_ERR_INVALID_PARAM:
            return "invalid parameter";
        case NV_ENC_ERR_INVALID_CALL:
            return "invalid call";
        case NV_ENC_ERR_OUT_OF_MEMORY:
            return "out of memory";
        case NV_ENC_ERR_ENCODER_NOT_INITIALIZED:
            return "encoder not initialized";
        case NV_ENC_ERR_UNSUPPORTED_PARAM:
            return "unsupported parameter";
        case NV_ENC_ERR_LOCK_BUSY:
            return "buffer fulfilling, try again";
        case NV_ENC_ERR_NOT_ENOUGH_BUFFER:
            return "buffer too small";
        case NV_ENC_ERR_INVALID_VERSION:
            return "invalid struct version";
        case NV_ENC_ERR_MAP_FAILED:
            return "failed mapping input resource";
        case NV_ENC_ERR_NEED_MORE_INPUT:
            return "need more input, try again";
        case NV_ENC_ERR_ENCODER_BUSY:
            return "encoder busy, try again";
        case NV_ENC_ERR_EVENT_NOT_REGISTERD:
            return "unknown completion event";
        case NV_ENC_ERR_GENERIC:
            return "internal error";
        case NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY:
            return "feature unavailable, need purchase";
        case NV_ENC_ERR_UNIMPLEMENTED:
            return "feature unimplemented";
        case NV_ENC_ERR_RESOURCE_REGISTER_FAILED:
            return "failed register resource";
        case NV_ENC_ERR_RESOURCE_NOT_REGISTERED:
            return "resource not registered";
        case NV_ENC_ERR_RESOURCE_NOT_MAPPED:
            return "resource not mapped";
        case NV_ENC_ERR_NEED_MORE_OUTPUT:
            return "need more output, try again";
    }

    return "unknown error";
}

namespace cuda {}

cuda_exception::cuda_exception(const CUresult result_) : result(result_)
{
    using namespace cuda;

    const char * name = nullptr;
    const char * message = nullptr;
    if (const auto r = cuGetErrorName(result, &name); r != CUDA_SUCCESS) {
        msg = std::format("cuda error {}: unknown error", static_cast<int>(result));
        return;
    }
    if (const auto r = cuGetErrorString(result, &message); r != CUDA_SUCCESS) {
        msg = std::format("cuda error {}({}): no message", name, static_cast<int>(result));
        return;
    }
    msg = std::format("cuda error {}({}): {}", name, static_cast<int>(result), message);
}

} // namespace avif_nvenc
