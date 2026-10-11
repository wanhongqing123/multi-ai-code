# LaMa ONNX model provenance

The optional `lama_fp32.onnx` file is downloaded by `MaiAgent/scripts/fetch-lama-model.sh` from
the [Carve LaMa-ONNX release](https://huggingface.co/Carve/LaMa-ONNX), which ports the
[LaMa inpainting model](https://github.com/advimman/lama). The expected SHA-256 is
`1faef5301d78db7dda502fe59966957ec4b79dd64e16f03ed96913c7a4eb68d6`.

The source project's Apache 2.0 license is copied into `LaMa_LICENSE` and bundled with the
model. The model file itself stays outside Git and is required only for builds that expose
the local Agent watermark tool.
