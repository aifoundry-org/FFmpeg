/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "et_runtime.h"
#include <device-layer/IDeviceLayer.h>
#include <cerrno>
#include <elf.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

int main()
{
    ETRuntime *r = nullptr;
    char error[1024];
    uint64_t ptr = 0;
    unsigned char a[129], b[129];
    memset(a, 37, sizeof(a));
    unsetenv("FF_ET_SYSEMU"); unsetenv("FF_ET_MEM_CHECK"); unsetenv("FF_ET_DEVICE");
    CHECK(ff_et_runtime_open(nullptr, nullptr, error, sizeof(error)) == -EINVAL);
    CHECK(ff_et_runtime_alloc(nullptr, 64, &ptr) == -EINVAL);
    CHECK(ff_et_runtime_shire_mask(nullptr) == 0);
    CHECK(ff_et_runtime_error(nullptr));
    ff_et_runtime_close(nullptr);
    mock::reset(); mock::devices = 0;
    CHECK(ff_et_runtime_open(&r, nullptr, error, sizeof(error)) == -ENODEV);
    CHECK(!r && !mock::creates);
    for (const char *value : {"-1", "", "1junk", "99999999999999999999999999999"}) {
        setenv("FF_ET_DEVICE", value, 1);
        CHECK(ff_et_runtime_open(&r, nullptr, error, sizeof(error)) == -EINVAL);
    }
    unsetenv("FF_ET_DEVICE");
    mock::reset();
    CHECK(!ff_et_runtime_open(&r, nullptr, error, sizeof(error)));
    CHECK(mock::creates == 1 && mock::streams == 1 && mock::loads == 0);
    CHECK(ff_et_runtime_shire_mask(r) == 5);
    CHECK(ff_et_runtime_launch(r, nullptr, 1) == -EINVAL);
    CHECK(!ff_et_runtime_alloc(r, 65, &ptr));
    CHECK(!ff_et_runtime_write(r, ptr, a + 1, 128));
    CHECK(!ff_et_runtime_read(r, b + 1, ptr, 128));
    CHECK(!memcmp(a + 1, b + 1, 128));
    CHECK(mock::waits == 2 && mock::retrieves == 2);
    CHECK(ff_et_runtime_write(r, ptr + 1, a, 64) == -EINVAL);
    CHECK(ff_et_runtime_read(r, b, ptr, 65) == -EINVAL);
    CHECK(ff_et_runtime_read(r, b, ptr + 128, 64) == -EINVAL);
    CHECK(ff_et_runtime_alloc(r, SIZE_MAX, &ptr) == -EINVAL);
    ff_et_runtime_close(&r);
    CHECK(!r && mock::allocations == mock::frees && mock::streams == 0 && mock::destroys == 1);
    ff_et_runtime_close(&r);

    /* Every asynchronous failure must inspect the queue and retain ownership. */
    for (int failure = 0; failure != 4; ++failure) {
        mock::reset();
        CHECK(!ff_et_runtime_open(&r, nullptr, error, sizeof(error)));
        CHECK(!ff_et_runtime_alloc(r, 128, &ptr));
        mock::stream_error = failure == 0;
        mock::wait_throw = failure == 1;
        mock::wait_timeout = failure == 2;
        mock::submit_throw = failure == 3;
        CHECK(ff_et_runtime_write(r, ptr, a, 128) == (failure == 2 ? -ETIMEDOUT : -EIO));
        CHECK(mock::retrieves == 1);
        CHECK(ff_et_runtime_read(r, b, ptr, 128) == -EIO);
        if (failure != 0) CHECK(ff_et_runtime_free(r, ptr) == -EBUSY);
        ff_et_runtime_close(&r);
        CHECK(mock::frees == 1 && mock::destroys == 1 && mock::streams == 0);
        CHECK(mock::aborts == (failure == 0 ? 0 : 1));
    }
    mock::reset();
    CHECK(!ff_et_runtime_open(&r, nullptr, error, sizeof(error)));
    mock::alloc_throw = true;
    CHECK(ff_et_runtime_alloc(r, 64, &ptr) == -ENOMEM);
    mock::alloc_throw = false;
    CHECK(!ff_et_runtime_alloc(r, 64, &ptr));
    mock::free_throw = true;
    CHECK(ff_et_runtime_free(r, ptr) == -EIO);
    mock::free_throw = false;
    ff_et_runtime_close(&r);
    CHECK(mock::frees == 1);

    char path[] = "/tmp/et-runtime-test-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    unsigned char elf[321] = {};
    Elf64_Ehdr eh{};
    memcpy(eh.e_ident, ELFMAG, SELFMAG);
    eh.e_ident[EI_CLASS] = ELFCLASS64;
    eh.e_ident[EI_DATA] = ELFDATA2LSB;
    eh.e_machine = EM_RISCV; eh.e_version = EV_CURRENT;
    eh.e_phoff = sizeof(eh); eh.e_phentsize = sizeof(Elf64_Phdr); eh.e_phnum = 3;
    Elf64_Phdr ph{};
    ph.p_type = PT_LOAD; ph.p_offset = 256; ph.p_filesz = ph.p_memsz = 64;
    memcpy(elf, &eh, sizeof(eh)); memcpy(elf + sizeof(eh), &ph, sizeof(ph));
    Elf64_Phdr metadata{};
    metadata.p_type = 0x70000003; metadata.p_offset = 320; metadata.p_filesz = 1;
    memcpy(elf + sizeof(eh) + sizeof(ph), &metadata, sizeof(metadata));
    metadata.p_type = PT_GNU_STACK; metadata.p_filesz = 0;
    memcpy(elf + sizeof(eh) + 2 * sizeof(ph), &metadata, sizeof(metadata));
    CHECK(write(fd, elf, sizeof(elf)) == sizeof(elf));
    close(fd);
    mock::reset(); mock::stream_error = true;
    CHECK(ff_et_runtime_open(&r, path, error, sizeof(error)) == -EIO);
    CHECK(!r && mock::loads == 1 && mock::unloads == 1 && mock::destroys == 1);
    mock::reset();
    CHECK(!ff_et_runtime_open(&r, path, error, sizeof(error)));
    unlink(path);
    CHECK(mock::loads == 1 && mock::waits == 1 && mock::retrieves == 1);
    CHECK(!ff_et_runtime_alloc(r, 256, &ptr));
    ETFrameParams p{};
    p.abi_version = ET_MPEG2_ABI_VERSION;
    p.input_addr = p.dst_addr = p.status_addr = ptr;
    p.input_bytes = p.frame_bytes = 128;
    p.nb_slices = 1; p.active_harts = 64;
    CHECK(ff_et_runtime_launch(r, &p, 0) == -EINVAL);
    CHECK(ff_et_runtime_launch(r, &p, 2) == -EINVAL);
    CHECK(ff_et_runtime_launch(r, &p, 5) == -EINVAL);
    CHECK(!ff_et_runtime_launch(r, &p, 4));
    CHECK(mock::launches == 1 && mock::waits == 2 && mock::retrieves == 2);
    mock::stream_error = true;
    CHECK(ff_et_runtime_launch(r, &p, 1) == -EIO);
    ff_et_runtime_close(&r);
    CHECK(mock::unloads == 1 && mock::frees == 1);
    puts("PASS: runtime C ABI, probe, staging, bounds, masks, failure injection and cleanup");
}
