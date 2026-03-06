// Copyright 2026 Yuan Tong. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#ifndef AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_DYN_LOADER_H
#define AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_DYN_LOADER_H

#include <atomic>
#include <format>
#include <string>
#include <tuple>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

#include "exception.h"

namespace avif_nvenc
{

extern void atomic_loader_loading_sentinel();
extern void atomic_loader_bad_sentinel();

template <typename Ptr_>
class atomic_loader
{
public:
    using Ptr = Ptr_;

    Ptr get()
    {
        Ptr current = nullptr;
        if (ptr.compare_exchange_strong(current, loading(), std::memory_order_relaxed, std::memory_order_relaxed)) {
            // we should fill the value
            current = load();
            if (current == nullptr) {
                current = bad();
            }
            ptr.store(current, std::memory_order_release);
            ptr.notify_all();
        } else if (current == loading()) {
            // wait for another thread to fill the value
            ptr.wait(loading(), std::memory_order_relaxed);
            current = ptr.load(std::memory_order_relaxed);
        }

        if (current == bad()) {
            throw_bad_load();
        }
        return current;
    }

    virtual ~atomic_loader() = default;

protected:
    virtual Ptr load() = 0;

    [[noreturn]] virtual void throw_bad_load() { throw msg_exception("cannot load resource"); }

private:
    static constexpr Ptr loading() { return reinterpret_cast<Ptr>(&atomic_loader_loading_sentinel); }
    static constexpr Ptr bad() { return reinterpret_cast<Ptr>(&atomic_loader_bad_sentinel); }

    std::atomic<Ptr> ptr { nullptr };
};

#if defined(_WIN32)
using module_t = HMODULE;
#else
using module_t = void *;
#endif

class module_loader : public atomic_loader<module_t>
{
    std::string module_name;

public:
    explicit module_loader(std::string name) : module_name(std::move(name)) {}
    using atomic_loader<module_t>::get;

    template <typename FPtr>
    FPtr function(const std::string & name)
    {
#if defined(_WIN32)
        return reinterpret_cast<FPtr>(GetProcAddress(get(), name.c_str()));
#else
        return reinterpret_cast<FPtr>(dlsym(get(), name.c_str()));
#endif
    }

    ~module_loader() override = default;

private:
    module_t load() final
    {
#if defined(_WIN32)
        return LoadLibraryA(module_name.c_str());
#else
        return dlopen(module_name.c_str(), RTLD_LAZY);
#endif
    }

    [[noreturn]] void throw_bad_load() final
    {
#if defined(_WIN32)
        throw msg_exception(std::format("failed loading module {}: {:#08x}", module_name, GetLastError()));
#else
        throw msg_exception(std::format("failed loading module {}: {}", module_name, dlerror()));
#endif
    }

    template <typename Ptr2>
    friend class function_loader;
};

template <typename Ptr>
class function_loader : public atomic_loader<Ptr>
{
    template <typename>
    struct function_traits
    {
    };

    template <typename Ret, typename... Args>
    struct function_traits<Ret (*)(Args...)>
    {
        using return_t = Ret;
        using args_t = std::tuple<Args...>;
    };

protected:
    Ptr load() final { return get_module().template function<Ptr>(function_name); }
    explicit function_loader(std::string name) : function_name(std::move(name)) {}
    virtual module_loader & get_module() = 0;

public:
    using atomic_loader<Ptr>::get;

    Ptr operator*() { return get(); }

    template <typename... Args>
    typename function_traits<Ptr>::return_t operator()(Args... args)
    {
        return get()(std::forward<Args>(args)...);
    }

private:
    std::string function_name;
    [[noreturn]] void throw_bad_load() final
    {
#if defined(_WIN32)
        throw msg_exception(
            std::format("failed loading function {} from module {}: {:#08x}", function_name, get_module().module_name, GetLastError()));
#else
        throw msg_exception(
            std::format("failed loading function {} from module {}: {}", function_name, get_module().module_name, dlerror()));
#endif
    }
};

} // namespace avif_nvenc

#endif //AVIF_CONTRIB_CUSTOM_CODEC_NVIDIA_DYN_LOADER_H
