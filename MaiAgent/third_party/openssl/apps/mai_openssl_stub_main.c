#ifdef MAI_OPENSSL_EMBEDDED
#include "mai_openssl_embed.h"

/* Build-only entry so OpenSSL's normal make target emits every CLI object. The app does not
 * package or launch that executable; the static archive excludes this object. */
int main(int argc, char **argv)
{
    return mai_openssl_execute(argc, argv);
}
#endif
