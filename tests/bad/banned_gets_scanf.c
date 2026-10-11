#include <stdio.h>

char *gets(char *);   /* removed from C11 headers; old code still calls it */

int main(void) {
    char line[64];
    char word[16];
    char copy[16];
    int n;
    gets(line);                          /* no bound at all */
    scanf("%d %s", &n, word);            /* bare %s: no width */
    sscanf(line, "%s", copy);            /* bare %s: no width */
    printf("%d %s %s\n", n, word, copy);
    return 0;
}
