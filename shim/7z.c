/* Local-build stand-in for 7-Zip: build-arch.bat only uses 7z to make zips we don't need locally. */
#include <stdio.h>

int main(int argc, char** argv)
{
    (void)argv;
    printf("[7z shim] skipped packaging step (%d args)\n", argc - 1);
    return 0;
}
