# picoarch interfaces for an erosq port (`plat_erosq`)

Discovery spike for pcsx_rearmed-style in-game-menu work on the EROS Q
(Ingenic X1000, 32MB RAM, musl, no GPU, 320x240 SLCD, raw-DRM only — SDL2 on
this device is `video=dummy` and cannot render).

Repo: `picoarch`, upstream `https://github.com/shauninman/picoarch`.
**Pinned commit: `802047c276a5a931b0bf837c4ea4b8e238bdeabe`** (2022-03-16 00:00:04 -0400).
Submodules (recorded in this commit's `.gitmodules` / gitlinks, fetched via
`git submodule update --init --recursive`):
- `libpicofe` -> `https://github.com/notaz/libpicofe` @ `76e45c3489a0c32d91744413c198f685b553f6c0` (2021-01-18)
- `libretro-common` -> `https://github.com/libretro/libretro-common` @ `88252880232fb8488296dc0cd4c57d6fb8159b3d`

## 0. Headline finding: there is no non-SDL plat in upstream picoarch

All three shipped platform backends are thin per-device keymap headers that
`#include` the same file:

- `plat_linux.c:73` → `#include "plat_sdl.c"`
- `plat_miyoomini.c:116` → `#include "plat_sdl.c"`
- `plat_trimui.c:112` → `#include "plat_sdl.c"`

`plat_sdl.c` implements every `plat_*` symbol using real SDL 1.2 calls:
`SDL_Init(SDL_INIT_VIDEO)` (`plat_sdl.c:733`), `SDL_SetVideoMode(...,
SDL_SWSURFACE)` (`plat_sdl.c:734`), `SDL_Flip(screen)` inside `fb_flip()`
(`plat_sdl.c:389`), `SDL_LockSurface`/`SDL_UnlockSurface` for pixel access,
`SDL_OpenAudio`/`SDL_PauseAudio` for sound (`plat_sdl.c:621-646`), and
`in_sdl_init()` (`libpicofe/in_sdl.c`) for input. There is **no existing
plat_dummy-style raw-framebuffer/DRM backend to copy** the way pcsx_rearmed's
`frontend/plat_dummy.c` + `frontend/drm_out.c` exist for pcsx.

Consequence for later tasks: `plat_erosq.c` for picoarch must be written from
scratch against the `plat.h` contract below (there is no picoarch platform
file to fork), but it can reuse verbatim, unmodified, the two pieces
`pcsx_rearmed/frontend` already has working on this hardware:
- `frontend/drm_out.c` / `frontend/drm_out.h` — raw-KMS 320x240 RGB565 dumb
  buffer, no libdrm, `drm_open()/drm_pixels()/drm_present()/drm_close()`.
- `libpicofe/linux/in_evdev.c` (`in_evdev_init()`, prototyped at
  `libpicofe/linux/in_evdev.h:3`) — evdev input, already used the same way in
  `pcsx_rearmed/frontend/plat_erosq.c:334-335` (`in_evdev_init(&pdata);
  in_probe();`).

This is not a blocker (picoarch's plat abstraction is real and narrow — see
§1 — it's just that SDL happens to be the only implementation upstream
shipped), but it means Task 2 is "write an original plat_erosq.c", not "port
an existing one".

## 1. The `plat_*` contract a port must implement

Full interface declared in `picoarch/plat.h:1-36` (picoarch's
own, distinct from `libpicofe/plat.h`, which it `#include`s at line 4):

```c
// plat.h:13-35
int  plat_init(void);
int  plat_reinit(void);
void plat_finish(void);
void plat_minimize(void);

void *plat_prepare_screenshot(int *w, int *h, int *bpp);
int plat_dump_screen(const char *filename);
int plat_load_screen(const char *filename, void *buf, size_t buf_size, int *w, int *h, int *bpp);

void plat_video_open(void);
void plat_video_set_msg(const char *new_msg, unsigned priority, unsigned msec);
void plat_video_process(const void *data, unsigned width, unsigned height, size_t pitch);
void plat_video_flip(void);
void plat_video_close(void);

unsigned plat_cpu_ticks(void);

float plat_sound_capacity(void);
void plat_sound_write(const struct audio_frame *data, int frames);
void plat_sound_resize_buffer(void);

void* plat_clean_screen(void);
```

`struct audio_frame` (`plat.h:6-9`) is `{ int16_t left; int16_t right; }` —
i.e. sound is always pushed as interleaved S16 stereo pairs, sample rate is
whatever the core reports (`sample_rate` global, set from
`retro_get_system_av_info` in `core.c:672`); there is no resampling contract
in `plat.h` itself, `plat_sound_write` receives raw core-rate frames and the
plat backend must deal with output rate (plat_sdl.c's own
`audio_resample_nearest()` at `plat_sdl.c:368-385` shows the reference
approach: nearest-neighbour rate conversion done in the plat layer, not by
core.c).

