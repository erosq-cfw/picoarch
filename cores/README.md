# Cores (built out-of-tree, not committed)

`*.so` is gitignored repo-wide (see the repo-root `.gitignore`), so
the built core binaries in this directory are **not** tracked in git. This
file documents how to reproduce them. `$EROSQ_ROOT` is the directory that
holds the sibling checkouts (gbemu/, buildroot-*/); `$PICOARCH` is this repo.

## gpsp (GBA) — `gpsp_libretro.so`

Pre-built, not compiled as part of this task. Source: notaz's gpsp libretro
fork (MIPS dynarec) at `$EROSQ_ROOT/gbemu/gpsp` (its own git clone,
remote-tracked independently of picoarch/gbemu).

```
cp "$EROSQ_ROOT"/gbemu/gpsp/gpsp_libretro.so cores/
```

Verified MIPS32 ELF shared object (`static-pie linked`):

```
file gpsp_libretro.so
# gpsp_libretro.so: ELF 32-bit LSB shared object, MIPS, MIPS32 version 1 (SYSV), static-pie linked, not stripped
```

`retro_api_version()` disassembles to `li v0,1; jr ra` (confirmed with
`mipsel-linux-objdump -d`) — API version **1**, matches picoarch's expected
(unenforced) v1 ABI.

Note: this core is `static-pie` and leaves a few libgcc soft-float helpers
(e.g. `__fixdfdi`) as unresolved dynamic symbols, expecting the *host
process* (picoarch itself, once it links libgcc) to provide them — a bare
`dlopen()` harness that never touches `double`/`int64_t` conversions itself
will fail to relocate it for that reason. This is expected/by-design for this
core's build, not a defect; full functional dlopen verification happens in
Task 3 once `plat_erosq`/picoarch's own binary exists to supply those
symbols.

## gambatte (GB/GBC) — `gambatte_libretro.so`

Built from source at `$EROSQ_ROOT/gbemu/gambatte` (own git clone of
`https://github.com/libretro/gambatte-libretro`, HEAD `dfc1655`), cross-compiled
for mipsel-musl with the buildroot toolchain.

```bash
cd "$EROSQ_ROOT"/gbemu/gambatte
PATH="$EROSQ_ROOT"/buildroot-2026.05/output/host/bin:$PATH \
  make -f Makefile.libretro platform=unix \
       CC=mipsel-linux-gcc CXX=mipsel-linux-g++ -j4
cp gambatte_libretro.so "$PICOARCH"/cores/
```

Toolchain triple: `mipsel-linux-gcc`/`mipsel-linux-g++` are symlinks
(`toolchain-wrapper`) for `mipsel-buildroot-linux-musl-gcc`/`-g++`
(confirm with `mipsel-linux-gcc -dumpmachine` -> `mipsel-buildroot-linux-musl`).

**Do not pass `CFLAGS=...` on the make command line.** `Makefile.libretro`
builds up its own `CFLAGS`/`CXXFLAGS`/`DEFINES` (include paths, `-D__LIBRETRO__`,
etc.) via `+=`. A command-line `CFLAGS=` assignment has "command line" origin
in GNU Make and silently defeats every later `+=` from the makefile, which
breaks the `.c` file compile rules (they lose all `-I` and `-D` flags while
`.cpp` files, which get their flags from a separately-accumulated `CXXFLAGS`,
still build) — this produced a `fatal error: string/stdstring.h: No such
file or directory` on the first attempt. Fix: don't touch `CFLAGS` at all.

**No `-DVIDEO_RGB565` needed.** `Makefile.libretro` already defaults
`VIDEO_RGB565 = 1` at the top of the file (line 3) for every non-PS2
platform; RGB565 is gambatte-libretro's default pixel format, not an opt-in.

Verified MIPS32r2 ELF shared object, dynamically linked against
`libstdc++.so.6`/`libgcc_s.so.1`/`libc.so` (all present under
`buildroot-2026.05/output/target/usr/lib`, so no extra runtime deps needed
on-device):

```
file gambatte_libretro.so
# gambatte_libretro.so: ELF 32-bit LSB shared object, MIPS, MIPS32 rel2 version 1 (SYSV), dynamically linked, not stripped
```

`retro_api_version()` confirmed live under `qemu-mipsel-static` (dlopen +
dlsym harness against the buildroot musl sysroot): returns **1**.
