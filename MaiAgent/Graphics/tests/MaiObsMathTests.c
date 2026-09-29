#include <stdio.h>

#include "../matrix4.h"

static int failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

static bool close_enough(float actual, float expected) {
    float difference = actual - expected;
    return difference > -0.0001f && difference < 0.0001f;
}

int main(void) {
    struct matrix4 identity;
    struct matrix4 translated;
    struct matrix4 inverse;
    struct vec3 offset;
    struct vec3 input;
    struct vec3 output;
    struct vec3 restored;

    matrix4_identity(&identity);
    vec3_set(&offset, 2.0f, 3.0f, 4.0f);
    matrix4_translate3v(&translated, &identity, &offset);
    vec3_set(&input, 1.0f, 2.0f, 3.0f);
    vec3_transform(&output, &input, &translated);
    CHECK(close_enough(output.x, 3.0f));
    CHECK(close_enough(output.y, 5.0f));
    CHECK(close_enough(output.z, 7.0f));
    CHECK(matrix4_inv(&inverse, &translated));
    vec3_transform(&restored, &output, &inverse);
    CHECK(close_enough(restored.x, input.x));
    CHECK(close_enough(restored.y, input.y));
    CHECK(close_enough(restored.z, input.z));
    return failures ? 1 : 0;
}
