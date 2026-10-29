# qpdf embedded PDF encryption

- Upstream: https://github.com/qpdf/qpdf/tree/v12.2.0
- Source archive: https://codeload.github.com/qpdf/qpdf/tar.gz/refs/tags/v12.2.0
- SHA-256: `2d004611699a94030a594318393f08a7352ab1cf3cee337c5312d4e7eb7f8a4f`
- License: Apache-2.0, see `LICENSE.txt` and `NOTICE.md`; complete tagged source is vendored for offline builds.

## Version and toolchain

12.2.0 is the final C++17 minor release documented by upstream; 12.3.0 changes to C++20.
MaiChat still supports VS2019/MSVC 19.29 and C++17 mobile builds, so the product pins 12.2.0.
Compared with the initially evaluated 11.9.1, this includes Android build fixes, shared
`/Kids` page-tree stack-overflow detection and damaged-xref resource-use guards. See
`manual/release-notes.rst`. Later upstream hardening is not included by this version pin;
this feature primarily encrypts PDFs produced by the existing in-process native renderers,
and the ordinary API limits input/output to 100 MiB. This is not a claim that arbitrary
untrusted PDF parsing is risk-free. Revisit the pin when the Windows C++20 baseline changes.

## Local build-only patch

`libqpdf/CMakeLists.txt` accepts `MAI_QPDF_EMBEDDED_DEPENDENCIES` and explicitly supplied
include paths/targets. This bypasses host-machine `pkg-config` and library probing during
cross compilation. It also marks the explicitly supplied OpenSSL provider as found.
No qpdf parser, password handling or cryptographic source is modified.

The same embedding flag configures only `include` and `libqpdf`: CLI executables,
upstream test programs, examples, fuzz programs, CPack and installation/export rules
are skipped. This avoids iOS inheriting `MACOSX_BUNDLE` for an unneeded CLI install
and prevents Android AGP's default native build from compiling upstream test drivers.
It does not change platform bundling behavior or remove files from the vendored source.

`../MaiPdfDependencies.cmake` selects static libqpdf, OpenSSL only, and the repository's
private prefixed zlib. JPEG reuses the product's existing OpenCV `libjpeg-turbo` target;
standalone MaiAgent uses the same vendored 3.1.2 C sources as an excluded fallback target.
Only the selected JPEG archive is linked. No runtime executable, download or GnuTLS library
is introduced. iOS archive staging excludes stale standalone fallback archives when the
OpenCV JPEG archive exists.

## Embedded execution boundary

MaiPdfDocumentTools calls upstream `QPDFJob` configuration/create/write functions directly.
It never calls CLI `main`, `exit`, or a shell command. Each invocation owns a fresh job and
private discard logger. A private save pipeline writes through an exclusively created file
handle; upstream output `-` is redirected to this pipeline rather than process stdout.
Cancellation is checked before/after parsing and from progress/output callbacks. Errors are
mapped to fixed credential-free messages; `exception.what()` and qpdf logs are not returned.

AES-256 standard security handler R6 is provided by qpdf/OpenSSL. A nonempty UTF-8 user
password opens the document; a random per-file owner password prevents accidental empty
owner credentials. The user is not asked to manage owner/permission passwords. Rendering,
private password entry and atomic file publication remain application responsibilities.
Correct/incorrect/empty-password behavior was independently checked with Poppler, including
successful text extraction only with the correct password; product code does not call it.
