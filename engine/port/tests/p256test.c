/*
 * p256test -- LibreSSL's P-256 scalar multiplications on fiat-crypto's field (engine/deps/patches/libressl:
 * ecp_nistp256.c, which the Montgomery method hands P-256 to) against LibreSSL's generic code, the same curve built
 * both ways in one program (PLAN.md section 77). Linked with the patched libcrypto.a and compiled with LibreSSL's
 * internal headers (engine/tools/ecbench.py test does both and runs it under qemu-arm): the reference group is made
 * with EC_GROUP_new(EC_GFp_simple_method()) and that method's own group_set_curve; the simple method never hands
 * anything to ecp_nistp256.c.
 *   p256test [ROUNDS [SEED]]     ROUNDS random cases per kind (default 1000)
 * Every result of the new method is compared with the generic one in affine coordinates (or both at infinity).
 * Exit status 0 only when every check passed.
 */
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdh.h>
#include <openssl/ecdsa.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/x509.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ec_local.h"

EC_GROUP *EC_GROUP_new(const EC_METHOD *meth);

static long checks, failures;
static BN_CTX *ctx;
static EC_GROUP *fast, *generic;	/* P-256 as named (the new code) and on the simple method (the generic code) */
static BIGNUM *order, *prime;

#define CHECK(condition, ...) do { \
	checks++; \
	if (!(condition)) { \
		failures++; \
		if (failures <= 40) { \
			printf("FAIL %s:%d: ", __func__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
		} \
	} \
} while (0)

#define MUST(condition) do { \
	if (!(condition)) { \
		printf("setup failed at line %d\n", __LINE__); \
		ERR_print_errors_fp(stdout); \
		exit(2); \
	} \
} while (0)

/* xorshift64*: the cases repeat for a seed. */
static uint64_t random_state = 0x9e3779b97f4a7c15ULL;

static uint64_t
random64(void)
{
	random_state ^= random_state >> 12;
	random_state ^= random_state << 25;
	random_state ^= random_state >> 27;
	return random_state * 0x2545f4914f6cdd1dULL;
}

static void
random_bn(BIGNUM *out, int bits)
{
	unsigned char bytes[64];
	size_t i;

	for (i = 0; i < sizeof(bytes); i++)
		bytes[i] = (unsigned char)(random64() >> 32);
	MUST(BN_bin2bn(bytes, sizeof(bytes), out) != NULL);
	MUST(BN_mask_bits(out, bits) || BN_num_bits(out) <= bits);
}

/* A scalar of a random kind: below the order, sparse, dense, near the order, near a power of two. */
static void
random_scalar(BIGNUM *out)
{
	int kind = (int)(random64() % 16), i;

	switch (kind) {
	case 0:	/* a few bits set */
		BN_zero(out);
		for (i = (int)(random64() % 6); i >= 0; i--)
			MUST(BN_set_bit(out, (int)(random64() % 256)));
		break;
	case 1:	/* a few bits clear */
		MUST(BN_set_bit(out, 256) && BN_sub_word(out, 1));
		for (i = (int)(random64() % 6); i >= 0; i--)
			MUST(BN_clear_bit(out, (int)(random64() % 256)));
		break;
	case 2:	/* order - small */
		MUST(BN_copy(out, order) != NULL && BN_sub_word(out, (BN_ULONG)(random64() % 1000)));
		break;
	case 3:	/* small */
		MUST(BN_set_word(out, (BN_ULONG)(random64() % 1000)));
		break;
	case 4:	/* 2^i - small or 2^i + small */
		BN_zero(out);
		MUST(BN_set_bit(out, (int)(random64() % 257)));
		if (random64() & 1)
			MUST(BN_add_word(out, (BN_ULONG)(random64() % 40)));
		else
			MUST(BN_sub_word(out, (BN_ULONG)(random64() % 40)));
		if (BN_is_negative(out))
			BN_zero(out);
		break;
	case 5:	/* a repeated 32-bit word: the comb's four bits alike */
		{
			unsigned char bytes[32];
			uint32_t word = (uint32_t)random64();

			for (i = 0; i < 32; i++)
				bytes[i] = (unsigned char)(word >> (8 * (i % 4)));
			MUST(BN_bin2bn(bytes, sizeof(bytes), out) != NULL);
		}
		break;
	case 6:	/* any 256 bits, the order or above included */
		random_bn(out, 256);
		break;
	default:
		random_bn(out, 320);
		MUST(BN_nnmod(out, out, order, ctx));
		break;
	}
}

/* Whether the group's scalar multiplications go to ecp_nistp256.c. */
static int
fast_path(const EC_GROUP *group)
{
	EC_POINT *point = EC_POINT_new(group);
	int ret;

	MUST(point != NULL && EC_POINT_set_to_infinity(group, point));
	ret = ec_nistp256_mul_single_ct(group, point, BN_value_one(), point, ctx);
	EC_POINT_free(point);
	if (ret == 0)
		MUST(!"ec_nistp256_mul_single_ct failed");
	return ret == 1;
}

/* The point of one group in the other, through affine coordinates. */
static void
transfer(const EC_GROUP *to, EC_POINT *out, const EC_GROUP *from, const EC_POINT *in)
{
	BIGNUM *x, *y;

	if (EC_POINT_is_at_infinity(from, in)) {
		MUST(EC_POINT_set_to_infinity(to, out));
		return;
	}
	BN_CTX_start(ctx);
	MUST((x = BN_CTX_get(ctx)) != NULL && (y = BN_CTX_get(ctx)) != NULL);
	MUST(EC_POINT_get_affine_coordinates(from, in, x, y, ctx));
	MUST(EC_POINT_set_affine_coordinates(to, out, x, y, ctx));
	BN_CTX_end(ctx);
}

