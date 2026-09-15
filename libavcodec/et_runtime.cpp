/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "et_runtime.h"

#include <device-layer/IDeviceLayer.h>
#include <runtime/IRuntime.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <elf.h>
#include <limits>
#include <list>
#include <memory>
#include <new>
#include <string>
#include <system_error>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

/* The C interface is serialized by its caller. Host buffers remain owned until
 * the SDK has stopped, including when a submission or its wait throws. */
struct ETRuntime {
    struct Allocation { std::byte *ptr; size_t size; };
    std::shared_ptr<dev::IDeviceLayer> device_layer;
    rt::RuntimePtr runtime;
    rt::DeviceId device{};
    rt::StreamId stream{};
    rt::KernelId kernel{};
    bool have_stream = false;
    bool have_kernel = false;
    bool pending = false;
    bool poisoned = false;
    uint64_t shire_mask = 0;
    char error[1024] = {};
    char run_dir[64] = {};
    std::list<Allocation> allocations;
    std::vector<std::byte> elf;
    std::byte *staging = nullptr;
    size_t staging_size = 0;
    alignas(64) ETFrameParams launch_params{};

    int fail(int code, const char *operation, const char *message) noexcept {
        std::snprintf(error, sizeof(error), "%s: %s", operation, message);
        return -code;
    }

    int exception(const char *operation) noexcept {
        try { throw; }
        catch (const std::bad_alloc &) { return fail(ENOMEM, operation, "out of memory"); }
        catch (const std::system_error &e) {
            int code = e.code().value();
            if (e.code().category() != std::generic_category() &&
                e.code().category() != std::system_category())
                code = EIO;
            return fail(code > 0 ? code : EIO, operation, e.what());
        }
        catch (const std::exception &e) { return fail(EIO, operation, e.what()); }
        catch (...) { return fail(EIO, operation, "unknown SDK exception"); }
    }

    /* Always inspect the stream queue, even if waitForEvent throws. A failed
     * wait never grants permission to recycle a DMA source/destination. */
    int finish(rt::EventId event, const char *operation) noexcept {
        int ret = 0;
        try {
            if (!runtime->waitForEvent(event))
                ret = fail(ETIMEDOUT, operation, "event wait timed out");
            else
                pending = false;
        } catch (...) { ret = exception(operation); }
        try {
            auto errors = runtime->retrieveStreamErrors(stream);
            if (!errors.empty())
                ret = fail(EIO, operation, errors.front().getString().c_str());
        } catch (...) { if (!ret) ret = exception(operation); }
        if (ret) poisoned = true;
        return ret;
    }

    int submission_failed(const char *operation) noexcept {
        int ret = exception(operation);
        poisoned = true;
        if (have_stream) {
            try {
                auto errors = runtime->retrieveStreamErrors(stream);
                if (!errors.empty())
                    ret = fail(EIO, operation, errors.front().getString().c_str());
            } catch (...) {} // Preserve the original submission error.
        }
        return ret;
    }

    bool contains(uint64_t address, size_t size) const noexcept {
        for (const auto &a : allocations) {
            if (!a.ptr) continue;
            uint64_t base = reinterpret_cast<uintptr_t>(a.ptr);
            if (address >= base && address - base <= a.size &&
                size <= a.size - (address - base))
                return true;
        }
        return false;
    }

    int stage(size_t size) {
        if (size <= staging_size) return 0;
        void *p = nullptr;
        int ret = posix_memalign(&p, ET_CACHE_LINE, size);
        if (ret) return fail(ret, "DMA staging", "aligned host allocation failed");
        std::free(staging);
        staging = static_cast<std::byte *>(p);
        staging_size = size;
        return 0;
    }

