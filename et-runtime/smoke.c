/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "et_runtime.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    ETRuntime *runtime = NULL;
    char error[1024];
    uint64_t address = 0;
    unsigned char input[129], output[129];
    int ret;
    const char *kernel = NULL;
    if (argc == 3 && !strcmp(argv[1], "--load"))
        kernel = argv[2];
    else if (argc != 2 || (strcmp(argv[1], "--probe") && strcmp(argv[1], "--dma"))) {
        fprintf(stderr, "Usage: %s --probe|--dma|--load kernel.elf (FF_ET_SYSEMU=1 selects emulator)\n", argv[0]);
        return 2;
    }
    ret = ff_et_runtime_open(&runtime, kernel, error, sizeof(error));
    if (ret < 0) {
        fprintf(stderr, "open failed (%d): %s\n", ret, error);
        return 1;
    }
    printf("%s OK: compute shire mask=0x%" PRIx64 "\n", kernel ? "load" : "probe", ff_et_runtime_shire_mask(runtime));
    if (!strcmp(argv[1], "--dma")) {
        for (size_t i = 0; i < sizeof(input); ++i) input[i] = (unsigned char)(i * 31 + 7);
        memset(output, 0, sizeof(output));
        if ((ret = ff_et_runtime_alloc(runtime, 127, &address)) < 0 ||
            (ret = ff_et_runtime_write(runtime, address, input + 1, 128)) < 0 ||
            (ret = ff_et_runtime_read(runtime, output + 1, address, 128)) < 0)
            goto failed;
        if (memcmp(input + 1, output + 1, 128)) {
            fprintf(stderr, "DMA roundtrip mismatch\n");
            ret = -EIO;
            goto done;
        }
        if (ff_et_runtime_write(runtime, address + 1, input, 64) != -EINVAL ||
            ff_et_runtime_read(runtime, output, address, 63) != -EINVAL ||
            ff_et_runtime_read(runtime, output, address + 128, 64) != -EINVAL) {
            fprintf(stderr, "DMA invalid argument unexpectedly accepted\n");
            ret = -EIO;
            goto done;
        }
        if ((ret = ff_et_runtime_free(runtime, address)) < 0) goto failed;
        if (ff_et_runtime_free(runtime, address) != -EINVAL) { ret = -EIO; goto done; }
        /* Deliberately leave one allocation for deterministic close cleanup. */
        if ((ret = ff_et_runtime_alloc(runtime, 64, &address)) < 0) goto failed;
        printf("DMA OK: unaligned host staging, roundtrip, bounds/alignment rejection, free\n");
    }
    ret = 0;
    goto done;
failed:
    fprintf(stderr, "operation failed (%d): %s\n", ret, ff_et_runtime_error(runtime));
done:
    ff_et_runtime_close(&runtime);
    ff_et_runtime_close(&runtime);
    return ret < 0;
}
