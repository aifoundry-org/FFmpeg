# Private FFmpeg `-mno-fdiv` rebuild

Build only in the SDK container, with PCIe disabled:

```sh
FF_ET_ALLOW_PCIE=0 et-tools/et-env env FF_ET_ALLOW_PCIE=0 \
  python3 et-audio/full/build-ffmpeg.py
```

The command refuses an existing output and writes the private copy to
`build-et/aac-full/ffmpeg-no-fdiv/source/`.  It copies the complete pinned
`r6` configured source/build tree, changes only the copied
`ffbuild/config.mak` by appending `CFLAGS += -mno-fdiv`, and force-rebuilds
only `libavcodec/libavcodec.a` and `libavutil/libavutil.a`.

Handoff proof is in that output: `INPUT_ARCHIVE_SHA256SUMS.txt`,
`PRIVATE_CONFIG.mak`, `logs/build-commands.log`,
`audit/SOURCE_CONFIGURATION_DIFF.txt`,
`audit/FP_DIV_SQRT_AUDIT.txt`, and
`audit/UNRESOLVED_COMPILER_HELPERS.txt`.  The instruction audit requires no
`fdiv.s`/`fsqrt.s` in either archive or the selected `ff_aac_decoder` closure;
`__divsf3` is intentionally unresolved in these static archives until the
real compiler-rt final link.

This script does not access hardware, a simulator, or the network, and never
edits the retained `build-et/aac-decoder/...-r6` input.  A parent integration
may point its private FFmpeg source path at this output; this helper does not
edit that integration.
