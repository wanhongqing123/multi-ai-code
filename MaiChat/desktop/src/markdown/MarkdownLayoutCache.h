#pragma once

#include <QString>
#include <memory>

#include "markdown/MarkdownLayout.h"

// 排好的版，按内容和宽度留着。
//
// ── 为什么要有它 ────────────────────────────────────────────────
//
// 排版是整条链路上最贵的一步，而且贵得不讲道理：一条几百字的消息，解析只要
// 0.01ms，排版要 1.3ms——**一百三十倍**。开销全在 QTextLayout 的断行和字形排布
// 上，那是 Qt 的字体引擎在做事，绕不过去。
//
// 于是切会话就很难受：IM 切一次会话会把整列消息拆掉重建，每条都要重排一遍，
// 二百条就是三百多毫秒，上千条就是「卡死」。可这些消息**一个字都没变**，
// 宽度也没变，上一次排好的结果本来还能用，只是随着部件一起被销毁了。
//
// 这里就把那份结果留下来。切回去的时候直接取，不用再排。
//
// ── 键是什么 ────────────────────────────────────────────────────
//
// 内容、末尾附注、宽度、皮肤，四样任意一个不同都得重排。皮肤那一项用
// MarkdownTheme::zoom——产品里所有主题都出自 MarkdownTheme::standard(zoom)，
// 所以那个数就能把两份皮肤分开（见那边的注释）。
//
// 哈希会撞，所以命中之后**还要把键逐项比一遍**，不能只信哈希——撞上一次就是
// 把别人的消息画到这条上，那种 bug 找起来要命。
//
// 线程：只在界面线程用。
class MarkdownLayoutCache {
public:
    // 取走一份。没有就返回空。**是取走不是借**：MarkdownLayout 只能移动，
    // 而且取走之后调用方会往里写选区，留一份在这儿给别人用是错的。
    static std::unique_ptr<MarkdownLayout> take(const QString& source, const QString& note,
                                                int notePixelSize, unsigned int noteRgba,
                                                qreal width, qreal zoom);

    // 还回来。用完就还，下次切回来还能用上。空指针会被忽略。
    static void put(const QString& source, const QString& note, int notePixelSize,
                    unsigned int noteRgba, qreal width, qreal zoom,
                    std::unique_ptr<MarkdownLayout> layout);

    // 清空。换皮肤之类的场合用不上（键里有皮肤），主要是给测试用。
    static void clear();

    // 现在存了几份。测试用。
    static int count();

    // 命中 / 未命中的累计次数。给探针看命中率用——命中率上不去就说明键没对上，
    // 那是要查的，不是调大上限能解决的。
    static int hits();
    static int misses();
};