Additionally, `libpicofe/plat.h` (included by `plat.h:4`) declares a second,
larger contract that picoarch also expects a plat backend to implement —
these are called directly by `menu.c`/`core.c`/`main.c` and are **not**
optional:

```c
// libpicofe/plat.h:96-125 (subset actually exercised by picoarch)
void plat_video_menu_enter(int is_rom_loaded);
void plat_video_menu_begin(void);
void plat_video_menu_end(void);
void plat_video_menu_leave(void);
unsigned int plat_get_ticks_ms(void);
```

`plat_video_menu_*` are called from `main.c:339,345,349` (MMENU path) and
from libpicofe's own `menu.c`/`emu_action` flow; the SDL reference
implementations are `plat_sdl.c:464-498`. They read/write four libpicofe
globals declared `extern` in `libpicofe/menu.h:115-124`:
`g_menuscreen_ptr`, `g_menubg_ptr`, `g_menubg_src_ptr`, `g_menuscreen_w`,
`g_menuscreen_h`, `g_menuscreen_pp`, `g_menubg_src_w`, `g_menubg_src_h`,
`g_menubg_src_pp` — a port must set `g_menuscreen_w/h/pp` in `plat_init()`
before `menu_init()` runs (exactly as `pcsx_rearmed/frontend/plat_erosq.c:319-321`
already does for its own, differently-shaped, plat contract).

Input is not part of `plat_*` at all: `core.c`'s `pa_input_poll()`
(`core.c:524-539`) calls libpicofe's own `in_update(actions)` /
`in_get_config` / `in_get_dev_def_binds` / `in_bind_key`
(`libpicofe/input.h:131,143,149`), so a port supplies input by calling
`in_evdev_init()` (or another libpicofe `in_*` driver) from inside
`plat_init()`, not by implementing a `plat_input_*` symbol — this matches
what `pcsx_rearmed/frontend/plat_erosq.c:334-335` already does.

## 2. How picoarch is told which core `.so` + ROM to run

Pure CLI, positional, no flags, no env var for the pair itself
(`main.c:530-566`):

```
picoarch <core.so-path> <content-path> [NONE|DMG|LCD|<anything-else=SCANLINE>]
```

- `argv[1]` = path to the libretro core `.so`, `dlopen()`'d directly
  (`core.c:581`, `RTLD_LAZY`); no core-name allowlist or magic suffix
  required beyond what `dlopen` resolves. `core_extract_name()`
  (`core.c:552-566`) strips a `_libretro.so` or `.so` suffix to get a short
  "core name" (e.g. `gambatte`) used only for config-dir naming and the
  `fmsx` special-case at `core.c:660`.
- `argv[2]` = content/ROM path, wrapped in `content_init()`
  (`content.c:307-315`, `main.c:576`) which stats for an `.m3u` sibling and
  is later opened via `content_load_game_info()` → `retro_load_game()`
  (`core.c:637-684`).
- `argv[3]` optional, one of the literal strings `NONE`/`DMG`/`LCD`/else
  (`main.c:561-566`) selecting `default_scale_effect` (an LCD-simulation
  filter, not core-related).
- `-h`/`--help` prints usage and exits 0 (`main.c:534-539`); anything else
  missing (`argc<=1` or `argc<=2`) calls `quit(-1)` — **both core and ROM
  args are mandatory**, there is no interactive/menu-driven picker at
  startup (the in-emu `select_content()` ROM browser in `menu.c:248` is only
  reachable after a core+ROM pair is already running, e.g. for M3U disc
  swaps).

Two environment variables are load-bearing and **not defensively checked for
NULL**:
- `SDCARD_PATH` — read unconditionally in `core.c:276` (`set_directories`)
  to build `save_dir`/`system_dir` (`core.c:289-290`, via `snprintf(...,
  "%s/Saves/%s/", sdcard_path, tag_name)`), and again in `main.c:513`
  (`get_tag_name`, via a raw `sprintf(roms_path, "%s/Roms/",
  getenv("SDCARD_PATH"))`, **no null check at all**). If `SDCARD_PATH` is
  unset, `main.c:513` passes a NULL pointer to `sprintf("%s")` — undefined
  behaviour, and on erosq's musl libc this is a real crash risk, not just a
  glibc "(null)" cosmetic. **Treat `SDCARD_PATH` as a hard-required env var
  for any erosq launch wrapper**, not an optional one.
