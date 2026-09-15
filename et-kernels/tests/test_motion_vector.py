#!/usr/bin/env python3
"""Native unit model of the *expanded inline-asm strings*, not an ET launch.

Checks mask restoration, inactive-lane isolation, exact byte access sets,
natural halfword alignment, and all eight interpolation/blending variants.
This is NOT a replacement for a silicon/official-sysemu correctness gate.
Only the host C preprocessor is invoked. See MOTION.md for ISA sources.
"""
import ast
from collections import Counter
import os
from pathlib import Path
import random
import re
import subprocess

HEADER = Path(__file__).resolve().parents[1] / "src/motion.h"
def preprocess(impl, store):
    return subprocess.check_output([
        os.environ.get("CC", "cc"), "-E", "-P", "-x", "c",
        "-DET_DEVICE=1", "-D__riscv=1", f"-DET_MC_IMPL={impl}",
        f"-DET_MC_V8_STORE={store}", str(HEADER)
    ], text=True)


def program(text, name):
    match = re.search(r"void " + name + r"\(.*?__asm__ volatile\((.*?) : \[s\]", text, re.S)
    assert match, name
    instructions = "".join(ast.literal_eval(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"', match[1]))
    lines = [line.strip() for line in instructions.splitlines() if line.strip()]
    labels = {line[:-1]: i for i, line in enumerate(lines) if line.endswith(":" )}
    return lines, labels


def execute(code, impl, store, n, hx, hy, avg, alignment, stride, rng, initial_mask):
    lines, labels = code
    source, dest, offsets = 0x1000 + alignment, 0x100000+8*rng.randrange(8), 0x200000
    s = {source + y*stride+x: rng.randrange(256)
         for y in range(n+hy) for x in range(n+hx)}
    d = {dest + y*stride+x: rng.randrange(256) for y in range(n) for x in range(n)}
    # Include saturation, sign-extension and nested-rounding counterexamples.
    if rng.randrange(4) == 0:
        for i, pos in enumerate(s):
            s[pos] = (0, 255, 127, 128, 1, 2, 3, 254)[i % 8]
    expected = d.copy()
    for y in range(n):
        for x in range(n):
            values = [s[source+(y+j)*stride+x+i] for j in range(hy+1) for i in range(hx+1)]
            pred = (sum(values) + len(values)//2)//len(values)
            pos = dest+y*stride+x
            expected[pos] = (d[pos]+pred+1)//2 if avg else pred
    r = {"%[s]": source, "%[d]": dest, "%[stride]": stride,
         "%[size]": n, "%[rows]": n, "%[step]": stride-n+8,
         "%[offsets]": offsets, "%[cols]": n,
         "%[colstep]": 8-(n-1)*stride, "%[badstride]": 32 if stride&31 else 0,
         "%[limit]": 25-hx, "%[gather]": sum(i << (5*i) for i in range(8)),
         "%[wconf]": 8, "zero": 0}
    mask = initial_mask
    f = {"f"+str(i): [0xDEADBEEF]*8 for i in range(7)}
    read_source, read_dest, writes = set(), set(), set()

    def read_byte(addr):
        if addr in s:
            read_source.add(addr)
            return s[addr]
        assert avg and addr in d, ("read outside proven rectangle", hex(addr))
        read_dest.add(addr)
        return d[addr]

    def write_value(addr, value, width):
        assert not (addr & (width-1)), "unaligned store"
        for byte in range(width):
            assert addr+byte in d, ("store outside proven rectangle", hex(addr+byte))
            assert addr+byte not in writes, "repeated destination write"
            writes.add(addr+byte)
            d[addr+byte] = (value >> (8*byte))&255

    pc = steps = instructions = packed_stores = mask_writes = 0
    fast_source = general_source = window_tests = 0
    while pc < len(lines):
        steps += 1
        assert steps < 20000, "nonterminating loop"
        line = lines[pc]
        pc += 1
        if line.endswith(":"):
            continue
        instructions += 1
        op, args = line.split(None, 1)
        args = [a.strip() for a in args.split(",")]
        lanes = [i for i in range(8) if (mask >> i)&1]
        if op == "mova.x.m":
            r[args[0]] = mask
        elif op == "mova.m.x":
            mask = r[args[0]]
        elif op == "mov.m.x":
            mask_writes += 1
            assert args[0] == "m0"
            mask = (mask & ~255) | ((r[args[1]] | int(args[2])) & 255)
        elif op in ("addi", "add", "mv"):
            r[args[0]] = r[args[1]] + (int(args[2]) if op == "addi" else r[args[2]] if op == "add" else 0)
        elif op == "andi":
            r[args[0]] = r[args[1]] & int(args[2])
        elif op == "or":
            r[args[0]] = r[args[1]] | r[args[2]]
        elif op == "sltiu":
            r[args[0]] = int(r[args[1]] < r[args[2]])
            window_tests += 1
        elif op in ("j", "beqz"):
            if op == "j" or r[args[0]] == 0:
                pc = labels[args[-1][:-1]]
        elif op == "flw.ps":
            address = re.fullmatch(r"0\((.*)\)", args[1])[1]
            assert r[address] == offsets
            assert lanes == (list(range(4)) if impl == 2 else list(range(8)))
            for lane in lanes:
                f[args[0]][lane] = lane*2 if impl == 2 else lane
        elif op in ("fgb.ps", "fsch.ps", "fscb.ps"):
            index, address = re.fullmatch(r"(f\d+)\((.*)\)", args[1]).groups()
            assert lanes == (list(range(4)) if impl == 2 else list(range(8))), "wrong gather/scatter mask"
            if op == "fgb.ps" and r[address] in s:
                general_source += 1
            for lane in lanes:
                addr = r[address] + f[index][lane]
                if op == "fgb.ps":
                    val = read_byte(addr)
                    f[args[0]][lane] = (val if val < 128 else val-256) & 0xFFFFFFFF
                else:
                    write_value(addr, f[args[0]][lane], 2 if op == "fsch.ps" else 1)
        elif op in ("fg32b.ps", "fsc32w.ps"):
            assert impl == 4
            conf, address = re.fullmatch(r"(.*)\((.*)\)", args[1]).groups()
            base = r[address]
            if op == "fg32b.ps":
                assert lanes == list(range(8))
                assert (base&31) <= 24, "FG32 read would wrap outside the eight-byte span"
                if base in s:
                    fast_source += 1
                for lane in lanes:
                    addr = (base & ~31) + ((base + (r[conf] >> (5*lane))) & 31)
                    assert addr == base+lane, "unexpected FG32 configuration"
                    val = read_byte(addr)
                    f[args[0]][lane] = (val if val < 128 else val-256) & 0xFFFFFFFF
            else:
                assert lanes == [0, 1] and not (base&7), "packed FSC32 store mask/alignment"
                for lane in lanes:
                    addr = (base & ~31) + ((base + ((r[conf] >> (3*lane)) << 2)) & 28)
                    assert addr == base+4*lane, "FSC32 wrap or wrong word-offset configuration"
                    write_value(addr, f[args[0]][lane], 4)
                packed_stores += 1
        elif op == "fsw.ps":
            assert impl == 3 and store == 0 and lanes == [0, 1], "packed store must enable EXACTLY two lanes"
            address = re.fullmatch(r"0\((.*)\)", args[1])[1]
            assert not (r[address]&7), "packed destination not eight-byte aligned"
            for lane in lanes:
                write_value(r[address]+4*lane, f[args[0]][lane], 4)
            packed_stores += 1
        elif op == "fpackrepb.pi":
            assert impl >= 3 and lanes == list(range(8))
            # Snapshot source: instruction permits in-place packing. Output
            # lane e packs source lanes (4*e+j)%8, j=0..3, low byte only.
            values = f[args[1]].copy()
            for lane in lanes:
                f[args[0]][lane] = sum((values[(4*lane+j)%8]&255) << (8*j) for j in range(4))
        elif op in ("fandi.pi", "faddi.pi", "fadd.pi", "fsrli.pi", "fslli.pi", "for.pi"):
            for lane in lanes:
                a = f[args[1]][lane]
                b = f[args[2]][lane] if op in ("fadd.pi", "for.pi") else int(args[2])
                value = (a & b if op == "fandi.pi" else
                         a+b if op in ("faddi.pi", "fadd.pi") else
                         a >> b if op == "fsrli.pi" else
                         a << b if op == "fslli.pi" else a | b)
                f[args[0]][lane] = value & 0xFFFFFFFF
        else:
            raise AssertionError(("unmodeled instruction", line))
    assert mask == initial_mask, "mask state not restored"
    if impl == 2:
        assert all(v[4:] == [0xDEADBEEF]*4 for v in f.values()), "inactive vector half clobbered"
    else:
        assert all(f[f"f{i}"] == [0xDEADBEEF]*8 for i in range(3, 7)), "undeclared FP clobber"
    expected_stores = n*n//8 if (impl == 3 and store == 0) or impl == 4 else 0
    assert packed_stores == expected_stores
    assert mask_writes == 1+2*expected_stores, "unexpected per-row/per-group mask operations"
    if impl == 4:
        assert window_tests == n//8, "window proof repeated inside row loop"
        fast_groups = n*sum(not (stride&31) and ((source+x)&31) <= 24-hx for x in range(0,n,8))
        assert fast_source == fast_groups*(1+hx)*(1+hy), "fast source path selection"
        assert general_source == (n*n//8-fast_groups)*(1+hx)*(1+hy), "general source fallback selection"
        body_end = next(i for i, line in enumerate(lines) if line.startswith("addi %[rows]"))
        body_instructions = body_end-labels["2"]-1
        assert instructions == (175 if n == 16 else 48) + (n*n//8)*body_instructions
    else:
        assert window_tests == 0
        body_end = next(i for i, line in enumerate(lines) if line.startswith("addi %[cols]"))
        body_instructions = body_end-labels["2"]-1
        assert instructions == (209 if n == 16 else 65) + (n*n//8)*body_instructions
    assert d == expected, ("pixel mismatch", n, hx, hy, avg, alignment)
    assert read_source == s.keys(), "missing/extra reference reads"
    assert read_dest == (d.keys() if avg else set()), "put unexpectedly reads destination"
    assert writes == d.keys(), "missing destination write"


rng = random.Random(0x4D50454732)
names = ("put", "x", "y", "xy", "avg", "ax", "ay", "axy")
for impl, store in ((2, 0), (3, 0), (3, 1), (4, 0)):
    text = preprocess(impl, store)
    cases = 0
    body_counts = []
    for mode, suffix in enumerate(names):
        code = program(text, {2: "et_mc_v_", 3: "et_mc_w_", 4: "et_mc_r_"}[impl]+suffix)
        lines, labels = code
        end = next(i for i, line in enumerate(lines) if line.startswith("addi %[rows]" if impl == 4 else "addi %[cols]"))
        body = lines[labels["2"]+1:end]
        counts = Counter(line.split()[0] for line in body)
        body_counts.append((suffix, len(body), counts["fgb.ps"]+counts["fg32b.ps"], counts["mov.m.x"]))
        # A0 masked cache-bypassing vector stores are forbidden, irrespective
        # of arithmetic/model correctness. Only ordinary L1 stores qualify.
        assert not any(line.startswith(("fswl.ps", "fswg.ps")) for line in lines)
        for n in (8, 16):
            for alignment in (range(64) if impl == 4 else (*range(8), 24, 25, 28, 31, 56, 57, 60, 63)):
                for stride in ((24, 40, 64, 128) if impl == 4 else (64, 128)):
                    for m0 in (0, 1, 15, 240, 255):
                        for _ in range(2):
                            masks = (rng.getrandbits(56) << 8) | m0
                            execute(code, impl, store, n, mode&1, (mode>>1)&1, mode>>2,
                                    alignment, stride, rng, masks)
                            cases += 1
    print(f"PASS impl={impl} store={store}: {cases} expanded-asm native model cases; exact bytes/masks")
    print("  eight-pixel body (mode,instructions,gathers,mask-writes):", body_counts)
