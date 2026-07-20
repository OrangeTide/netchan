/* test_keystore.c : the on-disk key and credential formats */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "keystore.h"
#include "monocypher.h"

static int failures;

static void
check(const char *what, int cond)
{
    printf("%s: %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        failures++;
}

int
main(void)
{
    char dir[] = "/tmp/nc_auth_ks_XXXXXX";
    char known[256], keyf[256], authk[256], pwf[256], hostf[256];
    uint8_t pk[32], other[32], sk[64], loaded_pk[32], stored[32];
    uint8_t host_a[32], host_b[32];
    FILE *f;

    if (!mkdtemp(dir)) {
        printf("FAIL: cannot make a temp directory\n");
        return 1;
    }
    snprintf(known, sizeof(known), "%s/known_hosts", dir);
    snprintf(keyf,  sizeof(keyf),  "%s/id_netchan", dir);
    snprintf(authk, sizeof(authk), "%s/authorized_keys", dir);
    snprintf(pwf,   sizeof(pwf),   "%s/passwd", dir);
    snprintf(hostf, sizeof(hostf), "%s/host_key", dir);

    memset(pk, 0x11, sizeof(pk));
    memset(other, 0x22, sizeof(other));

    /* known_hosts: unknown, then match, then the change that must be loud. */
    check("unseen host is unknown",
          ks_known_host(known, "example", pk, NULL) == KS_HOST_UNKNOWN);
    check("recording a host succeeds",
          ks_known_host_add(known, "example", pk) == 0);
    check("recorded host matches",
          ks_known_host(known, "example", pk, NULL) == KS_HOST_MATCH);
    check("a different key is reported as changed",
          ks_known_host(known, "example", other, stored) == KS_HOST_CHANGED);
    check("the stored key comes back for the warning",
          crypto_verify32(stored, pk) == 0);
    check("an unrelated name is still unknown",
          ks_known_host(known, "elsewhere", pk, NULL) == KS_HOST_UNKNOWN);

    /* Host key: created on first call, stable on the second. */
    check("host key is created", ks_host_key(hostf, host_a) == 0);
    check("host key is stable", ks_host_key(hostf, host_b) == 0 &&
          crypto_verify32(host_a, host_b) == 0);

    /* Key file, unencrypted. */
    check("plain key file writes",
          ks_keyfile_generate(keyf, "", pk) == 0);
    check("plain key file is not marked encrypted",
          !ks_keyfile_encrypted(keyf));
    check("plain key file loads",
          ks_keyfile_load(keyf, NULL, sk, loaded_pk) == 0 &&
          crypto_verify32(loaded_pk, pk) == 0);

    /* Key file, passphrase protected. A wrong passphrase must be reported
     * as such, not as a corrupt file, which is what the AEAD tag buys. */
    check("encrypted key file writes",
          ks_keyfile_generate(keyf, "open sesame", pk) == 0);
    check("encrypted key file is marked encrypted",
          ks_keyfile_encrypted(keyf));
    check("right passphrase unlocks",
          ks_keyfile_load(keyf, "open sesame", sk, loaded_pk) == 0 &&
          crypto_verify32(loaded_pk, pk) == 0);
    check("wrong passphrase is rejected cleanly",
          ks_keyfile_load(keyf, "open sesam", sk, loaded_pk) == -2);
    check("a missing passphrase is rejected",
          ks_keyfile_load(keyf, NULL, sk, loaded_pk) == -2);

    /* authorized_keys. */
    f = fopen(authk, "w");
    if (f) {
        char hex[65];

        ks_hex_encode(hex, pk, sizeof(pk));
        fprintf(f, "# a comment\nalice %s\n", hex);
        fclose(f);
    }
    check("an authorised key is accepted",
          ks_authorized_key(authk, "alice", pk));
    check("the same key under another name is not",
          !ks_authorized_key(authk, "bob", pk));
    check("an unlisted key is not accepted",
          !ks_authorized_key(authk, "alice", other));

    /* passwd. */
    check("password enrols", ks_passwd_add(pwf, "alice", "correct horse") == 0);
    check("the right password verifies",
          ks_check_password(pwf, "alice", "correct horse"));
    check("a wrong password does not",
          !ks_check_password(pwf, "alice", "hunter2"));
    check("an unknown account does not",
          !ks_check_password(pwf, "mallory", "correct horse"));
    check("enrolment is visible", ks_user_exists(pwf, "alice"));
    check("an unknown account is not", !ks_user_exists(pwf, "mallory"));

    unlink(known);
    unlink(keyf);
    unlink(authk);
    unlink(pwf);
    unlink(hostf);
    rmdir(dir);

    if (failures)
        printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
