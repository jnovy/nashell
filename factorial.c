#include <stdio.h>
#include <stdlib.h>

/* Iterative factorial function */
unsigned long long factorial(int n) {
    if (n < 0) {
        fprintf(stderr, "Error: factorial is not defined for negative numbers\n");
        return 0;
    }

    unsigned long long result = 1;
    for (int i = 2; i <= n; i++) {
        result *= i;
    }
    return result;
}

/* Simple test framework */
static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT_EQ(expected, actual, msg) do { \
    tests_run++; \
    if ((expected) == (actual)) { \
        tests_passed++; \
        printf("  PASS: %s\n", msg); \
    } else { \
        printf("  FAIL: %s (expected %llu, got %llu)\n", msg, (unsigned long long)(expected), (unsigned long long)(actual)); \
    } \
} while(0)

void run_tests(void) {
    printf("Running factorial tests...\n\n");

    /* Test 0! = 1 */
    ASSERT_EQ(1, factorial(0), "0! = 1");

    /* Test 1! = 1 */
    ASSERT_EQ(1, factorial(1), "1! = 1");

    /* Test 2! = 2 */
    ASSERT_EQ(2, factorial(2), "2! = 2");

    /* Test 3! = 6 */
    ASSERT_EQ(6, factorial(3), "3! = 6");

    /* Test 5! = 120 */
    ASSERT_EQ(120, factorial(5), "5! = 120");

    /* Test 10! = 3628800 */
    ASSERT_EQ(3628800ULL, factorial(10), "10! = 3628800");

    /* Test 20! = 2432902008176640000 */
    ASSERT_EQ(2432902008176640000ULL, factorial(20), "20! = 2432902008176640000");

    /* Test negative input */
    printf("\nTest negative input (should print error to stderr):\n");
    factorial(-1);

    /* Summary */
    printf("\n========================================\n");
    printf("Results: %d/%d tests passed\n", tests_passed, tests_run);
    if (tests_passed == tests_run) {
        printf("All tests passed!\n");
    } else {
        printf("Some tests failed!\n");
    }
    printf("========================================\n");
}

int main(int argc, char *argv[]) {
    /* If run with --test flag, run the test suite */
    if (argc > 1 && argv[1][0] == '-') {
        if (argv[1][1] == 't' || argv[1][1] == 'T') {
            run_tests();
            return (tests_passed == tests_run) ? 0 : 1;
        }
    }

    /* Default: interactive mode */
    printf("Factorial Calculator\n");
    printf("====================\n");
    printf("Enter a non-negative integer (or 'q' to quit):\n");

    char input[64];
    while (fgets(input, sizeof(input), stdin)) {
        if (input[0] == 'q' || input[0] == 'Q') {
            printf("Goodbye!\n");
            break;
        }

        int n = atoi(input);
        if (n < 0) {
            printf("Please enter a non-negative integer.\n");
            continue;
        }

        unsigned long long result = factorial(n);
        printf("%d! = %llu\n\n", n, result);
        printf("Enter a non-negative integer (or 'q' to quit):\n");
    }

    return 0;
}
