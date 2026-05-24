#include <stdio.h>
#include <math.h>
#include <stdlib.h>

// Function to check if a number is prime
int is_prime(int n) {
    if (n < 2) return 0;
    if (n == 2) return 1;
    if (n % 2 == 0) return 0;
    for (int i = 3; i <= (int)sqrt((double)n); i += 2) {
        if (n % i == 0) return 0;
    }
    return 1;
}

int main(void) {
    const int COUNT = 1000;
    int primes[COUNT];
    int found = 0;
    int candidate = 2;

    printf("Calculating the first %d prime numbers...\n\n", COUNT);

    while (found < COUNT) {
        if (is_prime(candidate)) {
            primes[found] = candidate;
            found++;
        }
        candidate++;
    }

    // Print the primes in rows of 10
    for (int i = 0; i < COUNT; i++) {
        printf("%6d ", primes[i]);
        if ((i + 1) % 10 == 0) {
            printf("\n");
        }
    }

    printf("\n\nThe 1000th prime number is: %d\n", primes[COUNT - 1]);

    return 0;
}
