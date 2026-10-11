#include <stdlib.h>
#include <string.h>

/* The buffer is freed on only one branch, then used after the branches
   join. On the path where verbose == 0 this is a use after free. */
int main(int argc, char **argv) {
    (void)argv;
    int verbose = argc > 1;
    char *buf = malloc(64);
    if (buf == NULL)
        return 1;
    memset(buf, 0, 64);
    if (!verbose) {
        free(buf);
    } else {
        buf[0] = 'v';
    }
    buf[1] = 'x';               /* use after free on the !verbose path */
    return 0;
}
