/*
 * picoarch platform backend for the Iriver/AK "EROS Q"
 * (Ingenic X1000, mipsel musl, single core, no GPU, 320x240 SLCD).
 *
 * Written from scratch against picoarch's plat.h contract (there is no
 * non-SDL plat in upstream picoarch to fork). Links NO SDL: SDL2 on this
 * device is video=dummy and cannot render.
 *
 *   Video : raw-KMS 320x240 RGB565 dumb buffer (drm_out.c, proven on this
 *           exact GC9308 panel by doom/gbemu/pcsx). picoarch's scale.c scales
 *           the core frame into a 320x240 shadow "screen" (SCREEN_WIDTH/HEIGHT
 *           are 320x240 for the EROSQ build); flip blits it into the scanout
 *           and SETCRTCs.
 *   Input : libpicofe linux/in_evdev over /dev/input/event* (on-device
 *           gpio-keys + an optional BT gamepad's BTN_* codes).
 *   Audio : ALSA (plughw:0,0, soft-resampled to the core's rate). Blocking
 *           writes pace the emulator to real time, exactly the role SDL's
 *           buffer-full wait plays in plat_sdl.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <linux/input.h>
#include <alsa/asoundlib.h>

#include "libretro.h"
#include "libpicofe/input.h"
#include "libpicofe/linux/in_evdev.h"
#include "libpicofe/plat.h"
#include "libpicofe/menu.h"
#include "libpicofe/fonts.h"
#include "core.h"
#include "main.h"
#include "menu.h"
#include "options.h"
#include "scale.h"
#include "util.h"
#include "plat.h"
#include "drm_out.h"

#define PANEL_W DRM_OUT_W   /* 320 */
#define PANEL_H DRM_OUT_H   /* 240 */

/* --- input -------------------------------------------------------------- */
/* Only ~6 keys + a scroll wheel exist on the device (x1000-erosq gpio-keys),
 * so the physical map is minimal-but-playable; a paired BT gamepad's BTN_*
 * codes reach a full pad + the emu menu. Player binds are libretro joypad
 * ids (core.c: buttons >> RETRO_DEVICE_ID_JOYPAD_*). Mirrors pcsx's proven
 * erosq evdev map, retargeted to picoarch's action constants. */
static const struct in_default_bind in_evdev_defbinds[] = {
	/* on-device gpio-keys */
	{ KEY_VOLUMEUP,     IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_UP },
	{ KEY_VOLUMEDOWN,   IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_DOWN },
	{ KEY_PREVIOUSSONG, IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_LEFT },
	{ KEY_NEXTSONG,     IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_RIGHT },
	{ KEY_PLAYPAUSE,    IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_B },
	{ KEY_BACK,         IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_A },
	{ KEY_MENU,         IN_BINDTYPE_EMU,      EACTION_MENU },
	/* BT gamepad: face + shoulders + start/select + a HID d-pad */
	{ BTN_DPAD_UP,      IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_UP },
	{ BTN_DPAD_DOWN,    IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_DOWN },
	{ BTN_DPAD_LEFT,    IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_LEFT },
	{ BTN_DPAD_RIGHT,   IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_RIGHT },
	{ BTN_A,            IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_B },
	{ BTN_B,            IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_A },
	{ BTN_X,            IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_Y },
	{ BTN_Y,            IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_X },
	{ BTN_TL,           IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_L },
	{ BTN_TR,           IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_R },
	{ BTN_TL2,          IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_L2 },
	{ BTN_TR2,          IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_R2 },
	{ BTN_SELECT,       IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_SELECT },
	{ BTN_START,        IN_BINDTYPE_PLAYER12, RETRO_DEVICE_ID_JOYPAD_START },
	{ BTN_MODE,         IN_BINDTYPE_EMU,      EACTION_MENU },
	{ 0, 0, 0 }
};

/* menu navigation (PBTN_*) — mirrors the pad so the frontend menu is usable
 * on-device without a keyboard. */
static const struct menu_keymap key_pbtn_map[] = {
	{ KEY_VOLUMEUP,     PBTN_UP },
	{ KEY_VOLUMEDOWN,   PBTN_DOWN },
	{ KEY_PREVIOUSSONG, PBTN_LEFT },
	{ KEY_NEXTSONG,     PBTN_RIGHT },
	{ KEY_PLAYPAUSE,    PBTN_MOK },
	{ KEY_BACK,         PBTN_MBACK },
	{ KEY_MENU,         PBTN_MENU },
	/* BT gamepad */
	{ BTN_DPAD_UP,      PBTN_UP },
	{ BTN_DPAD_DOWN,    PBTN_DOWN },
	{ BTN_DPAD_LEFT,    PBTN_LEFT },
	{ BTN_DPAD_RIGHT,   PBTN_RIGHT },
	{ BTN_A,            PBTN_MOK },
	{ BTN_B,            PBTN_MBACK },
	{ BTN_MODE,         PBTN_MENU },
};

