#include <stdlib.h>

int main(void) {
    int *arr = malloc(4 * sizeof(int));
    if (arr == NULL)
        return 1;
    for (int i = 0; i < 4; i++) {
        arr[i] = i;
        if (i == 2)
            free(arr);           /* freed in iteration 2 ... */
    }                            /* ... but written again in iteration 3 */
    return 0;
}