/* Whether a point of the new method and one of the generic method are the same point. */
static int
same(const EC_POINT *a, const EC_POINT *b)
{
	BIGNUM *xa, *ya, *xb, *yb;
	int infinity_a = EC_POINT_is_at_infinity(fast, a), infinity_b = EC_POINT_is_at_infinity(generic, b);
	int ret = 0;

	if (infinity_a || infinity_b)
		return infinity_a && infinity_b;
	BN_CTX_start(ctx);
	MUST((xa = BN_CTX_get(ctx)) != NULL && (ya = BN_CTX_get(ctx)) != NULL);
	MUST((xb = BN_CTX_get(ctx)) != NULL && (yb = BN_CTX_get(ctx)) != NULL);
	if (EC_POINT_get_affine_coordinates(fast, a, xa, ya, ctx) &&
	    EC_POINT_get_affine_coordinates(generic, b, xb, yb, ctx))
		ret = BN_cmp(xa, xb) == 0 && BN_cmp(ya, yb) == 0 && !BN_is_negative(xa) && !BN_is_negative(ya) &&
		    BN_cmp(xa, prime) < 0 && BN_cmp(ya, prime) < 0;
	BN_CTX_end(ctx);
	return ret;
}

static char *
hex(const BIGNUM *bn)
{
	static char buffers[4][200];
	static int next;
	char *text = BN_bn2hex(bn), *out = buffers[next++ & 3];

	snprintf(out, sizeof(buffers[0]), "%s", text ? text : "?");
	free(text);
	return out;
}

/* k*G, k*P and u1*G + u2*Q on both groups for the scalars and the generic point given. */
static void
check_mul(const BIGNUM *k1, const BIGNUM *k2, const EC_POINT *generic_point, const char *what)
{
	EC_POINT *fa = EC_POINT_new(fast), *fb = EC_POINT_new(fast), *ga = EC_POINT_new(generic);
	int ok_fast, ok_generic;

	MUST(fa != NULL && fb != NULL && ga != NULL);
	transfer(fast, fb, generic, generic_point);

	ok_fast = EC_POINT_mul(fast, fa, k1, NULL, NULL, ctx);
	ok_generic = EC_POINT_mul(generic, ga, k1, NULL, NULL, ctx);
	CHECK(ok_fast && ok_generic && same(fa, ga), "%s: k*G, k = %s", what, hex(k1));
	CHECK(EC_POINT_is_on_curve(fast, fa, ctx) == 1, "%s: k*G off the curve, k = %s", what, hex(k1));

	ok_fast = EC_POINT_mul(fast, fa, NULL, fb, k2, ctx);
	ok_generic = EC_POINT_mul(generic, ga, NULL, generic_point, k2, ctx);
	CHECK(ok_fast && ok_generic && same(fa, ga), "%s: k*P, k = %s", what, hex(k2));
	CHECK(EC_POINT_is_on_curve(fast, fa, ctx) == 1, "%s: k*P off the curve, k = %s", what, hex(k2));

	ok_fast = EC_POINT_mul(fast, fa, k1, fb, k2, ctx);
	ok_generic = EC_POINT_mul(generic, ga, k1, generic_point, k2, ctx);
	CHECK(ok_fast && ok_generic && same(fa, ga), "%s: u1*G + u2*Q, u1 = %s u2 = %s", what, hex(k1), hex(k2));
	CHECK(EC_POINT_is_on_curve(fast, fa, ctx) == 1, "%s: u1*G + u2*Q off the curve", what);

	/* The result over its own operand. */
	ok_fast = EC_POINT_mul(fast, fb, NULL, fb, k2, ctx);
	ok_generic = EC_POINT_mul(generic, ga, NULL, generic_point, k2, ctx);
	CHECK(ok_fast && ok_generic && same(fb, ga), "%s: k*P in place, k = %s", what, hex(k2));

	EC_POINT_free(fa);
	EC_POINT_free(fb);
	EC_POINT_free(ga);
}

