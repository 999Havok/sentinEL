#include <stdio.h>
#include <string.h>

/* Bounded versions of everything the AST checker bans: must NOT be flagged. */
int main(int argc, char **argv) {
    char name[16];
    char msg[32];
    char word[16];
    char line[64];
    int n = 0;
    strlcpy(name, argc > 1 ? argv[1] : "world", sizeof name);
    strlcat(name, "!", sizeof name);
    snprintf(msg, sizeof msg, "Hello, %s", name);
    puts(msg);
    if (fgets(line, sizeof line, stdin) != NULL) {
        sscanf(line, "%d %15s", &n, word);   /* width given: OK */
        sscanf(line, "%*s %15s", word);      /* %*s stores nothing: OK */
        printf("100%% done: %d %s\n", n, word);
    }
    return 0;
}
