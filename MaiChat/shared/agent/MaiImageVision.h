#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Desktop OpenCV image vision. Model buffers are borrowed during creation only;
// the handle keeps its own loaded networks. Pass the bundled YuNet 2023mar and
// PPHumanSeg 2023mar ONNX files. Loading model bytes in the host preserves
// Unicode file paths on Windows. A handle may be called from multiple threads;
// inference is serialized internally. Destroy it after all calls finish. On
// failure, *error is an owned UTF-8 string; release it with maiImageVisionFree.
void *maiImageVisionCreate(const unsigned char *faceModel, size_t faceModelSize,
                           const unsigned char *personModel,
                           size_t personModelSize, char **error);
void maiImageVisionDestroy(void *handle);

typedef struct MaiVisionFace {
  float x;
  float y;
  float width;
  float height;
  float landmarks[10]; // Right eye, left eye, nose, right mouth, left mouth (x,
                       // y pairs).
  float confidence;
} MaiVisionFace;

// RGBA8 input rows have at least width * 4 bytes. Coordinates are in
// input-image pixels. An empty face list is a successful result. On success
// error is null. The face array is owned by the caller and must be released
// with maiImageVisionFree.
typedef struct MaiVisionFacesResult {
  MaiVisionFace *faces;
  int count;
  char *error;
} MaiVisionFacesResult;
MaiVisionFacesResult maiImageVisionDetectFacesRgba(void *handle,
                                                   const unsigned char *pixels,
                                                   int width, int height,
                                                   int stride);

// Returns one grayscale byte per input pixel: 0 = background, 255 = person. On
// success mask is owned by the caller and error is null; release mask with
// maiImageVisionFree. Source pixels and any original image file are never
// modified.
typedef struct MaiVisionMaskResult {
  unsigned char *mask;
  int width;
  int height;
  char *error;
} MaiVisionMaskResult;
MaiVisionMaskResult maiImageVisionSegmentPersonRgba(void *handle,
                                                    const unsigned char *pixels,
                                                    int width, int height,
                                                    int stride);

void maiImageVisionFree(void *pointer);

#ifdef __cplusplus
}
#endif