static void
test_groups(void)
{
	BIGNUM *p = BN_new(), *a = BN_new(), *b = BN_new(), *x = BN_new(), *y = BN_new(), *minus3 = BN_new();
	EC_GROUP *other, *p384, *copy;
	EC_POINT *point;
	unsigned char *der = NULL, *explicit_der = NULL;
	const unsigned char *in;
	int len;

	MUST(p && a && b && x && y && minus3);
	CHECK(fast->meth == EC_GFp_mont_method() && fast_path(fast), "the named group does not take the new code");
	CHECK(generic->meth == EC_GFp_simple_method() && !fast_path(generic), "the reference group takes the new code");
	CHECK(EC_GROUP_check(fast, ctx) == 1, "EC_GROUP_check");
	CHECK(EC_GROUP_cmp(fast, generic, ctx) == -1 || EC_GROUP_cmp(fast, generic, ctx) == 1,
	    "groups of two methods compared equal");
	ERR_clear_error();

	MUST(EC_GROUP_get_curve(fast, p, a, b, ctx));
	MUST(EC_GROUP_get_curve(generic, x, y, minus3, ctx));
	CHECK(BN_cmp(p, x) == 0 && BN_cmp(a, y) == 0 && BN_cmp(b, minus3) == 0, "get_curve differs");
	CHECK(EC_GROUP_get_degree(fast) == 256 && EC_GROUP_order_bits(fast) == 256, "degree or order bits");

	/* A copy keeps the method and compares equal. */
	MUST((copy = EC_GROUP_dup(fast)) != NULL);
	CHECK(copy->meth == EC_GFp_mont_method() && fast_path(copy), "dup changed the method");
	CHECK(EC_GROUP_cmp(fast, copy, ctx) == 0, "dup compares different");
	CHECK(EC_GROUP_get_curve_name(copy) == NID_X9_62_prime256v1, "dup lost the name");
	MUST((point = EC_POINT_new(copy)) != NULL);
	CHECK(EC_POINT_copy(point, EC_GROUP_get0_generator(fast)) == 1, "EC_POINT_copy between two P-256 groups");
	CHECK(EC_POINT_cmp(fast, point, EC_GROUP_get0_generator(copy), ctx) == 0, "copied generator differs");
	EC_POINT_free(point);
	EC_GROUP_free(copy);

	/* A point of the generic method is not accepted by the new one, and the reverse. */
	MUST((point = EC_POINT_new(generic)) != NULL);
	CHECK(EC_POINT_copy(point, EC_GROUP_get0_generator(fast)) == 0, "EC_POINT_copy across methods");
	CHECK(EC_POINT_is_on_curve(fast, point, ctx) == -1, "a generic point in the new method");
	CHECK(EC_POINT_mul(fast, point, BN_value_one(), NULL, NULL, ctx) == 0, "a generic result in the new method");
	ERR_clear_error();
	EC_POINT_free(point);

	/* Built by a caller: with a = p - 3 and with a = -3. */
	MUST((other = EC_GROUP_new_curve_GFp(p, a, b, ctx)) != NULL);
	CHECK(other->meth == EC_GFp_mont_method() && fast_path(other), "EC_GROUP_new_curve_GFp(P-256) is generic");
	CHECK(EC_GROUP_get0_generator(other) == NULL, "a new group has a generator");
	MUST((point = EC_POINT_new(other)) != NULL);
	CHECK(EC_POINT_mul(other, point, BN_value_one(), NULL, NULL, ctx) == 0, "k*G without a generator");
	ERR_clear_error();
	MUST(EC_POINT_get_affine_coordinates(fast, EC_GROUP_get0_generator(fast), x, y, ctx));
	MUST(EC_POINT_set_affine_coordinates(other, point, x, y, ctx));
	MUST(EC_GROUP_set_generator(other, point, order, BN_value_one()));
	CHECK(EC_GROUP_check(other, ctx) == 1, "EC_GROUP_check of a built group");
	CHECK(EC_GROUP_cmp(fast, other, ctx) == 0, "built group compares different");
	/* A wrong order does not pass the check. */
	MUST(BN_copy(x, order) != NULL && BN_sub_word(x, 2));
	MUST(EC_GROUP_set_generator(other, point, x, BN_value_one()));
	CHECK(EC_GROUP_check(other, ctx) == 0, "EC_GROUP_check accepted a wrong order");
	ERR_clear_error();
	EC_POINT_free(point);
	EC_GROUP_free(other);

	MUST(BN_set_word(minus3, 3));
	BN_set_negative(minus3, 1);
	MUST((other = EC_GROUP_new_curve_GFp(p, minus3, b, ctx)) != NULL);
	CHECK(other->meth == EC_GFp_mont_method() && fast_path(other), "EC_GROUP_new_curve_GFp(a = -3) is generic");
	MUST(EC_GROUP_get_curve(other, NULL, x, NULL, ctx));
	CHECK(BN_cmp(x, a) == 0, "a = -3 is not returned as p - 3");
	EC_GROUP_free(other);

	/* Another b: the generic method. */
	MUST(BN_copy(x, b) != NULL && BN_add_word(x, 1));
	MUST((other = EC_GROUP_new_curve_GFp(p, a, x, ctx)) != NULL);
	CHECK(other->meth == EC_GFp_mont_method() && !fast_path(other), "another curve on P-256's prime took the new code");
	/*
	 * EC_GROUP_set_curve() to P-256 and away again, as LibreSSL's ectest does with one group and one point for
	 * every curve: the group keeps its method, so a point made before stays the group's, and the new code follows.
	 */
	MUST((point = EC_POINT_new(other)) != NULL);
	CHECK(EC_GROUP_set_curve(other, p, a, b, ctx) == 1 && fast_path(other), "set_curve to P-256");
	MUST(EC_POINT_get_affine_coordinates(fast, EC_GROUP_get0_generator(fast), x, y, ctx));
	CHECK(EC_POINT_set_compressed_coordinates(other, point, x, BN_is_odd(y), ctx) == 1,
	    "G on a group set to P-256, with a point made before");
	MUST(EC_GROUP_set_generator(other, point, order, BN_value_one()));
	CHECK(EC_GROUP_check(other, ctx) == 1, "EC_GROUP_check after set_curve");
	{
		EC_POINT *f = EC_POINT_new(other), *g = EC_POINT_new(generic);

		MUST(f != NULL && g != NULL && BN_set_word(x, 12345));
		CHECK(EC_POINT_mul(other, f, x, NULL, NULL, ctx) == 1 && EC_POINT_mul(generic, g, x, NULL, NULL, ctx) == 1,
		    "k*G on a group set to P-256");
		MUST(EC_POINT_get_affine_coordinates(other, f, y, minus3, ctx));
		MUST(EC_POINT_get_affine_coordinates(generic, g, p, a, ctx));
		CHECK(BN_cmp(y, p) == 0 && BN_cmp(minus3, a) == 0, "k*G on a group set to P-256 differs");
		EC_POINT_free(f);
		EC_POINT_free(g);
		MUST(EC_GROUP_get_curve(fast, p, a, b, ctx));
	}
	EC_POINT_free(point);

	MUST((p384 = EC_GROUP_new_by_curve_name(NID_secp384r1)) != NULL);
	CHECK(p384->meth == EC_GFp_mont_method(), "P-384 is not generic");
	MUST(EC_GROUP_get_curve(p384, p, a, b, ctx));
	CHECK(EC_GROUP_set_curve(other, p, a, b, ctx) == 1 && !fast_path(other), "set_curve from P-256 to P-384");
	MUST((point = EC_POINT_new(other)) != NULL);
	MUST(EC_POINT_get_affine_coordinates(p384, EC_GROUP_get0_generator(p384), x, y, ctx));
	CHECK(EC_POINT_set_affine_coordinates(other, point, x, y, ctx) == 1, "P-384's G on the group set to P-384");
	MUST(EC_GROUP_get_order(p384, x, ctx));
	MUST(EC_GROUP_set_generator(other, point, x, BN_value_one()));
	CHECK(EC_GROUP_check(other, ctx) == 1, "EC_GROUP_check of the group set to P-384");
	CHECK(EC_GROUP_cmp(other, p384, ctx) == 0, "the group set to P-384 differs from P-384");
	EC_POINT_free(point);
	EC_GROUP_free(other);
	EC_GROUP_free(p384);

	/* Named and explicit parameters through ASN.1. */
	MUST((len = i2d_ECPKParameters(fast, &der)) > 0);
	in = der;
	MUST((other = d2i_ECPKParameters(NULL, &in, len)) != NULL);
	CHECK(fast_path(other) && EC_GROUP_cmp(fast, other, ctx) == 0, "named parameters");
	EC_GROUP_free(other);
	MUST((copy = EC_GROUP_dup(fast)) != NULL);
	EC_GROUP_set_asn1_flag(copy, 0);
	MUST((len = i2d_ECPKParameters(copy, &explicit_der)) > 0);
	in = explicit_der;
	MUST((other = d2i_ECPKParameters(NULL, &in, len)) != NULL);
	CHECK(fast_path(other) && EC_GROUP_cmp(fast, other, ctx) == 0, "explicit parameters (%d bytes)", len);
	EC_GROUP_free(other);
	EC_GROUP_free(copy);
	free(der);
	free(explicit_der);

	BN_free(p);
	BN_free(a);
	BN_free(b);
	BN_free(x);
	BN_free(y);
	BN_free(minus3);
}

