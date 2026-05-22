#include <stdio.h>
#include <assert.h>

// Function to compute factorial of a non-negative integer
unsigned long long factorial(int n) {
    if (n < 0) {
        fprintf(stderr, "Error: Factorial is not defined for negative numbers.\n");
        return 0;
    }
    unsigned long long result = 1;
    for (int i = 2; i <= n; i++) {
        result *= i;
    }
    return result;
}

// Test function to verify factorial
void test_factorial() {
    // Test case 1: factorial of 0 should be 1
    assert(factorial(0) == 1);
    printf("Test 1 passed: factorial(0) = %llu\n", factorial(0));

    // Test case 2: factorial of 1 should be 1
    assert(factorial(1) == 1);
    printf("Test 2 passed: factorial(1) = %llu\n", factorial(1));

    // Test case 3: factorial of 5 should be 120
    assert(factorial(5) == 120);
    printf("Test 3 passed: factorial(5) = %llu\n", factorial(5));

    // Test case 4: factorial of 10 should be 3628800
    assert(factorial(10) == 3628800);
    printf("Test 4 passed: factorial(10) = %llu\n", factorial(10));

    // Test case 5: factorial of 12 should be 479001600
    assert(factorial(12) == 479001600);
    printf("Test 5 passed: factorial(12) = %llu\n", factorial(12));

    printf("\nAll tests passed!\n");
}

int main() {
    printf("Testing factorial function:\n");
    test_factorial();

    printf("\nInteractive mode:\nEnter a non-negative integer: ");
    int n;
    if (scanf("%d", &n) == 1) {
        if (n >= 0 && n <= 20) {
            printf("factorial(%d) = %llu\n", n, factorial(n));
        } else {
            printf("Error: Input out of range. Please enter a number between 0 and 20.\n");
        }
    }

    return 0;
}
