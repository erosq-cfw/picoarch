/* Portable C integer scalers for the EROSQ build. scaler_neon.c is ARM NEON
 * inline asm and cannot build for MIPS; scale.c only references the 1x/2x/3x
 * 16bpp entry points, so provide plain-C equivalents with the same contract
 * (src/dst top-left, sw/sh pixels, sp/dp strides in bytes; 0 stride => packed).
 * Nearest-neighbour is what actually runs for the EROSQ panel's fit cases;
 * these keep the integer-scale fast paths link-complete and correct. */
#include <stdint.h>
#include <stddef.h>
#include "scaler_neon.h"

void scale1x_n16(void *__restrict src, void *__restrict dst,
		 uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dp)
{
	uint32_t y, x;
	if (!sp) sp = sw * 2;
	if (!dp) dp = sw * 2;
	for (y = 0; y < sh; y++) {
		const uint16_t *s = (const uint16_t *)((const uint8_t *)src + (size_t)y * sp);
		uint16_t *d = (uint16_t *)((uint8_t *)dst + (size_t)y * dp);
		for (x = 0; x < sw; x++)
			d[x] = s[x];
	}
}

void scale2x_n16(void *__restrict src, void *__restrict dst,
		 uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dp)
{
	uint32_t y, x;
	if (!sp) sp = sw * 2;
	if (!dp) dp = sw * 2 * 2;
	for (y = 0; y < sh; y++) {
		const uint16_t *s = (const uint16_t *)((const uint8_t *)src + (size_t)y * sp);
		uint16_t *d0 = (uint16_t *)((uint8_t *)dst + (size_t)(y * 2) * dp);
		uint16_t *d1 = (uint16_t *)((uint8_t *)dst + (size_t)(y * 2 + 1) * dp);
		for (x = 0; x < sw; x++) {
			uint16_t p = s[x];
			d0[x*2] = d0[x*2+1] = p;
			d1[x*2] = d1[x*2+1] = p;
		}
	}
}

void scale3x_n16(void *__restrict src, void *__restrict dst,
		 uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dp)
{
	uint32_t y, x, r;
	if (!sp) sp = sw * 2;
	if (!dp) dp = sw * 2 * 3;
	for (y = 0; y < sh; y++) {
		const uint16_t *s = (const uint16_t *)((const uint8_t *)src + (size_t)y * sp);
		for (r = 0; r < 3; r++) {
			uint16_t *d = (uint16_t *)((uint8_t *)dst + (size_t)(y * 3 + r) * dp);
			for (x = 0; x < sw; x++)
				d[x*3] = d[x*3+1] = d[x*3+2] = s[x];
		}
	}
}