static void
test_edge_scalars(void)
{
	BIGNUM *k = BN_new(), *k2 = BN_new(), *d = BN_new();
	EC_POINT *points[8], *fa = EC_POINT_new(fast), *fb = EC_POINT_new(fast), *ga = EC_POINT_new(generic);
	const EC_POINT *g = EC_GROUP_get0_generator(generic);
	const char *names[8] = { "G", "-G", "2G", "infinity", "x = 0", "d*G", "-(d*G)", "(n-1)/2 * G" };
	int i, j, n;

	MUST(k && k2 && d && fa && fb && ga);
	for (i = 0; i < 8; i++)
		MUST((points[i] = EC_POINT_new(generic)) != NULL);
	MUST(EC_POINT_copy(points[0], g));
	MUST(EC_POINT_copy(points[1], g) && EC_POINT_invert(generic, points[1], ctx));
	MUST(EC_POINT_dbl(generic, points[2], g, ctx));
	MUST(EC_POINT_set_to_infinity(generic, points[3]));
	BN_zero(k);
	MUST(EC_POINT_set_compressed_coordinates(generic, points[4], k, 1, ctx));
	random_bn(d, 255);
	MUST(EC_POINT_mul(generic, points[5], d, NULL, NULL, ctx));
	MUST(EC_POINT_copy(points[6], points[5]) && EC_POINT_invert(generic, points[6], ctx));
	MUST(BN_rshift1(k, order) && EC_POINT_mul(generic, points[7], k, NULL, NULL, ctx));

	for (i = 0; i < 8; i++) {
		/* 0, 1, 2, ..., n - 2, n - 1, n, n + 1, 2n, 2^256 - 1, 2^256, beyond, negative. */
		for (j = 0; j < 20; j++) {
			MUST(BN_set_word(k, (BN_ULONG)j));
			MUST(BN_copy(k2, order) != NULL && BN_sub_word(k2, (BN_ULONG)j));
			check_mul(k, k2, points[i], names[i]);
			check_mul(k2, k, points[i], names[i]);
			MUST(BN_copy(k2, order) != NULL && BN_add_word(k2, (BN_ULONG)j));
			check_mul(k2, k2, points[i], names[i]);
		}
		MUST(BN_lshift1(k, order));
		BN_zero(k2);
		MUST(BN_set_bit(k2, 256));
		check_mul(k, k2, points[i], names[i]);
		MUST(BN_sub_word(k2, 1));
		check_mul(k2, k, points[i], names[i]);
		random_bn(k, 400);
		random_bn(k2, 511);
		check_mul(k, k2, points[i], names[i]);
		BN_set_negative(k, 1);
		check_mul(k, k2, points[i], names[i]);
		BN_set_negative(k2, 1);
		check_mul(k, k2, points[i], names[i]);
		MUST(BN_set_word(k, 1));
		BN_set_negative(k, 1);
		check_mul(k, k, points[i], names[i]);
		/* Every power of two, and one less. */
		for (j = 0; j <= 257; j += (i < 2 ? 1 : 13)) {
			BN_zero(k);
			MUST(BN_set_bit(k, j));
			MUST(BN_copy(k2, k) != NULL && BN_sub_word(k2, 1));
			check_mul(k, k2, points[i], names[i]);
		}
	}

	/* Small u1 and u2 with small multiples of G as Q: the sums meet doublings and cancellations on the way. */
	n = 12;
	for (i = -n; i <= n; i++) {
		MUST(BN_set_word(d, (BN_ULONG)(i < 0 ? -i : i)));
		BN_set_negative(d, i < 0);
		MUST(EC_POINT_mul(generic, ga, d, NULL, NULL, ctx));
		transfer(fast, fb, generic, ga);
		for (j = 0; j <= n; j++) {
			int l;

			for (l = 0; l <= n; l++) {
				EC_POINT *gr = EC_POINT_new(generic);

				MUST(gr != NULL);
				MUST(BN_set_word(k, (BN_ULONG)j) && BN_set_word(k2, (BN_ULONG)l));
				CHECK(EC_POINT_mul(fast, fa, k, fb, k2, ctx) == 1 &&
				    EC_POINT_mul(generic, gr, k, ga, k2, ctx) == 1 && same(fa, gr),
				    "%d*G + %d*(%d*G)", j, l, i);
				EC_POINT_free(gr);
			}
		}
	}

	/* u1*G = -(u2*Q) and u1*G = u2*Q with Q = d*G: the sum is the point at infinity, or a doubling at the end. */
	for (i = 0; i < 40; i++) {
		EC_POINT *gr = EC_POINT_new(generic);

		MUST(gr != NULL);
		random_bn(d, 256);
		MUST(BN_nnmod(d, d, order, ctx));
		if (BN_is_zero(d))
			MUST(BN_one(d));
		random_scalar(k2);
		MUST(EC_POINT_mul(generic, ga, d, NULL, NULL, ctx));
		transfer(fast, fb, generic, ga);
		MUST(BN_mod_mul(k, k2, d, order, ctx));
		CHECK(EC_POINT_mul(fast, fa, k, fb, k2, ctx) == 1 && EC_POINT_mul(generic, gr, k, ga, k2, ctx) == 1 &&
		    same(fa, gr), "u1*G = u2*Q");
		MUST(BN_sub(k, order, k));
		CHECK(EC_POINT_mul(fast, fa, k, fb, k2, ctx) == 1 && EC_POINT_mul(generic, gr, k, ga, k2, ctx) == 1 &&
		    same(fa, gr), "u1*G = -(u2*Q)");
		CHECK(BN_is_zero(k2) || BN_cmp(k2, order) == 0 || EC_POINT_is_at_infinity(fast, fa) == 1,
		    "u1*G - u1*G is not the point at infinity");
		EC_POINT_free(gr);
	}

	for (i = 0; i < 8; i++)
		EC_POINT_free(points[i]);
	EC_POINT_free(fa);
	EC_POINT_free(fb);
	EC_POINT_free(ga);
	BN_free(k);
	BN_free(k2);
	BN_free(d);
}