    ~ETRuntime() noexcept {
        /* Each cleanup is independent: one SDK failure must not skip the rest.
         * Abort only this stream, never an unrelated device's work. */
        if (runtime) {
            bool quiescent = !pending;
            if (have_stream) {
                if (pending) {
                    try { runtime->waitForEvent(runtime->abortStream(stream), std::chrono::seconds(30)); } catch (...) {}
                    try { runtime->retrieveStreamErrors(stream); } catch (...) {}
                }
                try { quiescent = runtime->waitForStream(stream, std::chrono::seconds(30)); } catch (...) {}
                try { runtime->retrieveStreamErrors(stream); } catch (...) {}
            }
            /* If the device is unresponsive, do not recycle memory underneath
             * outstanding work. SDK destruction releases its memory managers. */
            if (quiescent) {
                if (have_kernel) {
                    try { runtime->unloadCode(kernel); } catch (...) {}
                }
                for (const auto &a : allocations) {
                    if (a.ptr) { try { runtime->freeDevice(device, a.ptr); } catch (...) {} }
                }
                if (have_stream) { try { runtime->destroyStream(stream); } catch (...) {} }
            }
            runtime.reset();
        }
        device_layer.reset();
        std::free(staging);
        if (run_dir[0]) {
            const char *files[] = {"sysemu.log", "pu_uart0_tx.log", "pu_uart1_tx.log",
                                   "spio_uart0_tx.log", "spio_uart1_tx.log"};
            for (const char *file : files) {
                char path[128];
                std::snprintf(path, sizeof(path), "%s/%s", run_dir, file);
                unlink(path);
            }
            rmdir(run_dir);
        }
    }
};

struct FileCloser {
    void operator()(FILE *file) const noexcept { std::fclose(file); }
};

static bool enabled(const char *name) noexcept
{
    const char *value = std::getenv(name);
    return value && !std::strcmp(value, "1");
}

static int required_file(ETRuntime *r, const std::string &path, bool executable = false)
{
    struct stat st;
    if (stat(path.c_str(), &st)) return r->fail(errno, "SDK file", path.c_str());
    if (!S_ISREG(st.st_mode) || !st.st_size)
        return r->fail(EINVAL, "SDK file is not a nonempty regular file", path.c_str());
    if (access(path.c_str(), executable ? X_OK : R_OK))
        return r->fail(errno, "SDK file access", path.c_str());
    return 0;
}

/* loadCode allocates ELF-size + segment BSS bytes, then DMA-copies whole
 * segments. Check its preconditions before the SDK can hit a CHECK or issue
 * an unaligned transfer. The final file padding is not part of any section. */
static int validate_elf(ETRuntime *r)
{
    Elf64_Ehdr eh;
    if (r->elf.size() < sizeof(eh))
        return r->fail(ENOEXEC, "kernel", "truncated ELF header");
    std::memcpy(&eh, r->elf.data(), sizeof(eh));
    if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB || eh.e_machine != EM_RISCV ||
        eh.e_version != EV_CURRENT || eh.e_phentsize != sizeof(Elf64_Phdr) ||
        !eh.e_phnum || eh.e_phoff > r->elf.size() ||
        eh.e_phnum > (r->elf.size() - eh.e_phoff) / sizeof(Elf64_Phdr))
        return r->fail(ENOEXEC, "kernel", "expected a RISC-V ELF64 kernel with valid program headers");
    bool loadable = false;
    size_t extra = 0;
    for (unsigned i = 0; i < eh.e_phnum; ++i) {
        Elf64_Phdr ph;
        std::memcpy(&ph, r->elf.data() + eh.e_phoff + i * sizeof(ph), sizeof(ph));
        /* SDK 0.19 tests (type & PT_LOAD), so odd-valued metadata such as
         * PT_RISCV_ATTRIBUTES/GNU_STACK corrupts its BSS-size/base calculation.
         * Neutralize these non-loadable program headers in our owned copy;
         * sections and their relocation/debug information remain untouched. */
        if (ph.p_type != PT_LOAD) {
            if (ph.p_type & PT_LOAD) {
                ph.p_type = PT_NULL;
                std::memcpy(r->elf.data() + eh.e_phoff + i * sizeof(ph), &ph, sizeof(ph));
            }
            continue;
        }
        if (ph.p_filesz > ph.p_memsz || ph.p_offset > r->elf.size() ||
            ph.p_filesz > r->elf.size() - ph.p_offset ||
            ph.p_memsz > SIZE_MAX - extra ||
            ph.p_offset % ET_CACHE_LINE || ph.p_filesz % ET_CACHE_LINE || ph.p_memsz % ET_CACHE_LINE)
            return r->fail(ENOEXEC, "kernel", "load segments must be in bounds and 64-byte aligned (offset/filesz/memsz)");
        extra += ph.p_memsz - ph.p_filesz;
        if (ph.p_memsz) loadable = true;
    }
    if (!loadable || r->elf.size() > SIZE_MAX - (ET_CACHE_LINE - 1) ||
        extra > SIZE_MAX - ((r->elf.size() + ET_CACHE_LINE - 1) & ~size_t(ET_CACHE_LINE - 1)))
        return r->fail(ENOEXEC, "kernel", "no loadable segment or kernel size overflow");
    r->elf.resize((r->elf.size() + ET_CACHE_LINE - 1) & ~size_t(ET_CACHE_LINE - 1));
    return 0;
}

