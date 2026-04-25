#pragma once

#include "ffmpegInclude.h"
#include <QVariantList>
#include <memory>

class QrCodeScanner {
public:
    QrCodeScanner();
    ~QrCodeScanner();

    QVariantList scan(const std::shared_ptr<AVFrame> &frame);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