static void
test_random_mul(int rounds)
{
	BIGNUM *k = BN_new(), *k2 = BN_new(), *d = BN_new();
	EC_POINT *gp = EC_POINT_new(generic);
	int i;

	MUST(k && k2 && d && gp);
	for (i = 0; i < rounds; i++) {
		random_scalar(d);
		MUST(EC_POINT_mul(generic, gp, d, NULL, NULL, ctx));
		random_scalar(k);
		random_scalar(k2);
		check_mul(k, k2, gp, "random");
	}
	BN_free(k);
	BN_free(k2);
	BN_free(d);
	EC_POINT_free(gp);
}

/* The new method's points arrive in Jacobian coordinates from a multiplication, or affine from coordinates. */
static void
make_pair(EC_POINT *fp, EC_POINT *gp, int kind)
{
	BIGNUM *d;

	BN_CTX_start(ctx);
	MUST((d = BN_CTX_get(ctx)) != NULL);
	switch (kind % 8) {
	case 0:
		MUST(EC_POINT_set_to_infinity(fast, fp) && EC_POINT_set_to_infinity(generic, gp));
		break;
	case 1:	/* affine */
		random_scalar(d);
		MUST(EC_POINT_mul(generic, gp, d, NULL, NULL, ctx));
		transfer(fast, fp, generic, gp);
		break;
	case 2:	/* a small multiple: pairs repeat and cancel */
	case 3:
		MUST(BN_set_word(d, (BN_ULONG)(random64() % 5)));
		BN_set_negative(d, (int)(random64() & 1));
		MUST(EC_POINT_mul(generic, gp, d, NULL, NULL, ctx));
		MUST(EC_POINT_mul(fast, fp, d, NULL, NULL, ctx));
		break;
	default:	/* Jacobian on both sides */
		random_scalar(d);
		MUST(EC_POINT_mul(generic, gp, d, NULL, NULL, ctx));
		MUST(EC_POINT_mul(fast, fp, d, NULL, NULL, ctx));
		break;
	}
	BN_CTX_end(ctx);
}