static int open_runtime(ETRuntime *r, const char *kernel_path)
{
    const char *index_string = std::getenv("FF_ET_DEVICE");
    unsigned long index = 0;
    if (index_string) {
        if (!*index_string) return r->fail(EINVAL, "FF_ET_DEVICE", "expected a nonnegative device index");
        for (const char *p = index_string; *p; ++p)
            if (*p < '0' || *p > '9') return r->fail(EINVAL, "FF_ET_DEVICE", "expected a decimal device index");
        errno = 0;
        index = std::strtoul(index_string, nullptr, 10);
        if (errno || index > INT_MAX) return r->fail(EINVAL, "FF_ET_DEVICE", "device index is out of range");
    }

    /* Validate and read the ELF before touching hardware. Keep its storage
     * alive for loadCode and any error cleanup. NULL is the explicit probe. */
    if (kernel_path) {
        int ret = required_file(r, kernel_path);
        if (ret) return ret;
        std::unique_ptr<FILE, FileCloser> file(std::fopen(kernel_path, "rb"));
        if (!file) return r->fail(errno, "open kernel", kernel_path);
        struct stat st;
        if (fstat(fileno(file.get()), &st)) return r->fail(errno, "stat kernel", kernel_path);
        if (st.st_size <= 0 || static_cast<uintmax_t>(st.st_size) > r->elf.max_size())
            return r->fail(EFBIG, "kernel", "invalid ELF size");
        r->elf.resize(static_cast<size_t>(st.st_size));
        if (std::fread(r->elf.data(), 1, r->elf.size(), file.get()) != r->elf.size())
            return r->fail(EIO, "read kernel", kernel_path);
        ret = validate_elf(r);
        if (ret) return ret;
    }

    if (enabled("FF_ET_SYSEMU")) {
        if (index != 0) return r->fail(ENODEV, "FF_ET_DEVICE", "only emulator device 0 exists");
        const char *sdk_env = std::getenv("FF_ET_SDK");
        const std::string sdk = sdk_env ? sdk_env : "/opt/et";
        if (sdk.empty()) return r->fail(EINVAL, "FF_ET_SDK", "empty SDK path");
        emu::SysEmuOptions options{};
        options.bootromTrampolineToBL2ElfPath = sdk + "/lib/esperanto-fw/BootromTrampolineToBL2/BootromTrampolineToBL2.elf";
        options.spBL2ElfPath = sdk + "/lib/esperanto-fw/ServiceProcessorBL2/fast-boot/ServiceProcessorBL2_fast-boot.elf";
        options.machineMinionElfPath = sdk + "/lib/esperanto-fw/MachineMinion/MachineMinion.elf";
        options.masterMinionElfPath = sdk + "/lib/esperanto-fw/MasterMinion/MasterMinion.elf";
        options.workerMinionElfPath = sdk + "/lib/esperanto-fw/WorkerMinion/WorkerMinion.elf";
        options.executablePath = sdk + "/bin/sys_emu";
        for (const std::string *path : {&options.bootromTrampolineToBL2ElfPath, &options.spBL2ElfPath,
                                      &options.machineMinionElfPath, &options.masterMinionElfPath,
                                      &options.workerMinionElfPath, &options.executablePath}) {
            int ret = required_file(r, *path, path == &options.executablePath);
            if (ret) return ret;
        }
        std::strcpy(r->run_dir, "/tmp/ff-et-XXXXXX");
        if (!mkdtemp(r->run_dir)) {
            r->run_dir[0] = 0;
            return r->fail(errno, "emulator", "cannot create private run directory");
        }
        options.runDir = r->run_dir;
        options.logFile = options.runDir + "/sysemu.log";
        options.puUart0Path = options.runDir + "/pu_uart0_tx.log";
        options.puUart1Path = options.runDir + "/pu_uart1_tx.log";
        options.spUart0Path = options.runDir + "/spio_uart0_tx.log";
        options.spUart1Path = options.runDir + "/spio_uart1_tx.log";
        options.maxCycles = std::numeric_limits<uint64_t>::max();
        options.minionShiresMask = UINT64_C(0x1ffffffff);
        options.startGdb = false;
        options.memcheck = enabled("FF_ET_MEM_CHECK");
        if (options.memcheck) options.additionalOptions.emplace_back("-Werror=memory");
        r->device_layer = dev::IDeviceLayer::createSysEmuDeviceLayer(options);
    } else {
        if (enabled("FF_ET_MEM_CHECK"))
            return r->fail(ENOTSUP, "FF_ET_MEM_CHECK", "memory checker requires FF_ET_SYSEMU=1");
        r->device_layer = dev::IDeviceLayer::createPcieDeviceLayer();
    }
    if (!r->device_layer) return r->fail(ENODEV, "device", "SDK returned no device layer");
    /* The SDK uses CHECK(devicesCount > 0); turn that case into errno before
     * entering its constructor. Reject an unavailable index before it resets
     * any device as part of runtime initialization. */
    int count = r->device_layer->getDevicesCount();
    if (count <= 0 || index >= static_cast<unsigned long>(count))
        return r->fail(ENODEV, "FF_ET_DEVICE", "device index is unavailable");
    r->runtime = rt::IRuntime::create(r->device_layer);
    if (!r->runtime) return r->fail(ENODEV, "runtime", "SDK returned no runtime");
    auto devices = r->runtime->getDevices();
    if (index >= devices.size()) return r->fail(ENODEV, "FF_ET_DEVICE", "device index is unavailable");
    r->device = devices[index];
    r->shire_mask = r->runtime->getDeviceProperties(r->device).computeMinionShireMask_;
    if (!r->shire_mask) return r->fail(ENODEV, "device", "no compute minion shires available");
    r->stream = r->runtime->createStream(r->device);
    r->have_stream = true;
    if (kernel_path) {
        r->pending = true;
        auto load = r->runtime->loadCode(r->stream, r->elf.data(), r->elf.size());
        r->kernel = load.kernel_;
        r->have_kernel = true;
        return r->finish(load.event_, "loadCode");
    }
    return 0;
}

