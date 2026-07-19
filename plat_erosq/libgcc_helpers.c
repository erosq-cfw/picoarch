/*
 * Exported libgcc arithmetic helpers for the dlopen'd libretro cores.
 *
 * The prebuilt cores (static-PIE gpsp especially) leave these unresolved and
 * expect the picoarch host binary to supply them at dlopen. Buildroot's
 * libgcc.a defines them as .hidden, so -rdynamic cannot re-export libgcc's
 * copies. We therefore define our own DEFAULT-visibility versions here: being
 * in a linked object (not an on-demand archive member) they win over libgcc's,
 * and -rdynamic then exports them in .dynsym for the cores to resolve against.
 *
 *   gpsp:     __bswapsi2 __fixdfdi __udivdi3
 *   gambatte: __divdi3 __udivdi3 __umoddi3
 *
 * The 64-bit divide is a plain shift/subtract long-division so the compiler
 * never lowers it back to __udivdi3 (which would recurse). Internal 64-bit
 * shifts may still call libgcc's (hidden, local) __ashldi3/__lshrdi3 — that's
 * fine, no core needs those.
 */
#include <stdint.h>

uint32_t __bswapsi2(uint32_t x)
{
	return ((x & 0x000000ffu) << 24) |
	       ((x & 0x0000ff00u) <<  8) |
	       ((x & 0x00ff0000u) >>  8) |
	       ((x & 0xff000000u) >> 24);
}

static uint64_t udivmod64(uint64_t n, uint64_t d, uint64_t *rem)
{
	uint64_t q = 0, r = 0;
	int i;

	if (d == 0) {            /* undefined; mirror libgcc's "return ~0" */
		if (rem) *rem = 0;
		return ~0ULL;
	}
	for (i = 63; i >= 0; i--) {
		r = (r << 1) | ((n >> i) & 1ULL);
		if (r >= d) {
			r -= d;
			q |= (1ULL << i);
		}
	}
	if (rem) *rem = r;
	return q;
}

uint64_t __udivdi3(uint64_t n, uint64_t d)
{
	return udivmod64(n, d, 0);
}

uint64_t __umoddi3(uint64_t n, uint64_t d)
{
	uint64_t r;
	udivmod64(n, d, &r);
	return r;
}

int64_t __divdi3(int64_t a, int64_t b)
{
	int neg = 0;
	uint64_t ua, ub, q;

	if (a < 0) { ua = -(uint64_t)a; neg ^= 1; } else ua = (uint64_t)a;
	if (b < 0) { ub = -(uint64_t)b; neg ^= 1; } else ub = (uint64_t)b;
	q = udivmod64(ua, ub, 0);
	return neg ? -(int64_t)q : (int64_t)q;
}

/* double -> signed 64-bit, truncating toward zero, via IEEE-754 bit surgery
 * (a (long long)d cast would lower to __fixdfdi and recurse). */
int64_t __fixdfdi(double dv)
{
	union { double d; uint64_t u; } v;
	uint64_t bits, mant;
	int sign, exp;
	int64_t result;

	v.d = dv;
	bits = v.u;
	sign = (int)(bits >> 63);
	exp  = (int)((bits >> 52) & 0x7ff) - 1023;
	if (exp < 0)
		return 0;                       /* |v| < 1 (incl. denormals) */
	if (exp >= 63)
		return sign ? INT64_MIN : INT64_MAX;  /* overflow / inf / nan */
	mant = (bits & 0xfffffffffffffULL) | 0x10000000000000ULL; /* implicit 1 */
	if (exp >= 52)
		result = (int64_t)(mant << (exp - 52));
	else
		result = (int64_t)(mant >> (52 - exp));
	return sign ? -result : result;
}