static void
test_point_operations(int rounds)
{
	EC_POINT *f1 = EC_POINT_new(fast), *f2 = EC_POINT_new(fast), *f3 = EC_POINT_new(fast), *f4 = EC_POINT_new(fast);
	EC_POINT *g1 = EC_POINT_new(generic), *g2 = EC_POINT_new(generic), *g3 = EC_POINT_new(generic);
	BIGNUM *x = BN_new(), *y = BN_new(), *x2 = BN_new(), *y2 = BN_new();
	unsigned char fast_bytes[80], generic_bytes[80];
	size_t fast_len, generic_len;
	int forms[3] = { POINT_CONVERSION_COMPRESSED, POINT_CONVERSION_UNCOMPRESSED, POINT_CONVERSION_HYBRID };
	int i, j, expected;

	MUST(f1 && f2 && f3 && f4 && g1 && g2 && g3 && x && y && x2 && y2);
	for (i = 0; i < rounds; i++) {
		make_pair(f1, g1, (int)random64());
		make_pair(f2, g2, (int)random64());
		CHECK(same(f1, g1) && same(f2, g2), "operands differ");
		CHECK(EC_POINT_is_on_curve(fast, f1, ctx) == 1, "operand off the curve");

		CHECK(EC_POINT_add(fast, f3, f1, f2, ctx) == 1 && EC_POINT_add(generic, g3, g1, g2, ctx) == 1 &&
		    same(f3, g3), "add");
		CHECK(EC_POINT_is_on_curve(fast, f3, ctx) == 1, "sum off the curve");
		CHECK(EC_POINT_add(fast, f4, f2, f1, ctx) == 1 && EC_POINT_cmp(fast, f3, f4, ctx) == 0, "add commutes");
		CHECK(EC_POINT_add(fast, f3, f1, f1, ctx) == 1 && EC_POINT_add(generic, g3, g1, g1, ctx) == 1 &&
		    same(f3, g3), "add a point to itself");
		CHECK(EC_POINT_dbl(fast, f4, f1, ctx) == 1 && same(f4, g3) && EC_POINT_cmp(fast, f3, f4, ctx) == 0, "dbl");
		/* In place. */
		MUST(EC_POINT_copy(f3, f1) && EC_POINT_copy(g3, g1));
		CHECK(EC_POINT_add(fast, f3, f3, f2, ctx) == 1 && EC_POINT_add(generic, g3, g3, g2, ctx) == 1 &&
		    same(f3, g3), "add in place (first)");
		MUST(EC_POINT_copy(f3, f2) && EC_POINT_copy(g3, g2));
		CHECK(EC_POINT_add(fast, f3, f1, f3, ctx) == 1 && EC_POINT_add(generic, g3, g1, g3, ctx) == 1 &&
		    same(f3, g3), "add in place (second)");
		MUST(EC_POINT_copy(f3, f1) && EC_POINT_copy(g3, g1));
		CHECK(EC_POINT_dbl(fast, f3, f3, ctx) == 1 && EC_POINT_dbl(generic, g3, g3, ctx) == 1 && same(f3, g3),
		    "dbl in place");
		MUST(EC_POINT_copy(f3, f1));
		CHECK(EC_POINT_add(fast, f3, f3, f3, ctx) == 1 && same(f3, g3), "add in place (all three)");

		/* The opposite, and a point plus its opposite. */
		MUST(EC_POINT_copy(f3, f1) && EC_POINT_copy(g3, g1));
		CHECK(EC_POINT_invert(fast, f3, ctx) == 1 && EC_POINT_invert(generic, g3, ctx) == 1 && same(f3, g3),
		    "invert");
		CHECK(EC_POINT_is_on_curve(fast, f3, ctx) == 1, "opposite off the curve");
		CHECK(EC_POINT_add(fast, f4, f1, f3, ctx) == 1 && EC_POINT_is_at_infinity(fast, f4) == 1,
		    "a point plus its opposite");

		/* Comparison as the generic method sees it. */
		expected = EC_POINT_cmp(generic, g1, g2, ctx);
		CHECK(EC_POINT_cmp(fast, f1, f2, ctx) == expected, "cmp: %d expected", expected);
		expected = EC_POINT_cmp(generic, g1, g3, ctx);
		CHECK(EC_POINT_cmp(fast, f1, f3, ctx) == expected, "cmp with the opposite: %d expected", expected);
		MUST(EC_POINT_copy(f4, f1) );
		CHECK(EC_POINT_cmp(fast, f1, f4, ctx) == 0, "cmp with a copy");
		if (!EC_POINT_is_at_infinity(fast, f1)) {
			/* Affine again: the same point in other coordinates. */
			CHECK(EC_POINT_make_affine(fast, f4, ctx) == 1 && EC_POINT_cmp(fast, f1, f4, ctx) == 0 &&
			    EC_POINT_cmp(fast, f4, f1, ctx) == 0 && same(f4, g1), "make_affine");
		} else {
			CHECK(EC_POINT_get_affine_coordinates(fast, f1, x, y, ctx) == 0, "coordinates of infinity");
			ERR_clear_error();
		}

		/* Encodings, each form, and back. */
		for (j = 0; j < 3; j++) {
			fast_len = EC_POINT_point2oct(fast, f1, forms[j], fast_bytes, sizeof(fast_bytes), ctx);
			generic_len = EC_POINT_point2oct(generic, g1, forms[j], generic_bytes, sizeof(generic_bytes),
			    ctx);
			CHECK(fast_len != 0 && fast_len == generic_len && memcmp(fast_bytes, generic_bytes, fast_len) == 0,
			    "point2oct form %d", forms[j]);
			CHECK(EC_POINT_oct2point(fast, f4, fast_bytes, fast_len, ctx) == 1 && same(f4, g1) &&
			    EC_POINT_cmp(fast, f4, f1, ctx) == 0, "oct2point form %d", forms[j]);
			if (fast_len > 1) {
				/* A bit flipped in x or y: as the generic method decides. */
				generic_bytes[1 + random64() % (generic_len - 1)] ^= (unsigned char)(1 << (random64() % 8));
				expected = EC_POINT_oct2point(generic, g3, generic_bytes, generic_len, ctx);
				CHECK(EC_POINT_oct2point(fast, f4, generic_bytes, generic_len, ctx) == expected &&
				    (!expected || same(f4, g3)), "oct2point of a changed encoding: %d expected", expected);
				ERR_clear_error();
			}
		}

		/* Coordinates that are not on the curve, not reduced, or negative: as the generic method decides. */
		if (!EC_POINT_is_at_infinity(generic, g1)) {
			MUST(EC_POINT_get_affine_coordinates(generic, g1, x, y, ctx));
			MUST(BN_copy(y2, y) != NULL && BN_add_word(y2, 1));
			expected = EC_POINT_set_affine_coordinates(generic, g3, x, y2, ctx);
			CHECK(EC_POINT_set_affine_coordinates(fast, f4, x, y2, ctx) == expected && expected == 0,
			    "y + 1 accepted");
			MUST(BN_add(x2, x, prime) && BN_sub(y2, y, prime));
			expected = EC_POINT_set_affine_coordinates(generic, g3, x2, y2, ctx);
			CHECK(EC_POINT_set_affine_coordinates(fast, f4, x2, y2, ctx) == expected &&
			    (!expected || same(f4, g3)), "x + p, y - p: %d expected", expected);
			MUST(BN_lshift(x2, prime, 70) && BN_add(x2, x2, x));
			expected = EC_POINT_set_affine_coordinates(generic, g3, x2, y, ctx);
			CHECK(EC_POINT_set_affine_coordinates(fast, f4, x2, y, ctx) == expected &&
			    (!expected || same(f4, g3)), "x + p * 2^70: %d expected", expected);
			expected = EC_POINT_set_compressed_coordinates(generic, g3, x, BN_is_odd(y), ctx);
			CHECK(EC_POINT_set_compressed_coordinates(fast, f4, x, BN_is_odd(y), ctx) == expected &&
			    expected == 1 && same(f4, g1), "compressed coordinates");
			/* A random x: half of them are not on the curve. */
			random_bn(x2, 256);
			expected = EC_POINT_set_compressed_coordinates(generic, g3, x2, i & 1, ctx);
			CHECK(EC_POINT_set_compressed_coordinates(fast, f4, x2, i & 1, ctx) == expected &&
			    (!expected || same(f4, g3)), "compressed random x: %d expected", expected);
			ERR_clear_error();
		}
	}
	EC_POINT_free(f1);
	EC_POINT_free(f2);
	EC_POINT_free(f3);
	EC_POINT_free(f4);
	EC_POINT_free(g1);
	EC_POINT_free(g2);
	EC_POINT_free(g3);
	BN_free(x);
	BN_free(y);
	BN_free(x2);
	BN_free(y2);
}

