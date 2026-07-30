#include "remote/RemoteKeyMapping.h"

#include <QtCore/qnamespace.h>

namespace RemoteInput {
namespace {

constexpr quint32 kVkBack = 0x08;
constexpr quint32 kVkTab = 0x09;
constexpr quint32 kVkClear = 0x0c;
constexpr quint32 kVkReturn = 0x0d;
constexpr quint32 kVkShift = 0x10;
constexpr quint32 kVkControl = 0x11;
constexpr quint32 kVkMenu = 0x12;
constexpr quint32 kVkPause = 0x13;
constexpr quint32 kVkCapital = 0x14;
constexpr quint32 kVkEscape = 0x1b;
constexpr quint32 kVkSpace = 0x20;
constexpr quint32 kVkPrior = 0x21;
constexpr quint32 kVkNext = 0x22;
constexpr quint32 kVkEnd = 0x23;
constexpr quint32 kVkHome = 0x24;
constexpr quint32 kVkLeft = 0x25;
constexpr quint32 kVkUp = 0x26;
constexpr quint32 kVkRight = 0x27;
constexpr quint32 kVkDown = 0x28;
constexpr quint32 kVkSnapshot = 0x2c;
constexpr quint32 kVkInsert = 0x2d;
constexpr quint32 kVkDelete = 0x2e;
constexpr quint32 kVkHelp = 0x2f;
constexpr quint32 kVkLWin = 0x5b;
constexpr quint32 kVkRWin = 0x5c;
constexpr quint32 kVkApps = 0x5d;
constexpr quint32 kVkNumpad0 = 0x60;
constexpr quint32 kVkMultiply = 0x6a;
constexpr quint32 kVkAdd = 0x6b;
constexpr quint32 kVkSubtract = 0x6d;
constexpr quint32 kVkDecimal = 0x6e;
constexpr quint32 kVkDivide = 0x6f;
constexpr quint32 kVkF1 = 0x70;
constexpr quint32 kVkNumLock = 0x90;
constexpr quint32 kVkScroll = 0x91;
constexpr quint32 kVkLShift = 0xa0;
constexpr quint32 kVkRShift = 0xa1;
constexpr quint32 kVkLControl = 0xa2;
constexpr quint32 kVkRControl = 0xa3;
constexpr quint32 kVkLMenu = 0xa4;
constexpr quint32 kVkRMenu = 0xa5;
constexpr quint32 kVkVolumeMute = 0xad;
constexpr quint32 kVkVolumeDown = 0xae;
constexpr quint32 kVkVolumeUp = 0xaf;
constexpr quint32 kVkMediaNextTrack = 0xb0;
constexpr quint32 kVkMediaPrevTrack = 0xb1;
constexpr quint32 kVkMediaStop = 0xb2;
constexpr quint32 kVkMediaPlayPause = 0xb3;
constexpr quint32 kVkOem1 = 0xba;
constexpr quint32 kVkOemPlus = 0xbb;
constexpr quint32 kVkOemComma = 0xbc;
constexpr quint32 kVkOemMinus = 0xbd;
constexpr quint32 kVkOemPeriod = 0xbe;
constexpr quint32 kVkOem2 = 0xbf;
constexpr quint32 kVkOem3 = 0xc0;
constexpr quint32 kVkOem4 = 0xdb;
constexpr quint32 kVkOem5 = 0xdc;
constexpr quint32 kVkOem6 = 0xdd;
constexpr quint32 kVkOem7 = 0xde;

}  // namespace

quint32 canonicalKeyCodeFromQt(int qtKey, bool keypad) {
    if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z) {
        return static_cast<quint32>('A' + qtKey - Qt::Key_A);
    }
    if (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9) {
        return keypad ? kVkNumpad0 + static_cast<quint32>(qtKey - Qt::Key_0)
                      : static_cast<quint32>('0' + qtKey - Qt::Key_0);
    }
    if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F24) {
        return kVkF1 + static_cast<quint32>(qtKey - Qt::Key_F1);
    }

    switch (qtKey) {
        case Qt::Key_Backspace: return kVkBack;
        case Qt::Key_Tab:
        case Qt::Key_Backtab: return kVkTab;
        case Qt::Key_Clear: return kVkClear;
        case Qt::Key_Return:
        case Qt::Key_Enter: return kVkReturn;
        case Qt::Key_Shift: return kVkShift;
        case Qt::Key_Control: return kVkControl;
        case Qt::Key_Alt: return kVkMenu;
        case Qt::Key_AltGr: return kVkRMenu;
        case Qt::Key_Pause: return kVkPause;
        case Qt::Key_CapsLock: return kVkCapital;
        case Qt::Key_Escape: return kVkEscape;
        case Qt::Key_Space: return kVkSpace;
        case Qt::Key_PageUp: return kVkPrior;
        case Qt::Key_PageDown: return kVkNext;
        case Qt::Key_End: return kVkEnd;
        case Qt::Key_Home: return kVkHome;
        case Qt::Key_Left: return kVkLeft;
        case Qt::Key_Up: return kVkUp;
        case Qt::Key_Right: return kVkRight;
        case Qt::Key_Down: return kVkDown;
        case Qt::Key_Print: return kVkSnapshot;
        case Qt::Key_Insert: return kVkInsert;
        case Qt::Key_Delete: return kVkDelete;
        case Qt::Key_Help: return kVkHelp;
        case Qt::Key_Meta:
        case Qt::Key_Super_L: return kVkLWin;
        case Qt::Key_Super_R: return kVkRWin;
        case Qt::Key_Menu: return kVkApps;
        case Qt::Key_NumLock: return kVkNumLock;
        case Qt::Key_ScrollLock: return kVkScroll;
        case Qt::Key_Asterisk: return keypad ? kVkMultiply : static_cast<quint32>('8');
        case Qt::Key_Plus: return keypad ? kVkAdd : kVkOemPlus;
        case Qt::Key_Minus: return keypad ? kVkSubtract : kVkOemMinus;
        case Qt::Key_Period: return keypad ? kVkDecimal : kVkOemPeriod;
        case Qt::Key_Slash: return keypad ? kVkDivide : kVkOem2;
        case Qt::Key_Semicolon:
        case Qt::Key_Colon: return kVkOem1;
        case Qt::Key_Equal: return kVkOemPlus;
        case Qt::Key_Comma:
        case Qt::Key_Less: return kVkOemComma;
        case Qt::Key_Underscore: return kVkOemMinus;
        case Qt::Key_Greater: return kVkOemPeriod;
        case Qt::Key_Question: return kVkOem2;
        case Qt::Key_QuoteLeft:
        case Qt::Key_AsciiTilde: return kVkOem3;
        case Qt::Key_BracketLeft:
        case Qt::Key_BraceLeft: return kVkOem4;
        case Qt::Key_Backslash:
        case Qt::Key_Bar: return kVkOem5;
        case Qt::Key_BracketRight:
        case Qt::Key_BraceRight: return kVkOem6;
        case Qt::Key_Apostrophe:
        case Qt::Key_QuoteDbl: return kVkOem7;
        case Qt::Key_VolumeMute: return kVkVolumeMute;
        case Qt::Key_VolumeDown: return kVkVolumeDown;
        case Qt::Key_VolumeUp: return kVkVolumeUp;
        case Qt::Key_MediaNext: return kVkMediaNextTrack;
        case Qt::Key_MediaPrevious: return kVkMediaPrevTrack;
        case Qt::Key_MediaStop: return kVkMediaStop;
        case Qt::Key_MediaTogglePlayPause:
        case Qt::Key_MediaPlay:
        case Qt::Key_MediaPause: return kVkMediaPlayPause;
        default: return 0;
    }
}

