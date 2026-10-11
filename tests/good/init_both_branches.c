#include <stdio.h>

/* Initialised on every path before use: must NOT trigger -Wuninitialized. */
int main(int argc, char **argv) {
    (void)argv;
    int mode;
    if (argc > 1)
        mode = 1;
    else
        mode = 0;
    printf("%d\n", mode);
    return 0;
}
