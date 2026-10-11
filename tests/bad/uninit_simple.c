#include <stdio.h>

int main(void) {
    int total;                  /* never initialised */
    for (int i = 0; i < 3; i++)
        total += i;             /* reads garbage on the first iteration */
    printf("%d\n", total);
    return 0;
}
