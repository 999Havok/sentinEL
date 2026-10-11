#include <stdio.h>

int main(int argc, char **argv) {
    (void)argv;
    int mode;
    if (argc > 1)
        mode = 1;               /* only set on one path */
    printf("%d\n", mode);       /* may be uninitialised */
    return 0;
}