/* The same key on a group of each method. */
static void
key_pair(EC_KEY **fast_key, EC_KEY **generic_key, int from_fast)
{
	EC_KEY *source, *target;
	EC_POINT *point;
	const EC_GROUP *source_group = from_fast ? fast : generic, *target_group = from_fast ? generic : fast;

	MUST((source = EC_KEY_new()) != NULL && (target = EC_KEY_new()) != NULL);
	MUST(EC_KEY_set_group(source, source_group) && EC_KEY_set_group(target, target_group));
	MUST(EC_KEY_generate_key(source));
	CHECK(EC_KEY_check_key(source) == 1, "EC_KEY_check_key of a generated key");
	MUST((point = EC_POINT_new(EC_KEY_get0_group(target))) != NULL);
	transfer(EC_KEY_get0_group(target), point, EC_KEY_get0_group(source), EC_KEY_get0_public_key(source));
	MUST(EC_KEY_set_private_key(target, EC_KEY_get0_private_key(source)));
	MUST(EC_KEY_set_public_key(target, point));
	CHECK(EC_KEY_check_key(target) == 1, "EC_KEY_check_key of the key in the other method");
	EC_POINT_free(point);
	CHECK(EC_KEY_get0_group(source)->meth == source_group->meth &&
	    EC_KEY_get0_group(target)->meth == target_group->meth, "a key's group changed method");
	*fast_key = from_fast ? source : target;
	*generic_key = from_fast ? target : source;
}

static void
test_ecdsa_ecdh(int rounds)
{
	unsigned char digest[64], fast_secret[32], generic_secret[32];
	EC_KEY *fast_key, *generic_key, *fast_peer, *generic_peer;
	ECDSA_SIG *sig;
	BIGNUM *r, *s;
	const BIGNUM *r0, *s0;
	size_t j;
	int i, digest_len;

	for (i = 0; i < rounds; i++) {
		key_pair(&fast_key, &generic_key, i & 1);
		for (j = 0; j < sizeof(digest); j++)
			digest[j] = (unsigned char)(random64() >> 24);
		digest_len = (i % 3 == 0) ? 32 : (i % 3 == 1) ? 48 : 64;

		/* Signed by one, verified by both; then a changed digest, r and s. */
		MUST((sig = ECDSA_do_sign(digest, digest_len, (i & 2) ? fast_key : generic_key)) != NULL);
		CHECK(ECDSA_do_verify(digest, digest_len, sig, fast_key) == 1, "verify (new method), signer %d", i & 2);
		CHECK(ECDSA_do_verify(digest, digest_len, sig, generic_key) == 1, "verify (generic), signer %d", i & 2);
		digest[random64() % 32] ^= 0x10;
		CHECK(ECDSA_do_verify(digest, digest_len, sig, fast_key) == 0, "verify of another digest");
		CHECK(ECDSA_do_verify(digest, digest_len, sig, generic_key) == 0, "generic verify of another digest");
		digest[0] ^= 1;
		ECDSA_SIG_get0(sig, &r0, &s0);
		MUST((r = BN_dup(r0)) != NULL && (s = BN_dup(s0)) != NULL);
		if (i & 4)
			MUST(BN_add_word(r, 1));
		else
			MUST(BN_add_word(s, 1));
		MUST(ECDSA_SIG_set0(sig, r, s));
		CHECK(ECDSA_do_verify(digest, digest_len, sig, fast_key) ==
		    ECDSA_do_verify(digest, digest_len, sig, generic_key), "verify of a changed signature");
		ERR_clear_error();
		ECDSA_SIG_free(sig);

		/* ECDH with a second key pair, all four ways. */
		key_pair(&fast_peer, &generic_peer, (i >> 1) & 1);
		CHECK(ECDH_compute_key(fast_secret, 32, EC_KEY_get0_public_key(fast_peer), fast_key, NULL) == 32 &&
		    ECDH_compute_key(generic_secret, 32, EC_KEY_get0_public_key(generic_peer), generic_key, NULL) == 32
		    && memcmp(fast_secret, generic_secret, 32) == 0, "ECDH");
		CHECK(ECDH_compute_key(fast_secret, 32, EC_KEY_get0_public_key(fast_key), fast_peer, NULL) == 32 &&
		    memcmp(fast_secret, generic_secret, 32) == 0, "ECDH from the other side");
		CHECK(ECDH_compute_key(generic_secret, 32, EC_KEY_get0_public_key(generic_key), generic_peer, NULL) == 32
		    && memcmp(fast_secret, generic_secret, 32) == 0, "generic ECDH from the other side");

		EC_KEY_free(fast_key);
		EC_KEY_free(generic_key);
		EC_KEY_free(fast_peer);
		EC_KEY_free(generic_peer);
	}
}

