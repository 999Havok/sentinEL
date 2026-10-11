#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    char name[16];
    char msg[32];
    if (argc < 2)
        return 1;
    strcpy(name, argv[1]);              /* overflow if argv[1] >= 16 chars */
    strcat(name, "!");
    sprintf(msg, "Hello, %s", name);
    puts(msg);
    return 0;
}
