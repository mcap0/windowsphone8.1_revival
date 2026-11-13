/*
 * p256field -- the P-256 field multiplication and squaring of ecp_nistp256_field.h (engine/deps/patches/libressl)
 * against fiat-crypto's proven fiat_p256_mul() and fiat_p256_square() (fiat_p256_32.h), word for word (PLAN.md section
 * 77). Both headers come from the patched LibreSSL tree (crypto/ec); engine/tools/ecbench.py test compiles this for
 * Linux ARMv7 and runs it under qemu-arm.
 *   p256field [RANDOM [SEED]]   RANDOM random pairs (default 2000000) after every pair of the structured values
 *   p256field --bench mul|sqr|fiat-mul|fiat-sqr N   N operations, for engine/tools/armcount
 * The structured values: 0, 1, 2, small numbers, 2^k and p - 2^k for every k, 2^256 mod p (one in the Montgomery
 * domain) and its square, (p - 1) / 2 and its neighbours, words of all ones or all zeros in every pattern below p, and
 * random values with long runs of ones or zeros; every pair of them is multiplied both ways, in place too.
 * Exit status 0 only when every result is the same as fiat's.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fiat_p256_32.h"
#include "ecp_nistp256_field.h"

typedef uint32_t felem[8];

static const felem P = { 0xffffffff, 0xffffffff, 0xffffffff, 0, 0, 0, 1, 0xffffffff };
static long checks, failures;

static uint64_t random_state = 0x6a09e667f3bcc909ULL;

static uint32_t
random32(void)
{
	random_state ^= random_state >> 12;
	random_state ^= random_state << 25;
	random_state ^= random_state >> 27;
	return (uint32_t)((random_state * 0x2545f4914f6cdd1dULL) >> 32);
}

static int
below_p(const felem a)
{
	int i;

	for (i = 7; i >= 0; i--) {
		if (a[i] != P[i])
			return a[i] < P[i];
	}
	return 0;
}

/* a - b over 256 bits (b <= a). */
static void
sub_words(felem out, const felem a, const felem b)
{
	uint64_t acc;
	uint32_t borrow = 0;
	int i;

	for (i = 0; i < 8; i++) {
		acc = (uint64_t)a[i] - b[i] - borrow;
		out[i] = (uint32_t)acc;
		borrow = (uint32_t)(acc >> 63);
	}
}

static void
show(const char *what, const felem a)
{
	int i;

	printf(" %s=", what);
	for (i = 7; i >= 0; i--)
		printf("%08x", a[i]);
}

static void
check_pair(const felem a, const felem b)
{
	felem fiat, ours, inplace;

	fiat_p256_mul(fiat, a, b);
	p256_field_mul(ours, a, b);
	checks++;
	if (memcmp(fiat, ours, sizeof(felem)) != 0) {
		if (failures++ < 20) {
			printf("mul:");
			show("a", a);
			show("b", b);
			show("fiat", fiat);
			show("ours", ours);
			printf("\n");
		}
	}
	memcpy(inplace, a, sizeof(felem));
	p256_field_mul(inplace, inplace, b);
	checks++;
	if (memcmp(fiat, inplace, sizeof(felem)) != 0 && failures++ < 20)
		printf("mul in place (first operand) differs\n");
	memcpy(inplace, b, sizeof(felem));
	p256_field_mul(inplace, a, inplace);
	checks++;
	if (memcmp(fiat, inplace, sizeof(felem)) != 0 && failures++ < 20)
		printf("mul in place (second operand) differs\n");
}

static void
check_square(const felem a)
{
	felem fiat, ours, inplace;

	fiat_p256_square(fiat, a);
	p256_field_sqr(ours, a);
	checks++;
	if (memcmp(fiat, ours, sizeof(felem)) != 0) {
		if (failures++ < 20) {
			printf("sqr:");
			show("a", a);
			show("fiat", fiat);
			show("ours", ours);
			printf("\n");
		}
	}
	memcpy(inplace, a, sizeof(felem));
	p256_field_sqr(inplace, inplace);
	checks++;
	if (memcmp(fiat, inplace, sizeof(felem)) != 0 && failures++ < 20)
		printf("sqr in place differs\n");
	check_pair(a, a);
}

static felem *values;
static int nvalues, capacity;

static void
add_value(const felem a)
{
	if (!below_p(a))
		return;
	if (nvalues == capacity) {
		capacity = capacity ? 2 * capacity : 1024;
		if ((values = realloc(values, capacity * sizeof(felem))) == NULL)
			exit(2);
	}
	memcpy(values[nvalues++], a, sizeof(felem));
}

