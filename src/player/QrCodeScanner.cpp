#include "QrCodeScanner.h"

#include <opencv2/core.hpp>
#include <opencv2/objdetect.hpp>

#include <QVariantMap>
#include <algorithm>
#include <vector>

namespace {

constexpr int kValuesPerQr = 8;
constexpr int kMaxResults = 12;

int appendDecodedResults(
    QVariantList &results,
    const std::vector<cv::String> &decodedInfo,
    const cv::Mat &points,
    int sourceWidth,
    int sourceHeight) {
    if (decodedInfo.empty() || points.empty() || results.size() >= kMaxResults) {
        return 0;
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

    const auto totalValues = static_cast<int>(floatPoints.total() * floatPoints.channels());
    const auto availableQrCount = totalValues / kValuesPerQr;
    const auto qrCount = std::min(static_cast<int>(decodedInfo.size()), availableQrCount);
    const float *rawPoints = floatPoints.ptr<float>();
    if (!rawPoints) {
        return 0;
    }

    int added = 0;
    for (int index = 0; index < qrCount && results.size() < kMaxResults; ++index) {
        const auto text = QString::fromUtf8(decodedInfo[index].c_str());
        if (text.isEmpty()) {
            continue;
        }

        float mappedPoints[kValuesPerQr] {};
        for (int i = 0; i < 4; ++i) {
            mappedPoints[i * 2] = std::clamp(
                rawPoints[index * kValuesPerQr + i * 2], 0.0f, static_cast<float>(sourceWidth));
            mappedPoints[i * 2 + 1] = std::clamp(
                rawPoints[index * kValuesPerQr + i * 2 + 1], 0.0f, static_cast<float>(sourceHeight));
        }

        float minX = mappedPoints[0];
        float maxX = mappedPoints[0];
        float minY = mappedPoints[1];
        float maxY = mappedPoints[1];
        for (int i = 1; i < 4; ++i) {
            minX = std::min(minX, mappedPoints[i * 2]);
            maxX = std::max(maxX, mappedPoints[i * 2]);
            minY = std::min(minY, mappedPoints[i * 2 + 1]);
            maxY = std::max(maxY, mappedPoints[i * 2 + 1]);
        }

        const auto qrWidth = std::max(0.0f, maxX - minX);
        const auto qrHeight = std::max(0.0f, maxY - minY);
        if (qrWidth < 4.0f || qrHeight < 4.0f) {
            continue;
        }

        QVariantList normalizedPoints;
        for (int i = 0; i < 4; ++i) {
            QVariantMap point;
            point.insert("x", (mappedPoints[i * 2] - minX) / qrWidth);
            point.insert("y", (mappedPoints[i * 2 + 1] - minY) / qrHeight);
            normalizedPoints.push_back(point);
        }

        QVariantMap qrItem;
        qrItem.insert("text", text);
        qrItem.insert("x", minX / static_cast<float>(sourceWidth));
        qrItem.insert("y", minY / static_cast<float>(sourceHeight));
        qrItem.insert("width", qrWidth / static_cast<float>(sourceWidth));
        qrItem.insert("height", qrHeight / static_cast<float>(sourceHeight));
        qrItem.insert("points", normalizedPoints);
        results.push_back(qrItem);
        ++added;
    }

    return added;
}

int detectQrCodes(cv::QRCodeDetector &detector, const cv::Mat &image, int sourceWidth, int sourceHeight, QVariantList &results) {
    std::vector<cv::String> decodedInfo;
    cv::Mat points;
    if (detector.detectAndDecodeMulti(image, decodedInfo, points)) {
        appendDecodedResults(results, decodedInfo, points, sourceWidth, sourceHeight);
    }

    if (results.isEmpty()) {
        decodedInfo.clear();
        points.release();
        const auto text = detector.detectAndDecode(image, points);
        if (!text.empty() && !points.empty()) {
            decodedInfo.push_back(text);
            appendDecodedResults(results, decodedInfo, points, sourceWidth, sourceHeight);
        }
    }

    return results.size();
}

} // namespace

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
    detectQrCodes(impl.detector, gray, frame->width, frame->height, results);

    return results;
}