extern "C" int ff_et_runtime_open(ETRuntime **out, const char *kernel_path, char *error, size_t error_size)
{
    if (error && error_size) error[0] = 0;
    if (!out) {
        if (error && error_size) std::snprintf(error, error_size, "open: NULL output pointer");
        return -EINVAL;
    }
    *out = nullptr;
    std::unique_ptr<ETRuntime> r;
    try { r.reset(new ETRuntime); }
    catch (const std::bad_alloc &) {
        if (error && error_size) std::snprintf(error, error_size, "open: out of memory");
        return -ENOMEM;
    } catch (...) {
        if (error && error_size) std::snprintf(error, error_size, "open: construction failed");
        return -EIO;
    }
    int ret;
    try { ret = open_runtime(r.get(), kernel_path); }
    catch (...) { ret = r->submission_failed("open"); }
    if (ret) {
        if (error && error_size) std::snprintf(error, error_size, "%s", r->error);
        return ret;
    }
    *out = r.release();
    return 0;
}

extern "C" void ff_et_runtime_close(ETRuntime **runtime)
{
    if (runtime) { delete *runtime; *runtime = nullptr; }
}

extern "C" const char *ff_et_runtime_error(const ETRuntime *runtime)
{
    return runtime ? runtime->error : "ET runtime is not open";
}

extern "C" uint64_t ff_et_runtime_shire_mask(const ETRuntime *runtime)
{
    return runtime ? runtime->shire_mask : 0;
}

extern "C" int ff_et_runtime_alloc(ETRuntime *r, size_t size, uint64_t *address)
{
    if (address) *address = 0;
    if (!r) return -EINVAL;
    if (r->poisoned) return r->fail(EIO, "alloc", "runtime is unusable after SDK failure");
    if (!address || !size || size > SIZE_MAX - (ET_CACHE_LINE - 1))
        return r->fail(EINVAL, "alloc", "invalid size or output pointer");
    size = (size + ET_CACHE_LINE - 1) & ~size_t(ET_CACHE_LINE - 1);
    try {
        /* Reserve bookkeeping first, so allocation cannot leak on bad_alloc. */
        r->allocations.push_back({nullptr, size});
        auto &allocation = r->allocations.back();
        allocation.ptr = r->runtime->mallocDevice(r->device, size, ET_CACHE_LINE);
        if (!allocation.ptr) {
            r->allocations.pop_back();
            return r->fail(ENOMEM, "alloc", "device allocation failed");
        }
        uint64_t ptr = reinterpret_cast<uintptr_t>(allocation.ptr);
        if (ptr % ET_CACHE_LINE) {
            r->poisoned = true;
            return r->fail(EIO, "alloc", "SDK returned a misaligned device address");
        }
        *address = ptr;
        return 0;
    } catch (...) { return r->exception("alloc"); }
}

