#pragma once

#include "ffmpegInclude.h"
#include <QVariantList>
#include <memory>

class QrCodeScanner {
public:
    QrCodeScanner();
    ~QrCodeScanner();

    QVariantList scan(const std::shared_ptr<AVFrame> &frame);
    QVariantList scanAdaptive(const std::shared_ptr<AVFrame> &frame, const QVariantList &trackedCodes, bool fullScan);
    QVariantList scanProfile(const std::shared_ptr<AVFrame> &frame, int profileIndex);
    QVariantList scanRegion(const std::shared_ptr<AVFrame> &frame, int x, int y, int width, int height);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