- `HOME` — read in `core.c:275`, used only if non-NULL to build
  `~/.picoarch-<core>-<tag>/` as `config_dir` (`core.c:282-283`); safely
  guarded (`if (home != NULL)`), unlike `SDCARD_PATH`.
- `CONTENT_DIR` — read in `menu.c:257-258`, used only by the in-emu ROM
  browser (`select_content()`) as the starting directory when no content is
  already loaded; falls back to a compile-time `-DCONTENT_DIR=...` macro
  (set for `trimui`/`miyoomini` platforms in `Makefile:66,70`, not set for
  `unix`) or else `$HOME` (`menu.c:259-264`).

## 3. On-disk config/asset dir layout

Built by `set_directories()` (`core.c:274-302`), called from `core_open()`
(`core.c:588`) with `(core_name, tag_name)` — `tag_name` comes from
`get_tag_name()` (`main.c:508-528`), which extracts either a `(Pak Name)`
parenthesized tag from the ROM's path or the immediate parent-of-parent
folder name under `$SDCARD_PATH/Roms/`.

| Path | Built at | Purpose |
|---|---|---|
| `$HOME/.picoarch-<core_name>-<tag_name>/` | `core.c:283`, `mkdir(...,0755)` at `core.c:284` | `config_dir`: global `picoarch.cfg` (`main.c:51`), per-ROM save states `<rom>.st<slot>` (`core.c:118-123`), per-ROM cheats `cheats/<rom>.cht` (`core.c:652`) |
| `$SDCARD_PATH/Saves/<tag_name>/` | `core.c:289` | `save_dir`: per-ROM SRAM `<rom>.sav` (`core.c:69,98`), and — if a `<rom>.cfg` exists here — the **per-game config override** (`config_file_name()`, `main.c:46-53`, checked before falling back to the global `config_dir/picoarch.cfg`) |
| `$SDCARD_PATH/Bios/<tag_name>` | `core.c:290` | `system_dir`, exposed to cores via `RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY` (`core.c:357-362`) |

Also relevant: `RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY` (`core.c:418-423`)
returns the same `save_dir`. There is a build-time `PICO_HOME_DIR` macro
(`Makefile:14`, `-DPICO_HOME_DIR='"/.picoarch/"'`) that is **defined but
never referenced in any `.c` file** (`grep -rn PICO_HOME_DIR *.c` → no hits)
— dead config, not a real override point; don't rely on it.

Config file format is a flat `key = value\n` text file
(`config.c:36-58,99-127`), containing both hardcoded frontend settings
(`show_fps`, `show_cpu`, `limit_frames`, `audio_buffer_size`, `scale_size`,
`scale_effect`, `optimize_text` — table at `config.c:24-34`) and every
core option key/value pair (`config.c:53-57`, `config.c:114-126`).

## 4. libretro API version

