#include <stdio.h>

#include "MaiGraphics.h"

int main(int argc, char** argv) {
    if (argc < 3) return 2;

    graphics_t* graphics = NULL;
    if (gs_create(&graphics, argv[1], 0) != GS_SUCCESS || !graphics) return 1;

    gs_enter_context(graphics);
    bool passed = true;
    for (int index = 2; index < argc; ++index) {
        char* errors = NULL;
        gs_effect_t* effect = gs_effect_create_from_file(argv[index], &errors);
        if (!effect) {
            fprintf(stderr, "Effect failed: %s (%s)\n", argv[index],
                    errors ? errors : "no details");
            passed = false;
        } else {
            gs_effect_destroy(effect);
        }
        if (errors) bfree(errors);
    }
    gs_leave_context();
    gs_destroy(graphics);
    return passed ? 0 : 1;
}
