#include "utils.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/random.h>
#include "mbedtls/md.h"

/* PATCHED for netchan nc_rtc (see nc_rtc/README.md): the upstream version called
 * srand(time(NULL)) on every call, so two credentials generated in the same
 * second were identical (the ice-pwd even started with the ice-ufrag). That
 * misroutes ICE when two peers are created close together. Draw from the OS
 * CSPRNG instead; fall back to a once-seeded rand() only if that fails. */
void utils_random_string(char* s, const int len) {
  int i;

  static const char alphanum[] =
      "0123456789"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz";

  unsigned char buf[256];
  int n = len < (int)sizeof(buf) ? len : (int)sizeof(buf) - 1;

  if (getrandom(buf, (size_t)n, 0) == (ssize_t)n) {
    for (i = 0; i < n; ++i) {
      s[i] = alphanum[buf[i] % (sizeof(alphanum) - 1)];
    }
  } else {
    static int seeded = 0;
    if (!seeded) {
      srand((unsigned)(time(NULL) ^ (getpid() << 16)));
      seeded = 1;
    }
    for (i = 0; i < n; ++i) {
      s[i] = alphanum[rand() % (sizeof(alphanum) - 1)];
    }
  }

  s[len] = '\0';
}

void utils_get_hmac_sha1(const char* input, size_t input_len, const char* key, size_t key_len, unsigned char* output) {
  mbedtls_md_context_t ctx;
  mbedtls_md_type_t md_type = MBEDTLS_MD_SHA1;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)key, key_len);
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)input, input_len);
  mbedtls_md_hmac_finish(&ctx, output);
  mbedtls_md_free(&ctx);
}

void utils_get_md5(const char* input, size_t input_len, unsigned char* output) {
  mbedtls_md_context_t ctx;
  mbedtls_md_type_t md_type = MBEDTLS_MD_MD5;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 1);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, (const unsigned char*)input, input_len);
  mbedtls_md_finish(&ctx, output);
  mbedtls_md_free(&ctx);
}
