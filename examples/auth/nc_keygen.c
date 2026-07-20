/* nc_keygen.c : make a client identity key, print it for authorized_keys */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "keystore.h"
#include "prompt.h"

static void
usage(void)
{
    fprintf(stderr,
        "usage: nc_keygen -f <keyfile> [-N <passphrase>] [-q]\n"
        "\n"
        "Generates an Ed25519 identity key. With no -N the tool prompts, and\n"
        "an empty answer leaves the key unencrypted. Prints the line to add\n"
        "to the server's authorized_keys file.\n");
    exit(2);
}

int
main(int argc, char **argv)
{
    const char *path = NULL;
    const char *pass = NULL;
    char buf[256], again[256];
    char hex[65];
    uint8_t pk[32];
    int quiet = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc)
            path = argv[++i];
        else if (strcmp(argv[i], "-N") == 0 && i + 1 < argc)
            pass = argv[++i];
        else if (strcmp(argv[i], "-q") == 0)
            quiet = 1;
        else
            usage();
    }
    if (!path)
        usage();

    if (!pass) {
        if (prompt_hidden("Passphrase (empty for none): ", buf, sizeof(buf)) != 0)
            return 1;
        if (buf[0] != '\0') {
            if (prompt_hidden("Same again: ", again, sizeof(again)) != 0)
                return 1;
            if (strcmp(buf, again) != 0) {
                fprintf(stderr, "nc_keygen: passphrases differ\n");
                return 1;
            }
        }
        pass = buf;
    }

    if (ks_keyfile_generate(path, pass, pk) != 0) {
        fprintf(stderr, "nc_keygen: cannot write %s\n", path);
        return 1;
    }

    ks_hex_encode(hex, pk, sizeof(pk));
    if (quiet) {
        printf("%s\n", hex);
    } else {
        printf("wrote %s (%s)\n", path,
               pass[0] ? "passphrase protected" : "unencrypted");
        printf("add this line to the server's authorized_keys:\n");
        printf("\n    <username> %s\n\n", hex);
    }
    return 0;
}
