#pragma once

// OBS's gs_* interface is the Graphics API exported by MaiAgent. Link the
// maiagent_graphics target and create a backend with gs_create before using
// device resources. All gs_* resource calls require gs_enter_context on the
// calling thread and must be balanced with gs_leave_context. Resource handles
// become invalid after gs_destroy. A platform backend must be available and
// passed to gs_create; the header alone does not provide one.
#include "graphics.h"
