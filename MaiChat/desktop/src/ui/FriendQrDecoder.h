#pragma once

#include <QString>

class QImage;

// Decode one MaiChat friend QR from a local image; returns an empty ID for other QR content.
QString maiChatAccountIdFromQrImage(const QImage& image);
