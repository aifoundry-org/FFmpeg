/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include <device-layer/IDeviceLayer.h>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <elf.h>
#include <map>
namespace rt {
enum class DeviceId : int {};
enum class StreamId : int {};
enum class KernelId : int {};
enum class EventId : int {};
struct DeviceProperties { uint32_t computeMinionShireMask_ = 5; };
struct LoadCodeResult { EventId event_{}; KernelId kernel_{}; };
struct StreamError { std::string getString() const { return "injected stream error"; } };
struct KernelLaunchOptions {
    uint64_t mask = 0;
    void setShireMask(uint64_t v) { mask = v; }
    void setBarrier(bool) {}
    void setFlushL3(bool) {}
};
class IRuntime;
using RuntimePtr = std::unique_ptr<IRuntime>;
class IRuntime {
    std::map<std::byte *, size_t> memory;
public:
    static RuntimePtr create(const std::shared_ptr<dev::IDeviceLayer> &) {
        ++mock::creates;
        return std::make_unique<IRuntime>();
    }
    ~IRuntime() { ++mock::destroys; for (auto a : memory) std::free(a.first); }
    std::vector<DeviceId> getDevices() { return {DeviceId{0}}; }
    DeviceProperties getDeviceProperties(DeviceId) { return {}; }
    StreamId createStream(DeviceId) { ++mock::streams; return {}; }
    void destroyStream(StreamId) { --mock::streams; }
    LoadCodeResult loadCode(StreamId, const std::byte *data, size_t size) {
        if (size % 64) throw std::runtime_error("unaligned ELF allocation size");
        Elf64_Ehdr eh;
        std::memcpy(&eh, data, sizeof(eh));
        for (unsigned i = 0; i < eh.e_phnum; ++i) {
            Elf64_Phdr ph;
            std::memcpy(&ph, data + eh.e_phoff + i * sizeof(ph), sizeof(ph));
            if ((ph.p_type & PT_LOAD) && ph.p_type != PT_LOAD)
                throw std::runtime_error("metadata header would confuse SDK loadCode");
        }
        ++mock::loads;
        return {};
    }
    void unloadCode(KernelId) { ++mock::unloads; }
    bool waitForEvent(EventId, std::chrono::seconds = std::chrono::seconds(1)) {
        ++mock::waits;
        if (mock::wait_throw) throw std::runtime_error("injected wait exception");
        return !mock::wait_timeout;
    }
    bool waitForStream(StreamId, std::chrono::seconds = std::chrono::seconds(1)) { return true; }
    std::vector<StreamError> retrieveStreamErrors(StreamId) {
        ++mock::retrieves;
        if (mock::stream_error) return {StreamError{}};
        return {};
    }
    EventId abortStream(StreamId) { ++mock::aborts; return {}; }
    std::byte *mallocDevice(DeviceId, size_t size, uint32_t alignment) {
        if (mock::alloc_throw) throw std::bad_alloc();
        if (alignment != 64 || size % 64) throw std::runtime_error("unaligned device allocation");
        auto p = static_cast<std::byte *>(std::aligned_alloc(alignment, size));
        if (!p) throw std::bad_alloc();
        memory[p] = size;
        ++mock::allocations;
        return p;
    }
    void freeDevice(DeviceId, std::byte *p) {
        if (mock::free_throw) throw std::runtime_error("injected free failure");
        if (!memory.erase(p)) throw std::runtime_error("invalid free");
        std::free(p);
        ++mock::frees;
    }
    EventId memcpyHostToDevice(StreamId, const std::byte *src, std::byte *dst, size_t size, bool barrier) {
        if (mock::submit_throw) throw std::runtime_error("injected submission failure");
        if (reinterpret_cast<uintptr_t>(src) % 64 || reinterpret_cast<uintptr_t>(dst) % 64 || size % 64 || !barrier)
            throw std::runtime_error("unaligned DMA");
        std::memcpy(dst, src, size);
        return {};
    }
    EventId memcpyDeviceToHost(StreamId s, const std::byte *src, std::byte *dst, size_t size, bool b) {
        return memcpyHostToDevice(s, src, dst, size, b);
    }
    EventId kernelLaunch(StreamId, KernelId, const std::byte *p, size_t size, const KernelLaunchOptions &o) {
        if (reinterpret_cast<uintptr_t>(p) % 64 || size != 128 || !o.mask || (o.mask & (o.mask - 1)))
            throw std::runtime_error("invalid launch alignment/mask");
        ++mock::launches;
        return {};
    }
};
}
