#include <stdio.h>
#include <stdlib.h>

struct node {
    int value;
    struct node *next;
};

int main(void) {
    struct node *n = malloc(sizeof *n);
    if (n == NULL)
        return 1;
    n->value = 7;
    n->next = NULL;
    free(n);
    printf("%d\n", n->value);   /* use after free (read) */
    return 0;
}
