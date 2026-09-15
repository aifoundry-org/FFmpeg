/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
namespace mock {
inline int devices = 1, creates = 0, destroys = 0, streams = 0, loads = 0, unloads = 0;
inline int allocations = 0, frees = 0, waits = 0, retrieves = 0, launches = 0, aborts = 0;
inline bool stream_error = false, wait_throw = false, wait_timeout = false, submit_throw = false;
inline bool alloc_throw = false, free_throw = false;
inline void reset() {
    devices = 1;
    creates = destroys = streams = loads = unloads = allocations = frees = waits = retrieves = launches = aborts = 0;
    stream_error = wait_throw = wait_timeout = submit_throw = alloc_throw = free_throw = false;
}
}
namespace emu {
struct SysEmuOptions {
    std::string bootromTrampolineToBL2ElfPath, spBL2ElfPath, machineMinionElfPath;
    std::string masterMinionElfPath, workerMinionElfPath, executablePath, runDir, logFile;
    std::string puUart0Path, puUart1Path, spUart0Path, spUart1Path;
    uint64_t maxCycles, minionShiresMask;
    bool startGdb, memcheck;
    std::vector<std::string> additionalOptions;
};
}
namespace dev {
struct IDeviceLayer {
    static std::unique_ptr<IDeviceLayer> createPcieDeviceLayer() { return std::make_unique<IDeviceLayer>(); }
    static std::unique_ptr<IDeviceLayer> createSysEmuDeviceLayer(const emu::SysEmuOptions &) {
        return std::make_unique<IDeviceLayer>();
    }
    int getDevicesCount() { return mock::devices; }
};
}
