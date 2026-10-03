# Face landmarker models

These two ONNX files are the BlazeFace short-range detector and the 478-point
Face Landmarker converted from the Google MediaPipe models by
[yakhyo/mediapipe-face-mesh-onnx](https://github.com/yakhyo/mediapipe-face-mesh-onnx)
at commit `add50e0f486405c96695812f9a9b9b89a485892b`. The conversion source
and its original-model attribution are in that repository. Its license is
Apache-2.0; see `LICENSE` here. Both files are used together, in process, by
MaiAgent's shared image beautification implementation. No per-platform model
selection is permitted.

| File | SHA-256 |
|---|---|
| `face_detection_short_range.onnx` | `2f2689b040becf555706d2cb978d2f0e3296ea82413734fba9a856c66c5f2b17` |
| `face_landmarker_Nx3x256x256.onnx` | `111795f8703cdeb6d0c68a9f3cc966a0f23f8786bb00f4577a11f461fc4276ac` |

The ONNX interface was checked with ONNX Runtime 1.26.0: the detector takes
`input` float32 `[N,3,128,128]` and returns `regressors` and `scores`; the
landmarker takes `input` float32 `[N,3,256,256]` and returns `landmarks`
`[N,478,3]` and `score` `[N,1]`. The upstream README documents the detector
anchor decode, ROI rotation and landmark coordinates used by our C++ code.
