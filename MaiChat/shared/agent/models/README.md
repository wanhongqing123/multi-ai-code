# Desktop image vision models

These model weights are bundled so the desktop agent can run face detection and
person segmentation without downloading models at runtime. They are separate
from the OpenCV 4.14.0 source in `MaiAgent/third_party/opencv/`.

| File | Upstream | SHA-256 | License |
|---|---|---|---|
| `face_detection_yunet_2023mar.onnx` | [OpenCV Zoo YuNet](https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet) | `8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4` | MIT; `LICENSE.yunet` |
| `human_segmentation_pphumanseg_2023mar.onnx` | [OpenCV Zoo PPHumanSeg](https://github.com/opencv/opencv_zoo/tree/main/models/human_segmentation_pphumanseg) | `552d8a984054e59b5d773d24b9b12022b22046ceb2bbc4c9aaeaceb36a9ddf24` | Apache-2.0; `LICENSE.pphumanseg` |

The weights were downloaded from the OpenCV Zoo Git LFS media endpoint, not
the small Git LFS pointer files. YuNet 2023mar is the OpenCV 4.x-compatible
model; the newer 2026may model needs OpenCV 5's ONNX Runtime engine.

The desktop bridge in `../MaiImageVision.h` takes both model files as memory
buffers. The host should load them with its platform-aware file API, create one
vision handle, and reuse it for inference. This avoids repeated model loading
and allows model paths with non-ASCII characters on Windows. Inference is
serialized by the handle; the original image pixels are never modified.
