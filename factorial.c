#include <stdio.h>

unsigned long long factorial(int n) {
    if (n < 0) {
        return 0; /* undefined for negative numbers */
    }
    unsigned long long result = 1;
    for (int i = 2; i <= n; i++) {
        result *= i;
    }
    return result;
}

int main(void) {
    int values[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 15, 20};
    int count = sizeof(values) / sizeof(values[0]);

    printf("Factorial computations:\n");
    for (int i = 0; i < count; i++) {
        printf("  %d! = %llu\n", values[i], factorial(values[i]));
    }

    return 0;
}
