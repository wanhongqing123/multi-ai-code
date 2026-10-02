# Video matting model

`rvm_mobilenetv3_fp32.onnx` is the official Robust Video Matting MobileNetV3
ONNX release (`v1.0.0`, opset 12):

<https://github.com/PeterL1n/RobustVideoMatting/releases/download/v1.0.0/rvm_mobilenetv3_fp32.onnx>

SHA-256: `88d4531297118f595bf2fd60f6f566aec2e559393802d1f436c380f0cbbd2828`

The model uses six inputs (`src`, `r1i`–`r4i`, `downsample_ratio`) and six outputs
(`fgr`, `pha`, `r1o`–`r4o`). Recurrent outputs must feed the next frame in display
order. The upstream repository distributes this model under GPL-3.0; the full
license is in `RVM_LICENSE`.
