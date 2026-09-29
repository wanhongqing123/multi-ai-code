#include "../graphics.h"

int main(void) {
    graphics_t* graphics = NULL;
    const int result = gs_create(&graphics, "/no-such-maiagent-graphics-backend", 0);
    return result == GS_ERROR_MODULE_NOT_FOUND && graphics == NULL ? 0 : 1;
}
