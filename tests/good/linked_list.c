#include <stdio.h>
#include <stdlib.h>

struct node {
    int value;
    struct node *next;
};

/* Classic correct list teardown: read next BEFORE freeing the node. */
int main(void) {
    struct node *head = NULL;
    for (int i = 0; i < 3; i++) {
        struct node *n = malloc(sizeof *n);
        if (n == NULL)
            return 1;
        n->value = i;
        n->next = head;
        head = n;
    }
    int sum = 0;
    while (head != NULL) {
        struct node *next = head->next;
        sum += head->value;
        free(head);
        head = next;
    }
    printf("%d\n", sum);
    return 0;
}