static void
structured_values(void)
{
	felem a, b, one;
	int i, k, pattern;

	/* 0 .. 16 */
	for (i = 0; i <= 16; i++) {
		memset(a, 0, sizeof(a));
		a[0] = (uint32_t)i;
		add_value(a);
	}
	/* 2^k, 2^k - 1, p - 2^k, p - 1 - k */
	for (k = 0; k < 256; k++) {
		memset(a, 0, sizeof(a));
		a[k / 32] = 1u << (k % 32);
		add_value(a);
		sub_words(b, P, a);
		add_value(b);
		memset(one, 0, sizeof(one));
		one[0] = 1;
		sub_words(b, a, one);
		add_value(b);
		memset(b, 0, sizeof(b));
		b[0] = (uint32_t)k + 1;
		sub_words(a, P, b);
		add_value(a);
	}
	/* one in the Montgomery domain (2^256 mod p), its square, (p - 1) / 2 and its neighbours */
	memset(a, 0, sizeof(a));
	a[0] = 1;
	fiat_p256_to_montgomery(one, a);
	add_value(one);
	fiat_p256_to_montgomery(b, one);
	add_value(b);
	for (i = 0; i < 8; i++)
		a[i] = (P[i] >> 1) | (i < 7 ? P[i + 1] << 31 : 0);
	add_value(a);
	a[0]++;
	add_value(a);
	a[0] -= 2;
	add_value(a);
	/* every pattern of all-ones and all-zero words (those below p) */
	for (pattern = 0; pattern < 256; pattern++) {
		for (i = 0; i < 8; i++)
			a[i] = (pattern >> i) & 1 ? 0xffffffff : 0;
		add_value(a);
		for (i = 0; i < 8; i++)
			a[i] = (pattern >> i) & 1 ? 0xffffffff : 1;
		add_value(a);
		for (i = 0; i < 8; i++)
			a[i] = (pattern >> i) & 1 ? 0xfffffffe : 0x80000000;
		add_value(a);
	}
	/* p's own words with one word changed */
	for (i = 0; i < 8; i++) {
		for (k = 0; k < 4; k++) {
			memcpy(a, P, sizeof(a));
			a[i] = k == 0 ? 0 : k == 1 ? a[i] - 1 : k == 2 ? 0x7fffffff : random32();
			add_value(a);
		}
	}
}

/* A random value below p of a random kind: uniform words, long runs of ones or zeros, near p, near 0. */
static void
random_value(felem a)
{
	int kind, i;

	do {
		kind = (int)(random32() % 6);
		for (i = 0; i < 8; i++) {
			uint32_t w = random32();

			switch (kind) {
			case 1:
				w = (random32() & 1) ? 0xffffffff : 0;
				break;
			case 2:
				w = (random32() & 3) ? 0xffffffff : w;
				break;
			case 3:
				w = (random32() & 3) ? 0 : w;
				break;
			case 4:
				w = P[i] - (i == 0 ? (random32() & 0xff) : 0);
				break;
			case 5:
				w = (random32() & 1) ? (1u << (random32() % 32)) : (0xffffffffu >> (random32() % 32));
				break;
			}
			a[i] = w;
		}
	} while (!below_p(a));
}

static int
bench(const char *what, long n)
{
	felem a, b;
	long i;

	random_value(a);
	random_value(b);
	for (i = 0; i < n; i++) {
		if (!strcmp(what, "mul"))
			p256_field_mul(a, a, b);
		else if (!strcmp(what, "sqr"))
			p256_field_sqr(a, a);
		else if (!strcmp(what, "fiat-mul"))
			fiat_p256_mul(a, a, b);
		else if (!strcmp(what, "fiat-sqr"))
			fiat_p256_square(a, a);
		else
			return 2;
	}
	printf("%08x\n", a[0]);
	return 0;
}

int
main(int argc, char **argv)
{
	long random_pairs = 2000000, i;
	felem a, b;
	int j, k;

	if (argc > 3 && !strcmp(argv[1], "--bench"))
		return bench(argv[2], atol(argv[3]));
	if (argc > 1)
		random_pairs = atol(argv[1]);
	if (argc > 2)
		random_state ^= strtoull(argv[2], NULL, 0);

	structured_values();
	for (j = 0; j < nvalues; j++) {
		check_square(values[j]);
		for (k = 0; k < nvalues; k++)
			check_pair(values[j], values[k]);
	}
	printf("structured: %d values, %ld checks, %ld failed\n", nvalues, checks, failures);
	fflush(stdout);
	for (i = 0; i < random_pairs; i++) {
		random_value(a);
		if (i & 1)
			memcpy(b, values[random32() % nvalues], sizeof(b));
		else
			random_value(b);
		check_pair(a, b);
		if ((i & 7) == 0)
			check_square(a);
	}
	printf("p256field: %ld checks, %ld failed\n", checks, failures);
	return failures != 0;
}
