#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The presenter owns one Graphics thread and one image-decoding thread for the
// process. Configure it once before attaching views. backend is a module path
// on desktop/Android and "builtin:metal" on iOS. effect_directory contains the
// bundled default.effect and its relative includes. Reconfiguration requires
// maiGraphicsPresenterStop after all views have been detached.
//
// The callback runs on the Graphics thread. The application must dispatch UI
// updates to its UI thread. A failed render leaves the previous frame visible.
typedef void (*MaiGraphicsPresentCallback)(uint64_t view_id, bool success, void* user_data);
typedef void (*MaiGraphicsNativeViewCallback)(void* native_view);
bool maiGraphicsPresenterStart(const char* backend, const char* effect_directory,
                               MaiGraphicsPresentCallback callback, void* user_data);

// native_view is a CAMetalLayer installed by the UI thread on Apple, an
// ANativeWindow on Android, and an HWND on Windows. retain_view runs before
// the request is
// queued; release_view runs after the final Graphics task releases the handle.
// Pass both callbacks when the platform handle is reference counted, or both
// NULL when the application keeps it alive until detach completes. Attach and
// rendering are asynchronous; a nonzero ID only
// means the request was accepted. The callback reports the result of rendering.
uint64_t maiGraphicsPresenterAttach(void* native_view, uint32_t width, uint32_t height,
                                    MaiGraphicsNativeViewCallback retain_view,
                                    MaiGraphicsNativeViewCallback release_view);
bool maiGraphicsPresenterShowImage(uint64_t view_id, const char* file_path, bool fill_view);
// Copies one decoded RGBA8 video frame into the Graphics queue. The caller may
// reuse pixels after this call. stride must be at least width * 4. Frame
// scheduling and audio synchronization belong to the media session; submitting
// a newer frame supersedes any older frame still waiting in the queue.
bool maiGraphicsPresenterShowFrame(uint64_t view_id, const uint8_t* pixels, uint32_t width,
                                   uint32_t height, uint32_t stride, bool fill_view);
void maiGraphicsPresenterResize(uint64_t view_id, uint32_t width, uint32_t height);
void maiGraphicsPresenterDetach(uint64_t view_id);

// Stop must run while the application's UI loop can still process view work.
// It waits for both workers and invalidates all view IDs.
void maiGraphicsPresenterStop(void);

#ifdef __cplusplus
}
#endif