/* A P-256 group whose generator is not G: no table, the generic point paths of the new method. */
static void
test_other_generator(int rounds)
{
	EC_GROUP *fast_other = EC_GROUP_dup(fast), *generic_other = EC_GROUP_dup(generic);
	EC_POINT *fh, *gh, *fr, *gr, *fq, *gq;
	BIGNUM *k = BN_new(), *k2 = BN_new(), *x = BN_new(), *y = BN_new(), *x2 = BN_new(), *y2 = BN_new();
	int i, ok;

	MUST(fast_other && generic_other && k && k2 && x && y && x2 && y2);
	fh = EC_POINT_new(fast_other);
	gh = EC_POINT_new(generic_other);
	fr = EC_POINT_new(fast_other);
	gr = EC_POINT_new(generic_other);
	fq = EC_POINT_new(fast_other);
	gq = EC_POINT_new(generic_other);
	MUST(fh && gh && fr && gr && fq && gq);
	MUST(BN_set_word(k, 7));
	MUST(EC_POINT_mul(generic, gh, k, NULL, NULL, ctx) && EC_POINT_mul(fast, fh, k, NULL, NULL, ctx));
	MUST(EC_GROUP_set_generator(generic_other, gh, order, BN_value_one()));
	MUST(EC_GROUP_set_generator(fast_other, fh, order, BN_value_one()));
	CHECK(EC_GROUP_check(fast_other, ctx) == 1, "EC_GROUP_check with the generator 7G");
	CHECK(EC_GROUP_cmp(fast, fast_other, ctx) == 1, "groups with different generators compare equal");

	for (i = 0; i < rounds; i++) {
		random_scalar(k);
		random_scalar(k2);
		MUST(EC_POINT_mul(generic, gq, k2, NULL, NULL, ctx));
		if (i % 7 == 0)
			MUST(EC_POINT_copy(gq, EC_GROUP_get0_generator(generic)));	/* the real G as Q */
		MUST(EC_POINT_get_affine_coordinates(generic_other, gq, x, y, ctx));
		MUST(EC_POINT_set_affine_coordinates(fast_other, fq, x, y, ctx));
		random_scalar(k2);

		ok = EC_POINT_mul(fast_other, fr, k, NULL, NULL, ctx) && EC_POINT_mul(generic_other, gr, k, NULL, NULL, ctx);
		if (ok && !EC_POINT_is_at_infinity(generic_other, gr))
			ok = EC_POINT_get_affine_coordinates(fast_other, fr, x, y, ctx) &&
			    EC_POINT_get_affine_coordinates(generic_other, gr, x2, y2, ctx) && BN_cmp(x, x2) == 0 &&
			    BN_cmp(y, y2) == 0;
		else if (ok)
			ok = EC_POINT_is_at_infinity(fast_other, fr);
		CHECK(ok, "k*H, k = %s", hex(k));

		ok = EC_POINT_mul(fast_other, fr, k, fq, k2, ctx) && EC_POINT_mul(generic_other, gr, k, gq, k2, ctx);
		if (ok && !EC_POINT_is_at_infinity(generic_other, gr))
			ok = EC_POINT_get_affine_coordinates(fast_other, fr, x, y, ctx) &&
			    EC_POINT_get_affine_coordinates(generic_other, gr, x2, y2, ctx) && BN_cmp(x, x2) == 0 &&
			    BN_cmp(y, y2) == 0;
		else if (ok)
			ok = EC_POINT_is_at_infinity(fast_other, fr);
		CHECK(ok, "u1*H + u2*Q, u1 = %s u2 = %s", hex(k), hex(k2));
	}
	EC_POINT_free(fh);
	EC_POINT_free(gh);
	EC_POINT_free(fr);
	EC_POINT_free(gr);
	EC_POINT_free(fq);
	EC_POINT_free(gq);
	EC_GROUP_free(fast_other);
	EC_GROUP_free(generic_other);
	BN_free(k);
	BN_free(k2);
	BN_free(x);
	BN_free(y);
	BN_free(x2);
	BN_free(y2);
}

int
main(int argc, char **argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 1000;
	BIGNUM *a = BN_new(), *b = BN_new(), *x = BN_new(), *y = BN_new();
	EC_POINT *g;
	long before;

	if (argc > 2)
		random_state ^= strtoull(argv[2], NULL, 0);
	MUST((ctx = BN_CTX_new()) != NULL && a && b && x && y);
	MUST((order = BN_new()) != NULL && (prime = BN_new()) != NULL);
	MUST((fast = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1)) != NULL);
	MUST(EC_GROUP_get_curve(fast, prime, a, b, ctx) && EC_GROUP_get_order(fast, order, ctx));

	/* The same curve on the simple method, whose scalar multiplications are the generic code. */
	MUST((generic = EC_GROUP_new(EC_GFp_simple_method())) != NULL);
	MUST(EC_GROUP_set_curve(generic, prime, a, b, ctx));
	MUST((g = EC_POINT_new(generic)) != NULL);
	MUST(EC_POINT_get_affine_coordinates(fast, EC_GROUP_get0_generator(fast), x, y, ctx));
	MUST(EC_POINT_set_affine_coordinates(generic, g, x, y, ctx));
	MUST(EC_GROUP_set_generator(generic, g, order, BN_value_one()));
	EC_GROUP_set_curve_name(generic, NID_X9_62_prime256v1);
	MUST(EC_GROUP_check(generic, ctx));
	EC_POINT_free(g);

#define RUN(call) do { \
	before = checks; \
	call; \
	printf("%-28s %8ld checks\n", #call, checks - before); \
	fflush(stdout); \
} while (0)
	RUN(test_groups());
	RUN(test_edge_scalars());
	RUN(test_random_mul(rounds));
	RUN(test_point_operations(rounds));
	RUN(test_ecdsa_ecdh(rounds / 4 + 1));
	RUN(test_other_generator(rounds / 4 + 1));

	printf("p256test: %ld checks, %ld failed\n", checks, failures);
	if (failures)
		ERR_print_errors_fp(stdout);
	return failures != 0;
}
