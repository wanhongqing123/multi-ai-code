#pragma once

#include <string>
#include <vector>

#include "MaiFaceBeautify.h"

// MaiAgent's shared face processor uses these in-process FFmpeg image codecs
// through its plain C++ callback interface. Every platform uses the same
// decoder, encoder, rotation, and ICC metadata handling.
bool maiDecodeFaceImage(const std::string &path, MaiFaceDecodedImage &image);
bool maiEncodeFacePng(const unsigned char *rgba, int width, int height, int stride,
                      const MaiFaceDecodedImage &source, std::vector<unsigned char> &png);
