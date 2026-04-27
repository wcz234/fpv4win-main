#include "QrCodeScanner.h"

#include <opencv2/core.hpp>
#include <opencv2/objdetect.hpp>

#include <QVariantMap>
#include <algorithm>
#include <vector>

struct QrCodeScanner::Impl {
    cv::QRCodeDetector detector;
    SwsContext *convertCtx = nullptr;
    std::vector<uint8_t> grayBuffer;
};

QrCodeScanner::QrCodeScanner()
    : m_impl(std::make_unique<Impl>()) {
}

QrCodeScanner::~QrCodeScanner() {
    if (m_impl && m_impl->convertCtx) {
        sws_freeContext(m_impl->convertCtx);
        m_impl->convertCtx = nullptr;
    }
}

QVariantList QrCodeScanner::scan(const std::shared_ptr<AVFrame> &frame) {
    QVariantList results;
    if (!(frame && frame->data[0] && frame->width > 0 && frame->height > 0)) {
        return results;
    }

    auto &impl = *m_impl;
    impl.convertCtx = sws_getCachedContext(
        impl.convertCtx, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), frame->width,
        frame->height, AV_PIX_FMT_GRAY8, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!impl.convertCtx) {
        return results;
    }

    const int stride = frame->width;
    impl.grayBuffer.resize(static_cast<size_t>(stride) * static_cast<size_t>(frame->height));

    uint8_t *dstData[4] = { impl.grayBuffer.data(), nullptr, nullptr, nullptr };
    int dstLinesize[4] = { stride, 0, 0, 0 };
    if (sws_scale(impl.convertCtx, frame->data, frame->linesize, 0, frame->height, dstData, dstLinesize) <= 0) {
        return results;
    }

    cv::Mat gray(frame->height, frame->width, CV_8UC1, impl.grayBuffer.data(), stride);
    std::vector<cv::String> decodedInfo;
    cv::Mat points;
    if (!impl.detector.detectAndDecodeMulti(gray, decodedInfo, points) || decodedInfo.empty() || points.empty()) {
        return results;
    }

    cv::Mat floatPoints;
    if (points.depth() != CV_32F) {
        points.convertTo(floatPoints, CV_32F);
    } else {
        floatPoints = points;
    }
    if (!floatPoints.isContinuous()) {
        floatPoints = floatPoints.clone();
    }

    static constexpr int valuesPerQr = 8;
    const auto totalValues = static_cast<int>(floatPoints.total() * floatPoints.channels());
    const auto availableQrCount = totalValues / valuesPerQr;
    const auto qrCount = std::min(static_cast<int>(decodedInfo.size()), availableQrCount);
    const float *rawPoints = floatPoints.ptr<float>();
    if (!rawPoints) {
        return results;
    }

    for (int index = 0; index < qrCount; ++index) {
        const auto text = QString::fromUtf8(decodedInfo[index].c_str());
        if (text.isEmpty()) {
            continue;
        }

        const float *qrPoints = rawPoints + index * valuesPerQr;
        float minX = qrPoints[0];
        float maxX = qrPoints[0];
        float minY = qrPoints[1];
        float maxY = qrPoints[1];
        for (int i = 1; i < 4; ++i) {
            const float pointX = qrPoints[i * 2];
            const float pointY = qrPoints[i * 2 + 1];
            minX = std::min(minX, pointX);
            maxX = std::max(maxX, pointX);
            minY = std::min(minY, pointY);
            maxY = std::max(maxY, pointY);
        }

        minX = std::clamp(minX, 0.0f, static_cast<float>(frame->width));
        maxX = std::clamp(maxX, 0.0f, static_cast<float>(frame->width));
        minY = std::clamp(minY, 0.0f, static_cast<float>(frame->height));
        maxY = std::clamp(maxY, 0.0f, static_cast<float>(frame->height));

        const auto qrWidth = std::max(0.0f, maxX - minX);
        const auto qrHeight = std::max(0.0f, maxY - minY);
        QVariantList points;
        if (qrWidth > 0.0f && qrHeight > 0.0f) {
            for (int i = 0; i < 4; ++i) {
                const auto pointX = std::clamp(qrPoints[i * 2], 0.0f, static_cast<float>(frame->width));
                const auto pointY = std::clamp(qrPoints[i * 2 + 1], 0.0f, static_cast<float>(frame->height));

                QVariantMap point;
                point.insert("x", (pointX - minX) / qrWidth);
                point.insert("y", (pointY - minY) / qrHeight);
                points.push_back(point);
            }
        }

        QVariantMap qrItem;
        qrItem.insert("text", text);
        qrItem.insert("x", minX / static_cast<float>(frame->width));
        qrItem.insert("y", minY / static_cast<float>(frame->height));
        qrItem.insert("width", qrWidth / static_cast<float>(frame->width));
        qrItem.insert("height", qrHeight / static_cast<float>(frame->height));
        qrItem.insert("points", points);
        results.push_back(qrItem);
    }

    return results;
}