static const struct in_pdata erosq_evdev_pdata = {
	.defbinds  = in_evdev_defbinds,
	.key_map   = key_pbtn_map,
	.kmap_size = array_size(key_pbtn_map),
};

/* --- video -------------------------------------------------------------- */
static uint16_t *screen;      /* SCREEN_WIDTH x SCREEN_HEIGHT RGB565 shadow */
static uint16_t *drm_buf;     /* mapped 320x240 RGB565 scanout */
static int drm_stride;        /* scanout row stride in pixels */

static char msg[HUD_LEN];
static unsigned msg_priority = 0;
static unsigned msg_expire = 0;

static void video_expire_msg(void)
{
	msg[0] = '\0';
	msg_priority = 0;
	msg_expire = 0;
}

static void video_update_msg(void)
{
	if (msg[0] && msg_expire < plat_get_ticks_ms())
		video_expire_msg();
}

/* push the shadow "screen" into the (possibly padded) scanout and modeset */
static void present_screen(void)
{
	int y;
	if (drm_stride == SCREEN_WIDTH) {
		memcpy(drm_buf, screen, (size_t)SCREEN_WIDTH * SCREEN_HEIGHT * 2);
	} else {
		for (y = 0; y < SCREEN_HEIGHT; y++)
			memcpy(drm_buf + (size_t)y * drm_stride,
			       screen + (size_t)y * SCREEN_WIDTH,
			       SCREEN_WIDTH * 2);
	}
	drm_present();
}

void plat_video_open(void)
{
}

void plat_video_set_msg(const char *new_msg, unsigned priority, unsigned msec)
{
	if (!new_msg) {
		video_expire_msg();
	} else if (priority >= msg_priority) {
		snprintf(msg, HUD_LEN, "%s", new_msg);
		msg_priority = priority;
		msg_expire = plat_get_ticks_ms() + msec;
	}
}

void plat_video_process(const void *data, unsigned width, unsigned height, size_t pitch)
{
	scale(width, height, pitch, data, screen);

	if (msg[0])
		basic_text_out16_nf(screen, SCREEN_WIDTH, 2, SCREEN_HEIGHT - 10, msg);

	video_update_msg();
}

void plat_video_flip(void)
{
	present_screen();
}

void plat_video_close(void)
{
}

/* --- menu --------------------------------------------------------------- */
/* g_menuscreen_ptr is the drawable menu surface; we point it at the shadow
 * "screen" (320x240 RGB565 == panel). menu_enter snapshots the live game
 * frame as the (darkened) menu background; menu_end presents. */
void plat_video_menu_enter(int is_rom_loaded)
{
	(void)is_rom_loaded;
	memcpy(g_menubg_src_ptr, screen,
	       (size_t)g_menubg_src_h * g_menubg_src_pp * sizeof(uint16_t));
	g_menuscreen_ptr = screen;
}

void plat_video_menu_begin(void)
{
	g_menuscreen_ptr = screen;
	menu_begin();
}

void plat_video_menu_end(void)
{
	menu_end();
	present_screen();
}

void plat_video_menu_leave(void)
{
	memset(g_menubg_src_ptr, 0,
	       (size_t)g_menuscreen_h * g_menuscreen_pp * sizeof(uint16_t));
	memset(screen, 0, (size_t)SCREEN_WIDTH * SCREEN_HEIGHT * 2);
	present_screen();
	g_menuscreen_ptr = NULL;
}

/* --- screenshots (state thumbnails + EACTION_SCREENSHOT) ---------------- */
/* Self-contained 24-bit BMP (bottom-up, BGR). No libpng/SDL. plat_load_screen
 * only ever reads what plat_dump_screen wrote. */
void *plat_prepare_screenshot(int *w, int *h, int *bpp)
{
	if (w) *w = SCREEN_WIDTH;
	if (h) *h = SCREEN_HEIGHT;
	if (bpp) *bpp = 16;
	return screen;
}

static void put_le32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void put_le16(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }

