/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "etaac_runtime.h"
#include <device-layer/IDeviceLayer.h>
#include <cerrno>
#include <elf.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

static int allocation(ETRuntime *r, size_t bytes, uint64_t *address)
{
    return ff_et_runtime_alloc(r, bytes, address);
}

static void valid_params(ETAACParams *p, uint64_t input, uint64_t state,
                         uint64_t output, uint64_t scratch, uint64_t status)
{
    *p = {};
    p->abi_version = ETAAC_ABI;
    p->count = 1;
    p->active_harts = 64;
    p->operation = ETAAC_SYNTH;
    p->input_addr = input;
    p->input_bytes = sizeof(ETAACInput);
    p->state_addr = state;
    p->state_bytes = sizeof(ETAACState);
    p->output_addr = output;
    p->output_bytes = ETAAC_SAMPLES * sizeof(float);
    p->scratch_addr = scratch;
    p->scratch_bytes = ETAAC_HARTS * ETAAC_SCRATCH_FLOATS * sizeof(float);
    p->status_addr = status;
    p->status_bytes = sizeof(ETAACStatus);
    p->generation = 1;
}

int main()
{
    ETRuntime *r = nullptr;
    char error[1024];
    uint64_t input = 0, state = 0, output = 0, scratch = 0, status = 0;
    unsetenv("FF_ET_SYSEMU"); unsetenv("FF_ET_MEM_CHECK"); unsetenv("FF_ET_DEVICE");

    /* No-device validation leaves mock::creates at zero, exactly like the
     * production pre-constructor guard. */
    mock::reset(); mock::devices = 0;
    CHECK(ff_et_runtime_open(&r, nullptr, error, sizeof(error)) == -ENODEV);
    CHECK(!r && !mock::creates);

    /* A loaded mock ELF lets the typed launch path be tested without hardware. */
    char path[] = "/tmp/etaac-runtime-test-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    unsigned char elf[321] = {};
    Elf64_Ehdr eh{};
    std::memcpy(eh.e_ident, ELFMAG, SELFMAG);
    eh.e_ident[EI_CLASS] = ELFCLASS64;
    eh.e_ident[EI_DATA] = ELFDATA2LSB;
    eh.e_machine = EM_RISCV; eh.e_version = EV_CURRENT;
    eh.e_phoff = sizeof(eh); eh.e_phentsize = sizeof(Elf64_Phdr); eh.e_phnum = 2;
    Elf64_Phdr ph{};
    ph.p_type = PT_LOAD; ph.p_offset = 256; ph.p_filesz = ph.p_memsz = 64;
    std::memcpy(elf, &eh, sizeof(eh)); std::memcpy(elf + sizeof(eh), &ph, sizeof(ph));
    /* The source shim must preserve its SDK-0.19 metadata normalization. */
    Elf64_Phdr metadata{};
    metadata.p_type = 0x70000003; metadata.p_offset = 320; metadata.p_filesz = 1;
    std::memcpy(elf + sizeof(eh) + sizeof(ph), &metadata, sizeof(metadata));
    CHECK(write(fd, elf, sizeof(elf)) == static_cast<ssize_t>(sizeof(elf)));
    close(fd);

    mock::reset();
    CHECK(!ff_et_runtime_open(&r, path, error, sizeof(error)));
    unlink(path);
    CHECK(mock::loads == 1 && ff_et_runtime_shire_mask(r) == 5);
    CHECK(!allocation(r, sizeof(ETAACInput), &input));
    CHECK(!allocation(r, sizeof(ETAACState), &state));
    CHECK(!allocation(r, ETAAC_SAMPLES * sizeof(float), &output));
    CHECK(!allocation(r, ETAAC_HARTS * ETAAC_SCRATCH_FLOATS * sizeof(float), &scratch));
    CHECK(!allocation(r, sizeof(ETAACStatus), &status));

    /* Generic copied DMA path retains 64-byte staging for unaligned callers. */
    unsigned char in[129], out[129];
    std::memset(in, 0x5a, sizeof(in));
    CHECK(!ff_et_runtime_write(r, output, in + 1, 128));
    CHECK(!ff_et_runtime_read(r, out + 1, output, 128));
    CHECK(!std::memcmp(in + 1, out + 1, 128));
    CHECK(ff_et_runtime_write(r, output + 1, in, 64) == -EINVAL);

    ETAACParams p;
    valid_params(&p, input, state, output, scratch, status);
    CHECK(etaac_params_valid(&p));
    CHECK(etaac_rt_launch(r, &p, 0) == -EINVAL);
    CHECK(etaac_rt_launch(r, &p, 2) == -EINVAL);
    CHECK(etaac_rt_launch(r, &p, 4) == -EINVAL);
    CHECK(etaac_rt_launch(r, &p, 5) == -EINVAL);

    p.abi_version = ETAAC_ABI + 1;
    CHECK(!etaac_params_valid(&p));
    CHECK(etaac_rt_launch(r, &p, 1) == -EINVAL);
    p.abi_version = ETAAC_ABI;
    p.count = 0;
    CHECK(!etaac_params_valid(&p));
    CHECK(etaac_rt_launch(r, &p, 1) == -EINVAL);
    p.count = ETAAC_MAX_TASKS + 1;
    CHECK(!etaac_params_valid(&p));
    CHECK(etaac_rt_launch(r, &p, 1) == -EINVAL);
    valid_params(&p, input, state, output, scratch, status);
    p.output_addr++;
    CHECK(!etaac_params_valid(&p));
    CHECK(etaac_rt_launch(r, &p, 1) == -EINVAL);
    valid_params(&p, input, state, output, scratch, status);
    p.input_addr = UINT64_C(0x4000);
    CHECK(etaac_params_valid(&p));
    CHECK(etaac_rt_launch(r, &p, 1) == -EINVAL);
    valid_params(&p, input, state, output, scratch, status);
    p.input_bytes -= 64;
    CHECK(etaac_rt_launch(r, &p, 1) == -EINVAL);

    valid_params(&p, input, state, output, scratch, status);
    CHECK(!etaac_rt_launch(r, &p, 1));
    CHECK(mock::launches == 1);
    mock::stream_error = true;
    CHECK(etaac_rt_launch(r, &p, 1) == -EIO);
    CHECK(etaac_rt_launch(r, &p, 1) == -EIO);
    ff_et_runtime_close(&r);
    CHECK(!r && mock::unloads == 1 && mock::allocations == mock::frees && mock::destroys == 1);

    puts("PASS: generated AAC runtime validates ABI/count/ranges/masks and uses mock-only SDK");
}
