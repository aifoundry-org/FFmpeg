#!/usr/bin/env python3
"""Generate the isolated AAC runtime from the pinned video runtime source.

This intentionally refuses source drift.  The resulting translation unit keeps
all generic lifetime/DMA/ELF code byte-for-byte from the pinned source except
for three checked substitutions: its private include, its inline params type,
and its typed launch routine.
"""
import argparse
import hashlib
from pathlib import Path
import sys

PINNED_SHA256 = "cb4705b3567730397f08c29cff74cca2d3c117eb4d5667344aa9dfe2197d8500"

LAUNCH = r'''extern "C" int etaac_rt_launch(ETRuntime *r, const ETAACParams *params, uint64_t shire_mask)
{
    if (!r) return -EINVAL;
    if (r->poisoned) return r->fail(EIO, "launch", "runtime is unusable after SDK failure");
    if (!r->have_kernel) return r->fail(EINVAL, "launch", "probe runtime has no loaded code");
    if (!params || !etaac_params_valid(params) ||
        shire_mask != UINT64_C(1) || !(r->shire_mask & UINT64_C(1)))
        return r->fail(EINVAL, "launch", "invalid AAC ABI, buffers, or mask: select available physical compute shire 0");
    if (!r->contains(params->input_addr, params->input_bytes) ||
        !r->contains(params->state_addr, params->state_bytes) ||
        !r->contains(params->output_addr, params->output_bytes) ||
        !r->contains(params->scratch_addr, params->scratch_bytes) ||
        !r->contains(params->status_addr, params->status_bytes))
        return r->fail(EINVAL, "launch", "AAC device buffers must be complete owned allocations");
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
'''


def replace_once(text: str, old: str, new: str, name: str) -> str:
    count = text.count(old)
    if count != 1:
        raise ValueError(f"{name}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    source = args.source.read_bytes()
    sha = hashlib.sha256(source).hexdigest()
    if sha != PINNED_SHA256:
        raise ValueError(
            f"refusing runtime source drift: expected {PINNED_SHA256}, got {sha}"
        )
    text = source.decode("utf-8")
    text = replace_once(text, '#include "et_runtime.h"',
                        '#include "etaac_runtime_private.h"', "private include")
    text = replace_once(text, 'alignas(64) ETFrameParams launch_params{};',
                        'alignas(64) ETAACParams launch_params{};', "launch params type")
    launch_start = 'extern "C" int ff_et_runtime_launch(ETRuntime *r, const ETFrameParams *params, uint64_t shire_mask)\n'
    start_count = text.count(launch_start)
    if start_count != 1:
        raise ValueError(f"video launch: expected exactly one signature, found {start_count}")
    start = text.index(launch_start)
    # The pinned launch is the final declaration.  Checking this makes the
    # replacement unambiguous rather than silently truncating an edited source.
    old_launch = text[start:]
    if not old_launch.endswith("}\n") or old_launch.count("extern \"C\" int ") != 1:
        raise ValueError("video launch: pinned routine is no longer the unique final declaration")
    text = text[:start] + LAUNCH
    if 'ETFrameParams' in text or 'ff_et_runtime_launch' in text:
        raise ValueError("postcondition failed: video launch type/symbol remains")
    if text.count('etaac_rt_launch') != 1:
        raise ValueError("postcondition failed: expected one AAC launch symbol")
    for symbol in ('ff_et_runtime_open', 'ff_et_runtime_close', 'ff_et_runtime_alloc',
                   'ff_et_runtime_free', 'ff_et_runtime_write', 'ff_et_runtime_read'):
        if text.count(f'extern "C" int {symbol}') + text.count(f'extern "C" void {symbol}') != 1:
            raise ValueError(f"postcondition failed: expected one preserved {symbol}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"generate_runtime.py: {exc}", file=sys.stderr)
        sys.exit(1)