int plat_dump_screen(const char *filename)
{
	char name[MAX_PATH];
	FILE *f;
	uint8_t hdr[54] = {0};
	int row_bytes = SCREEN_WIDTH * 3;
	int pad = (4 - (row_bytes & 3)) & 3;
	int stride = row_bytes + pad;
	uint32_t img = (uint32_t)stride * SCREEN_HEIGHT;
	uint8_t *line;
	int x, y;

	snprintf(name, sizeof(name), "%s.bmp", filename);
	f = fopen(name, "wb");
	if (!f)
		return -1;

	hdr[0]='B'; hdr[1]='M';
	put_le32(hdr+2, 54 + img);
	put_le32(hdr+10, 54);
	put_le32(hdr+14, 40);
	put_le32(hdr+18, SCREEN_WIDTH);
	put_le32(hdr+22, SCREEN_HEIGHT);
	put_le16(hdr+26, 1);
	put_le16(hdr+28, 24);
	put_le32(hdr+34, img);
	fwrite(hdr, 1, 54, f);

	line = calloc(1, stride);
	if (!line) { fclose(f); return -1; }
	for (y = SCREEN_HEIGHT - 1; y >= 0; y--) {
		const uint16_t *src = screen + (size_t)y * SCREEN_WIDTH;
		uint8_t *d = line;
		for (x = 0; x < SCREEN_WIDTH; x++) {
			uint16_t p = src[x];
			*d++ = (uint8_t)(( p        & 0x1f) << 3); /* B */
			*d++ = (uint8_t)(((p >> 5)  & 0x3f) << 2); /* G */
			*d++ = (uint8_t)(((p >> 11) & 0x1f) << 3); /* R */
		}
		fwrite(line, 1, stride, f);
	}
	free(line);
	fclose(f);
	return 0;
}