`RETRO_API_VERSION` is `1`, defined in the vendored
`libretro-common/include/libretro.h:84`. **picoarch never actually calls
`retro_api_version()` at runtime to check it** — `struct core_cbs` in
`core.h:14` declares a `retro_api_version` function pointer field, but
`core_open()`'s `dlsym()` block (`core.c:591-608`) never resolves or calls
it; the field is dead/vestigial. Compatibility is implicit: picoarch is
built against libretro-common's v1 struct layouts (`retro_game_info`,
`retro_system_av_info`, the `retro_environment_t` callback signature, etc.)
and simply assumes any core it's pointed at matches those layouts — there is
no runtime ABI guard. A port targeting v1-only cores (which is effectively
all libretro cores, including `gambatte`/`gpsp`/`pcsx_rearmed`/`picodrive`
already in this Makefile's `CORES` list at `Makefile:25`) needs no extra
version-negotiation code beyond what `core_open()` already does.

## 5. picoarch's vendored libpicofe vs. pcsx_rearmed's copy: NOT the same tree

```
picoarch/libpicofe        -> notaz/libpicofe @ 76e45c3489a0c32d91744413c198f685b553f6c0 (2021-01-18)
pcsx_rearmed/frontend/libpicofe -> notaz/pcsx_rearmed fork-in-tree @ 9941dbacebb33db438f7eb6d04399ebda25af82a (2026-07-14, per superproject; not its own git repo)
```

`diff -rq` between the two trees shows real divergence, not just
whitespace — every shared file differs
(`config_file.c`, `gl.c/h`, `gl_platform.c/h`, `input.c/h`, `in_sdl.c`,
`linux/fbdev.c/h`, `linux/host_dasm.c`, `linux/in_evdev.c`, `linux/plat.c`,
`linux/sndout_alsa.c`, `menu.c/h`, `pandora/plat.c`, `plat.h`, `plat_sdl.c/h`,
`posix.h`, `readpng.c/h`, `sndout_sdl.c`) and pcsx's copy has files picoarch's
doesn't (`gl_loader.c/h`). picoarch additionally carries a local patch,
`patches/libpicofe/0001-key-combos.patch`, applied at build time
(`Makefile:119-123`, target `libpicofe/.patched`) that adds mod-key/combo
handling to `in_sdl.c` + a `struct mod_keymap` to `input.h` — **SDL-input
specific**, not applicable to the evdev path a port would use.

**Practical consequence:** treat picoarch's libpicofe and pcsx_rearmed's
libpicofe as two independently-drifted forks of the same upstream project,
not as "the same copy." Don't assume a function signature or global that
exists in one is present/identical in the other — verify per-symbol. The one
piece confirmed reusable **as a file, verbatim**, is
`libpicofe/linux/in_evdev.c` (`in_evdev_init()`,
`libpicofe/linux/in_evdev.h:3`), since both trees implement the same narrow
evdev-polling contract that `pcsx_rearmed/frontend/plat_erosq.c:334-335`
already exercises successfully on this exact hardware — but pull it from
whichever libpicofe copy `plat_erosq` for picoarch actually links against
(likely picoarch's own submodule, to avoid also inheriting pcsx's
GL/gl_loader additions it doesn't need), and diff it against
`pcsx_rearmed/frontend/libpicofe/linux/in_evdev.c` before assuming
byte-identical behaviour.

## 6. Reusable from pcsx_rearmed's existing erosq port (reference, not picoarch)

`pcsx_rearmed/frontend/plat_erosq.c` and `frontend/plat.h` are a **different,
smaller, pcsx-specific contract** (`plat_init/plat_finish/plat_minimize/
plat_prepare_screenshot/plat_gvideo_open/plat_gvideo_set_mode/
plat_gvideo_flip/plat_gvideo_close`, `frontend/plat.h:1-15`) — the function
*names* do not transfer to picoarch's `plat_video_open/process/flip/close`
naming, but the two on-device building blocks it wraps are directly reusable
by a picoarch port:
- `frontend/drm_out.c`/`frontend/drm_out.h` — `drm_open()`, `drm_pixels(int
  *stride_px)`, `drm_present()`, `drm_close()` (`drm_out.h:8-11`), a raw-KMS
  320x240 RGB565 dumb-buffer scanout with no libdrm dependency, already
  proven on this hardware (doom, gbemu per prior work). Wire it under
  picoarch's `plat_video_process/plat_video_flip` instead.
- `libpicofe/linux/in_evdev.c` usage pattern: `in_evdev_init(&pdata);
  in_probe();` inside `plat_init()` (`frontend/plat_erosq.c:334-335`), binds
  fall back through the same `in_default_bind`/`menu_keymap`
  (`libpicofe/input.h`) structures picoarch's own `plat_sdl.c` populates for
  SDL — i.e. picoarch's `plat.h`/`libpicofe/input.h` already expect exactly
  this kind of `in_pdata` table, a port just needs an evdev-flavoured one
  (gpio-keys + BT gamepad `BTN_*`) in place of picoarch's SDL keysym one.

## Summary for Tasks 2-4

- **No existing non-SDL picoarch plat to fork** — `plat_erosq.c` is new code
  against the 13-function `plat.h` contract (§1) plus the 5
  `libpicofe/plat.h` menu/tick functions it also needs, modeled on
  `pcsx_rearmed/frontend/plat_erosq.c`'s already-proven drm_out.c + in_evdev
  wiring, but with picoarch's different function names/semantics.
- **Launch contract**: `picoarch <core.so> <rom> [NONE|DMG|LCD|SCANLINE]`,
  with `SDCARD_PATH` env var **mandatory** (unchecked NULL deref otherwise)
  and `HOME` recommended (else no persistent config dir).
- **Config/asset dirs**: `$HOME/.picoarch-<core>-<tag>/` (config+states+cheats),
  `$SDCARD_PATH/Saves/<tag>/` (SRAM + optional per-game cfg override),
  `$SDCARD_PATH/Bios/<tag>` (BIOS/system dir).
- **libretro API**: v1, unenforced at runtime — no version-negotiation code
  needed for standard v1 cores (gambatte/gpsp already in the Makefile's core
  list).
- **libpicofe provenance**: picoarch's vendored copy and pcsx_rearmed's are
  different forks; verify every symbol independently, don't assume parity.
