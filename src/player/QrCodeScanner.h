#pragma once

#include "ffmpegInclude.h"
#include <QVariantList>
#include <memory>

class QrCodeScanner {
public:
    QrCodeScanner();
    ~QrCodeScanner();

    QVariantList scan(const std::shared_ptr<AVFrame> &frame);
    QVariantList scanRegion(const std::shared_ptr<AVFrame> &frame, int x, int y, int width, int height);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
