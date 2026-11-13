/*
 * p256wycheproof -- Project Wycheproof's P-256 vectors (ECDSA verification with SHA-256, SHA-512, SHA3-256, SHA3-512
 * in DER and IEEE P1363 form; ECDH with the peer's key as a SubjectPublicKeyInfo or as an X9.62 point) through
 * LibreSSL's public API, as libssl reaches it (PLAN.md section 77). engine/tools/ecbench.py wycheproof turns the JSON
 * files into one line per test, runs this against the released LibreSSL and against ours, and compares.
 *   p256wycheproof FILE   lines "ecdsa SHA FORM ID RESULT SPKI MSG SIG" or "ecdh FORM ID RESULT PUBLIC PRIVATE SHARED"
 *                         (hex, "-" for empty; RESULT valid, invalid or acceptable)
 * A valid test must verify (ECDH: give the shared secret), an invalid one must be refused; for an acceptable one
 * either is right and the line "acceptable ID 0|1" says which happened, for the comparison of the two libraries.
 * Prints "FAIL ..." for every test that went wrong and a summary; exit status 0 only when none did.
 */
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdh.h>
#include <openssl/ecdsa.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/x509.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *
from_hex(const char *hex, size_t *len)
{
	size_t n = strcmp(hex, "-") ? strlen(hex) / 2 : 0, i;
	unsigned char *out = malloc(n + 1);

	if (out == NULL)
		exit(2);
	for (i = 0; i < n; i++) {
		unsigned int byte;

		if (sscanf(hex + 2 * i, "%2x", &byte) != 1)
			exit(2);
		out[i] = (unsigned char)byte;
	}
	*len = n;
	return out;
}

static const EVP_MD *
digest_named(const char *name)
{
	if (!strcmp(name, "SHA-256"))
		return EVP_sha256();
	if (!strcmp(name, "SHA-512"))
		return EVP_sha512();
	if (!strcmp(name, "SHA3-256"))
		return EVP_sha3_256();
	if (!strcmp(name, "SHA3-512"))
		return EVP_sha3_512();
	return NULL;
}

/* 1 if the signature verified, 0 if it was refused (any failure on the way). */
static int
ecdsa_test(const char *sha, const char *form, const char *spki_hex, const char *msg_hex, const char *sig_hex)
{
	size_t spki_len, msg_len, sig_len;
	unsigned char *spki = from_hex(spki_hex, &spki_len), *msg = from_hex(msg_hex, &msg_len);
	unsigned char *sig = from_hex(sig_hex, &sig_len), digest[EVP_MAX_MD_SIZE];
	const unsigned char *p = spki;
	unsigned int digest_len;
	const EVP_MD *md = digest_named(sha);
	EC_KEY *key = NULL;
	ECDSA_SIG *s = NULL;
	BIGNUM *r_bn = NULL, *s_bn = NULL;
	int ok = 0;

	if (md == NULL) {
		fprintf(stderr, "unknown digest %s\n", sha);
		exit(2);
	}
	if ((key = d2i_EC_PUBKEY(NULL, &p, (long)spki_len)) == NULL)
		goto done;
	if (!EVP_Digest(msg, msg_len, digest, &digest_len, md, NULL))
		goto done;
	if (!strcmp(form, "der")) {
		ok = ECDSA_verify(0, digest, (int)digest_len, sig, (int)sig_len, key) == 1;
	} else {
		/* IEEE P1363: r and s, each as long as the order, nothing else. */
		if (sig_len != 64)
			goto done;
		if ((r_bn = BN_bin2bn(sig, 32, NULL)) == NULL || (s_bn = BN_bin2bn(sig + 32, 32, NULL)) == NULL)
			goto done;
		if ((s = ECDSA_SIG_new()) == NULL || !ECDSA_SIG_set0(s, r_bn, s_bn))
			goto done;
		r_bn = s_bn = NULL;
		ok = ECDSA_do_verify(digest, (int)digest_len, s, key) == 1;
	}
 done:
	ERR_clear_error();
	ECDSA_SIG_free(s);
	BN_free(r_bn);
	BN_free(s_bn);
	EC_KEY_free(key);
	free(spki);
	free(msg);
	free(sig);
	return ok;
}