extern "C" int ff_et_runtime_free(ETRuntime *r, uint64_t address)
{
    if (!r) return -EINVAL;
    if (!address) return 0;
    if (r->pending) return r->fail(EBUSY, "free", "stream still has an in-flight operation");
    for (auto it = r->allocations.begin(); it != r->allocations.end(); ++it) {
        if (reinterpret_cast<uintptr_t>(it->ptr) == address) {
            try {
                r->runtime->freeDevice(r->device, it->ptr);
                r->allocations.erase(it);
                return 0;
            } catch (...) { return r->exception("free"); }
        }
    }
    return r->fail(EINVAL, "free", "address is not an owned allocation");
}

static int transfer(ETRuntime *r, uint64_t device, void *host, size_t size, bool write)
{
    if (!r) return -EINVAL;
    const char *operation = write ? "DMA write" : "DMA read";
    if (r->poisoned) return r->fail(EIO, operation, "runtime is unusable after SDK failure");
    if (!size) return 0;
    if (!host || device % ET_CACHE_LINE || size % ET_CACHE_LINE || !r->contains(device, size))
        return r->fail(EINVAL, operation, "device range must be owned and address/size aligned to 64 bytes");
    try {
        int ret = r->stage(size);
        if (ret) return ret;
        if (write) std::memcpy(r->staging, host, size);
        auto ptr = reinterpret_cast<std::byte *>(static_cast<uintptr_t>(device));
        r->pending = true;
        auto event = write ? r->runtime->memcpyHostToDevice(r->stream, r->staging, ptr, size, true)
                           : r->runtime->memcpyDeviceToHost(r->stream, ptr, r->staging, size, true);
        ret = r->finish(event, operation);
        if (!ret && !write) std::memcpy(host, r->staging, size);
        return ret;
    } catch (...) {
        return r->submission_failed(operation);
    }
}

extern "C" int ff_et_runtime_write(ETRuntime *r, uint64_t dst, const void *src, size_t size)
{
    return transfer(r, dst, const_cast<void *>(src), size, true);
}

extern "C" int ff_et_runtime_read(ETRuntime *r, void *dst, uint64_t src, size_t size)
{
    return transfer(r, src, dst, size, false);
}

extern "C" int ff_et_runtime_launch(ETRuntime *r, const ETFrameParams *params, uint64_t shire_mask)
{
    if (!r) return -EINVAL;
    if (r->poisoned) return r->fail(EIO, "launch", "runtime is unusable after SDK failure");
    if (!r->have_kernel) return r->fail(EINVAL, "launch", "probe runtime has no loaded code");
    if (!params || params->abi_version != ET_MPEG2_ABI_VERSION ||
        !shire_mask || (shire_mask & (shire_mask - 1)) || (shire_mask & ~r->shire_mask))
        return r->fail(EINVAL, "launch", "invalid ABI or mask: select exactly one available compute shire");
    if (!params->input_bytes || !params->frame_bytes ||
        !params->nb_slices || params->nb_slices > ET_MPEG2_MAX_SLICES ||
        !params->active_harts || params->active_harts > ET_MPEG2_HARTS ||
        params->input_addr % ET_CACHE_LINE || params->dst_addr % ET_CACHE_LINE ||
        params->status_addr % ET_CACHE_LINE || params->ref_fwd_addr % ET_CACHE_LINE ||
        params->ref_bwd_addr % ET_CACHE_LINE ||
        !r->contains(params->input_addr, params->input_bytes) ||
        !r->contains(params->dst_addr, params->frame_bytes) ||
        !r->contains(params->status_addr, size_t(params->nb_slices) * sizeof(ETSliceStatus)) ||
        (params->ref_fwd_addr && !r->contains(params->ref_fwd_addr, params->frame_bytes)) ||
        (params->ref_bwd_addr && !r->contains(params->ref_bwd_addr, params->frame_bytes)))
        return r->fail(EINVAL, "launch", "invalid frame parameters or unowned/misaligned device buffers");
    try {
        r->launch_params = *params;
        rt::KernelLaunchOptions options;
        options.setShireMask(shire_mask);
        options.setBarrier(true);
        options.setFlushL3(false);
        r->pending = true;
        return r->finish(r->runtime->kernelLaunch(r->stream, r->kernel,
                         reinterpret_cast<const std::byte *>(&r->launch_params),
                         sizeof(r->launch_params), options), "kernelLaunch");
    } catch (...) {
        return r->submission_failed("kernelLaunch");
    }
}
