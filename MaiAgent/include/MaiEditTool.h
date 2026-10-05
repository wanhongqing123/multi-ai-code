#pragma once

#include <memory>

#include "MaiTool.h"

// file_edit：把文件里的一段原文换成另一段。
//
// 和 file_write 的分工：file_write 是整份覆写，改一行要模型把整个文件重吐一遍，
// 又慢又贵，而且吐的过程中任何一处走样都会悄悄改掉别的地方。
std::unique_ptr<MaiTool> makeMaiEditTool();