int plat_load_screen(const char *filename, void *buf, size_t buf_size, int *w, int *h, int *bpp)
{
	char name[MAX_PATH];
	FILE *f;
	uint8_t hdr[54];
	int iw, ih, row_bytes, pad, stride, x, y;
	uint16_t *out = buf;
	uint8_t *line;

	snprintf(name, sizeof(name), "%s.bmp", filename);
	f = fopen(name, "rb");
	if (!f)
		return -1;
	if (fread(hdr, 1, 54, f) != 54 || hdr[0] != 'B' || hdr[1] != 'M') {
		fclose(f);
		return -1;
	}
	iw = hdr[18] | hdr[19]<<8 | hdr[20]<<16 | hdr[21]<<24;
	ih = hdr[22] | hdr[23]<<8 | hdr[24]<<16 | hdr[25]<<24;
	if (iw <= 0 || ih <= 0 || (hdr[28] | hdr[29]<<8) != 24 ||
	    (size_t)iw * ih * 2 > buf_size) {
		fclose(f);
		return -1;
	}
	row_bytes = iw * 3;
	pad = (4 - (row_bytes & 3)) & 3;
	stride = row_bytes + pad;
	line = malloc(stride);
	if (!line) { fclose(f); return -1; }

	for (y = ih - 1; y >= 0; y--) {  /* file is bottom-up */
		uint16_t *drow = out + (size_t)y * iw;
		if (fread(line, 1, stride, f) != (size_t)stride) {
			free(line); fclose(f); return -1;
		}
		for (x = 0; x < iw; x++) {
			uint8_t b = line[x*3+0], g = line[x*3+1], r = line[x*3+2];
			drow[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
		}
	}
	free(line);
	fclose(f);
	*w = iw;
	*h = ih;
	*bpp = 2;
	return 0;
}

static uint16_t *clean_buf;
void *plat_clean_screen(void)
{
	/* LCD-simulation "clean" pass for state thumbnails (scale.c). Returns a
	 * de-filtered copy if a *_lcd scaler is active, else NULL. */
	if (scale_clean(screen, clean_buf))
		return clean_buf;
	return NULL;
}

/* --- misc --------------------------------------------------------------- */
unsigned plat_cpu_ticks(void)
{
	long unsigned ticks = 0;
	long ticksps = 0;
	FILE *file = fopen("/proc/self/stat", "r");
	if (!file)
		return 0;
	if (fscanf(file, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu", &ticks)) {
		ticksps = sysconf(_SC_CLK_TCK);
		if (ticksps)
			ticks = ticks * 100 / ticksps;
	}
	fclose(file);
	return ticks;
}

/* --- audio (ALSA) ------------------------------------------------------- */
static snd_pcm_t *pcm;
static int pcm_rate;
static snd_pcm_uframes_t pcm_buf_frames;

static void audio_close(void)
{
	if (pcm) {
		snd_pcm_drain(pcm);
		snd_pcm_close(pcm);
		pcm = NULL;
	}
	pcm_rate = 0;
}

static int audio_open(int rate)
{
	const char *dev;
	snd_pcm_uframes_t period = 0;
	int ret;

	if (rate <= 0)
		return -1;
	if (pcm && rate == pcm_rate)
		return 0;
	audio_close();

	dev = getenv("EROSQ_ALSA_DEV");
	if (!dev)
		dev = "plughw:0,0";   /* plug wrapper soft-resamples core rate -> DAC */

	ret = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
	if (ret < 0) {
		fprintf(stderr, "plat_erosq: snd_pcm_open(%s) failed: %s\n",
			dev, snd_strerror(ret));
		pcm = NULL;
		return -1;
	}
	/* S16 interleaved stereo, soft-resample on, ~100ms latency */
	ret = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
				 SND_PCM_ACCESS_RW_INTERLEAVED, 2,
				 (unsigned)rate, 1, 100000);
	if (ret < 0) {
		fprintf(stderr, "plat_erosq: snd_pcm_set_params failed: %s\n",
			snd_strerror(ret));
		snd_pcm_close(pcm);
		pcm = NULL;
		return -1;
	}
	snd_pcm_get_params(pcm, &pcm_buf_frames, &period);
	if (pcm_buf_frames == 0)
		pcm_buf_frames = 1;
	pcm_rate = rate;
	return 0;
}

float plat_sound_capacity(void)
{
	snd_pcm_sframes_t avail;
	if (!pcm)
		return 1.0f;
	avail = snd_pcm_avail_update(pcm);
	if (avail < 0)
		return 1.0f;
	if ((snd_pcm_uframes_t)avail > pcm_buf_frames)
		avail = pcm_buf_frames;
	return (float)avail / (float)pcm_buf_frames;
}

void plat_sound_write(const struct audio_frame *data, int frames)
{
	const char *p = (const char *)data;

	if (audio_open((int)sample_rate) != 0)
		return;

	/* fast-forward (limit_frames off): don't block on a full buffer, drop */
	if (!limit_frames) {
		snd_pcm_sframes_t avail = snd_pcm_avail_update(pcm);
		if (avail >= 0 && avail < frames)
			return;
	}

	while (frames > 0) {
		snd_pcm_sframes_t w = snd_pcm_writei(pcm, p, frames);
		if (w < 0) {
			w = snd_pcm_recover(pcm, w, 1);
			if (w < 0)
				return;
			continue;
		}
		p += w * (snd_pcm_sframes_t)sizeof(struct audio_frame);
		frames -= w;
	}
}

void plat_sound_resize_buffer(void)
{
	audio_open((int)sample_rate);
}

/* --- lifecycle ---------------------------------------------------------- */
int plat_init(void)
{
	screen    = calloc(1, (size_t)SCREEN_WIDTH * SCREEN_HEIGHT * 2);
	clean_buf = calloc(1, (size_t)SCREEN_WIDTH * SCREEN_HEIGHT * 2);
	if (!screen || !clean_buf) {
		PA_ERROR("plat_init: OOM\n");
		return -1;
	}

	/* must be set before menu_init() allocates the bg buffers (main.c calls
	 * plat_init then menu_init). */
	g_menuscreen_w  = PANEL_W;
	g_menuscreen_h  = PANEL_H;
	g_menuscreen_pp = PANEL_W;
	g_menubg_src_w  = PANEL_W;
	g_menubg_src_h  = PANEL_H;
	g_menubg_src_pp = PANEL_W;
	g_menuscreen_ptr = NULL;

	if (drm_open() != 0) {
		PA_ERROR("plat_init: drm_open failed (another DRM master running? stop erosq-ui)\n");
		return -1;
	}
	drm_buf = drm_pixels(&drm_stride);

	if (in_evdev_init(&erosq_evdev_pdata)) {
		PA_ERROR("plat_init: in_evdev_init failed\n");
		return -1;
	}
	in_probe();

	/* audio is opened lazily once the core reports its sample rate */
	return 0;
}

int plat_reinit(void)
{
	audio_open((int)sample_rate);
	scale_update_scaler();
	return 0;
}

void plat_finish(void)
{
	audio_close();
	drm_close();
	free(screen);    screen = NULL;
	free(clean_buf); clean_buf = NULL;
}

void plat_minimize(void)
{
}
