The source files and headers in this directory are from
[dlbeer/quirc](https://github.com/dlbeer/quirc) commit
`927d680904dc95fdff4cd9d022eb374b438ff8f2` (ISC license, see `LICENSE`).

The repository's OpenCV copy of quirc omits `identify.c`, which provides image
detection and extraction. The desktop QR scanner compiles the complete pinned
upstream library here so the headers and implementation match.

SHA-256 of upstream `identify.c`:
`ae858d86adcb12db80ad01f6d941cc2247fb5970abf0754f24dca027ede2ba99`.
