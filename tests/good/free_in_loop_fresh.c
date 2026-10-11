#include <stdlib.h>
#include <string.h>

/* malloc + free inside a loop: each iteration has its own buffer. */
int main(void) {
    int total = 0;
    for (int i = 0; i < 5; i++) {
        char *line = malloc(32);
        if (line == NULL)
            return 1;
        memset(line, 'x', 31);
        line[31] = '\0';
        total += (int)strlen(line);
        free(line);
    }
    return total == 155 ? 0 : 1;
}
