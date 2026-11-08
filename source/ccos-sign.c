/* ccos-sign - the signing key for ccos updates (runs on the build machine)
 *   ccos-sign keygen KEYFILE         make a new key; prints the public key (hex)
 *   ccos-sign sign KEYFILE FILE      prints the signature of FILE (hex)
 *   ccos-sign verify PUBHEX SIGHEX FILE
 * build: gcc -O2 -o ccos-sign ccos-sign.c -lsodium */
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(1); }
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(*n + 1); if (fread(b, 1, *n, f) != *n) exit(1); fclose(f); return b;
}
static void hex(const unsigned char *b, size_t n) { for (size_t i = 0; i < n; i++) printf("%02x", b[i]); printf("\n"); }
int main(int argc, char **argv) {
    if (sodium_init() < 0) return 1;
    if (argc == 3 && !strcmp(argv[1], "keygen")) {
        unsigned char pk[32], sk[64]; crypto_sign_keypair(pk, sk);
        FILE *f = fopen(argv[2], "wx"); if (!f) { perror(argv[2]); return 1; }
        fwrite(sk, 1, 64, f); fclose(f); hex(pk, 32); return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "sign")) {
        size_t kn, n; unsigned char *sk = slurp(argv[2], &kn), *m = slurp(argv[3], &n), sig[64];
        if (kn != 64) { fprintf(stderr, "bad key file\n"); return 1; }
        crypto_sign_detached(sig, NULL, m, n, sk); hex(sig, 64); return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "pub")) {
        size_t kn; unsigned char *sk = slurp(argv[2], &kn); hex(sk + 32, 32); return 0;
    }
    if (argc == 5 && !strcmp(argv[1], "verify")) {
        unsigned char pk[32], sig[64]; size_t n; unsigned char *m = slurp(argv[4], &n);
        if (sodium_hex2bin(pk, 32, argv[2], 64, NULL, NULL, NULL) || sodium_hex2bin(sig, 64, argv[3], 128, NULL, NULL, NULL)) return 2;
        int ok = crypto_sign_verify_detached(sig, m, n, pk) == 0; puts(ok ? "good" : "BAD"); return !ok;
    }
    fprintf(stderr, "usage: ccos-sign keygen KEY | sign KEY FILE | pub KEY | verify PUB SIG FILE\n"); return 1;
}