/* 1 if the exchange gave the expected secret, 0 if it was refused; -1 if it gave another secret. */
static int
ecdh_test(const char *form, const char *public_hex, const char *private_hex, const char *shared_hex)
{
	size_t public_len, private_len, shared_len;
	unsigned char *public = from_hex(public_hex, &public_len), *private = from_hex(private_hex, &private_len);
	unsigned char *shared = from_hex(shared_hex, &shared_len), out[64];
	const unsigned char *p = public;
	EC_KEY *ours = NULL, *peer = NULL;
	EC_POINT *point = NULL;
	const EC_POINT *peer_point = NULL;
	BIGNUM *d = NULL;
	int n, ok = 0;

	if ((ours = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1)) == NULL)
		exit(2);
	if ((d = BN_bin2bn(private, (int)private_len, NULL)) == NULL || !EC_KEY_set_private_key(ours, d))
		goto done;
	if (!strcmp(form, "asn")) {
		if ((peer = d2i_EC_PUBKEY(NULL, &p, (long)public_len)) == NULL)
			goto done;
		peer_point = EC_KEY_get0_public_key(peer);
	} else {
		if ((point = EC_POINT_new(EC_KEY_get0_group(ours))) == NULL)
			exit(2);
		if (!EC_POINT_oct2point(EC_KEY_get0_group(ours), point, public, public_len, NULL))
			goto done;
		peer_point = point;
	}
	if (peer_point == NULL)
		goto done;
	n = ECDH_compute_key(out, sizeof(out), peer_point, ours, NULL);
	if (n <= 0)
		goto done;
	ok = (size_t)n == shared_len && memcmp(out, shared, shared_len) == 0 ? 1 : -1;
 done:
	ERR_clear_error();
	EC_POINT_free(point);
	EC_KEY_free(peer);
	EC_KEY_free(ours);
	BN_free(d);
	free(public);
	free(private);
	free(shared);
	return ok;
}

int
main(int argc, char **argv)
{
	static char line[1 << 20];
	long tests = 0, failures = 0, acceptable = 0;
	FILE *in;

	if (argc < 2 || (in = fopen(argv[1], "r")) == NULL)
		return 2;
	while (fgets(line, sizeof(line), in) != NULL) {
		char *field[9];
		int n = 0, got, want_valid, want_invalid;
		char *save = NULL, *token;

		for (token = strtok_r(line, " \n", &save); token != NULL && n < 9; token = strtok_r(NULL, " \n", &save))
			field[n++] = token;
		if (n == 0)
			continue;
		if (!strcmp(field[0], "ecdsa") && n == 8) {
			got = ecdsa_test(field[1], field[2], field[5], field[6], field[7]);
			want_valid = !strcmp(field[4], "valid");
			want_invalid = !strcmp(field[4], "invalid");
		} else if (!strcmp(field[0], "ecdh") && n == 7) {
			got = ecdh_test(field[1], field[4], field[5], field[6]);
			want_valid = !strcmp(field[3], "valid");
			want_invalid = !strcmp(field[3], "invalid");
		} else {
			fprintf(stderr, "bad line: %s\n", field[0]);
			return 2;
		}
		tests++;
		{
			const char *id = field[0][3] == 's' ? field[3] : field[2];

			if (got < 0) {
				failures++;
				printf("FAIL %s %s: a wrong shared secret\n", field[0], id);
			} else if (want_valid && got != 1) {
				failures++;
				printf("FAIL %s %s: valid, refused\n", field[0], id);
			} else if (want_invalid && got != 0) {
				failures++;
				printf("FAIL %s %s: invalid, accepted\n", field[0], id);
			} else if (!want_valid && !want_invalid) {
				acceptable++;
				printf("acceptable %s %s %d\n", field[0], id, got);
			}
		}
	}
	fclose(in);
	printf("p256wycheproof %s: %ld tests (%ld acceptable), %ld failed\n", argv[1], tests, acceptable, failures);
	return failures != 0;
}