int macKeyCodeFromCanonical(quint32 keyCode) {
    // CGKeyCode 的数值是 Apple 虚拟键盘的物理位置，不随当前输入法变化。
    switch (keyCode) {
        case 'A': return 0;
        case 'S': return 1;
        case 'D': return 2;
        case 'F': return 3;
        case 'H': return 4;
        case 'G': return 5;
        case 'Z': return 6;
        case 'X': return 7;
        case 'C': return 8;
        case 'V': return 9;
        case 'B': return 11;
        case 'Q': return 12;
        case 'W': return 13;
        case 'E': return 14;
        case 'R': return 15;
        case 'Y': return 16;
        case 'T': return 17;
        case '1': return 18;
        case '2': return 19;
        case '3': return 20;
        case '4': return 21;
        case '6': return 22;
        case '5': return 23;
        case '9': return 25;
        case '7': return 26;
        case '8': return 28;
        case '0': return 29;
        case 'O': return 31;
        case 'U': return 32;
        case 'I': return 34;
        case 'P': return 35;
        case 'L': return 37;
        case 'J': return 38;
        case 'K': return 40;
        case 'N': return 45;
        case 'M': return 46;
        case kVkOemPlus: return 24;
        case kVkOem6: return 30;
        case kVkOem4: return 33;
        case kVkOem7: return 39;
        case kVkOem1: return 41;
        case kVkOem5: return 42;
        case kVkOemComma: return 43;
        case kVkOem2: return 44;
        case kVkOemPeriod: return 47;
        case kVkOemMinus: return 27;
        case kVkOem3: return 50;
        case kVkReturn: return 36;
        case kVkTab: return 48;
        case kVkSpace: return 49;
        case kVkBack: return 51;
        case kVkEscape: return 53;
        case kVkLWin: return 55;
        case kVkRWin: return 54;
        case kVkShift:
        case kVkLShift: return 56;
        case kVkRShift: return 60;
        case kVkCapital: return 57;
        case kVkMenu:
        case kVkLMenu: return 58;
        case kVkRMenu: return 61;
        case kVkControl:
        case kVkLControl: return 59;
        case kVkRControl: return 62;
        case kVkDecimal: return 65;
        case kVkMultiply: return 67;
        case kVkAdd: return 69;
        case kVkClear:
        case kVkNumLock: return 71;
        case kVkDivide: return 75;
        case kVkSubtract: return 78;
        case kVkNumpad0: return 82;
        case kVkNumpad0 + 1: return 83;
        case kVkNumpad0 + 2: return 84;
        case kVkNumpad0 + 3: return 85;
        case kVkNumpad0 + 4: return 86;
        case kVkNumpad0 + 5: return 87;
        case kVkNumpad0 + 6: return 88;
        case kVkNumpad0 + 7: return 89;
        case kVkNumpad0 + 8: return 91;
        case kVkNumpad0 + 9: return 92;
        case kVkF1: return 122;
        case kVkF1 + 1: return 120;
        case kVkF1 + 2: return 99;
        case kVkF1 + 3: return 118;
        case kVkF1 + 4: return 96;
        case kVkF1 + 5: return 97;
        case kVkF1 + 6: return 98;
        case kVkF1 + 7: return 100;
        case kVkF1 + 8: return 101;
        case kVkF1 + 9: return 109;
        case kVkF1 + 10: return 103;
        case kVkF1 + 11: return 111;
        case kVkF1 + 12: return 105;
        case kVkF1 + 13: return 107;
        case kVkF1 + 14: return 113;
        case kVkF1 + 15: return 106;
        case kVkHelp:
        case kVkInsert: return 114;
        case kVkHome: return 115;
        case kVkPrior: return 116;
        case kVkDelete: return 117;
        case kVkEnd: return 119;
        case kVkNext: return 121;
        case kVkLeft: return 123;
        case kVkRight: return 124;
        case kVkDown: return 125;
        case kVkUp: return 126;
        default: return -1;
    }
}

}  // namespace RemoteInput
