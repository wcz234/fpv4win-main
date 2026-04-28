#include "QrCodeScanner.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect.hpp>

#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr int kValuesPerQr = 8;
constexpr int kMaxResults = 12;

struct ScanImage {
    cv::Mat image;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    int offsetX = 0;
    int offsetY = 0;
};

bool isDuplicateResult(const QVariantList &results, float minX, float minY, float width, float height) {
    const float centerX = minX + width * 0.5f;
    const float centerY = minY + height * 0.5f;

    for (const auto &item : results) {
        const auto map = item.toMap();
        const float oldX = map.value("x").toFloat();
        const float oldY = map.value("y").toFloat();
        const float oldW = map.value("width").toFloat();
        const float oldH = map.value("height").toFloat();
        const float oldCenterX = oldX + oldW * 0.5f;
        const float oldCenterY = oldY + oldH * 0.5f;
        const float centerDistance = std::hypot(centerX - oldCenterX, centerY - oldCenterY);
        if (centerDistance < 0.025f && std::abs(width - oldW) < 0.06f && std::abs(height - oldH) < 0.06f) {
            return true;
        }
    }

    return false;
}

int appendDecodedResults(
    QVariantList &results,
    const std::vector<cv::String> &decodedInfo,
    const cv::Mat &points,
    int sourceWidth,
    int sourceHeight,
    int offsetX,
    int offsetY,
    float scaleX,
    float scaleY) {
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
                rawPoints[index * kValuesPerQr + i * 2] / scaleX + static_cast<float>(offsetX), 0.0f,
                static_cast<float>(sourceWidth));
            mappedPoints[i * 2 + 1] = std::clamp(
                rawPoints[index * kValuesPerQr + i * 2 + 1] / scaleY + static_cast<float>(offsetY), 0.0f,
                static_cast<float>(sourceHeight));
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

        const float normalizedX = minX / static_cast<float>(sourceWidth);
        const float normalizedY = minY / static_cast<float>(sourceHeight);
        const float normalizedW = qrWidth / static_cast<float>(sourceWidth);
        const float normalizedH = qrHeight / static_cast<float>(sourceHeight);
        if (isDuplicateResult(results, normalizedX, normalizedY, normalizedW, normalizedH)) {
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
        qrItem.insert("x", normalizedX);
        qrItem.insert("y", normalizedY);
        qrItem.insert("width", normalizedW);
        qrItem.insert("height", normalizedH);
        qrItem.insert("points", normalizedPoints);
        results.push_back(qrItem);
        ++added;
    }

    return added;
}

int detectQrCodes(
    cv::QRCodeDetector &detector,
    const ScanImage &scanImage,
    int sourceWidth,
    int sourceHeight,
    QVariantList &results) {
    std::vector<cv::String> decodedInfo;
    cv::Mat points;
    if (detector.detectAndDecodeMulti(scanImage.image, decodedInfo, points)) {
        appendDecodedResults(
            results, decodedInfo, points, sourceWidth, sourceHeight, scanImage.offsetX, scanImage.offsetY,
            scanImage.scaleX, scanImage.scaleY);
    }

    decodedInfo.clear();
    points.release();
    if (detector.detectMulti(scanImage.image, points) && detector.decodeMulti(scanImage.image, points, decodedInfo)) {
        appendDecodedResults(
            results, decodedInfo, points, sourceWidth, sourceHeight, scanImage.offsetX, scanImage.offsetY,
            scanImage.scaleX, scanImage.scaleY);
    }

    decodedInfo.clear();
    points.release();
    const auto text = detector.detectAndDecode(scanImage.image, points);
    if (!text.empty() && !points.empty()) {
        decodedInfo.push_back(text);
        appendDecodedResults(
            results, decodedInfo, points, sourceWidth, sourceHeight, scanImage.offsetX, scanImage.offsetY,
            scanImage.scaleX, scanImage.scaleY);
    }

    return results.size();
}

ScanImage makeScaledImage(const cv::Mat &image, double scale) {
    if (std::abs(scale - 1.0) < 0.01) {
        return { image, 1.0f, 1.0f, 0, 0 };
    }

    cv::Mat resized;
    const auto interpolation = scale > 1.0 ? cv::INTER_CUBIC : cv::INTER_AREA;
    cv::resize(image, resized, cv::Size(), scale, scale, interpolation);
    return { resized, static_cast<float>(scale), static_cast<float>(scale), 0, 0 };
}

void appendProfileImages(const cv::Mat &gray, int profileIndex, std::vector<ScanImage> &images) {
    switch (profileIndex % 4) {
    case 0:
        images.push_back(makeScaledImage(gray, 1.0));
        break;
    case 1:
        images.push_back(makeScaledImage(gray, 1.6));
        break;
    case 2:
        images.push_back(makeScaledImage(gray, 0.7));
        break;
    default: {
        cv::Mat equalized;
        cv::equalizeHist(gray, equalized);
        images.push_back(makeScaledImage(equalized, 1.0));

        cv::Mat thresholded;
        cv::adaptiveThreshold(
            equalized, thresholded, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C, cv::THRESH_BINARY, 31, 3);
        images.push_back(makeScaledImage(thresholded, 1.0));
        break;
    }
    }
}

} // namespace

struct QrCodeScanner::Impl {
    cv::QRCodeDetector detector;
    SwsContext *convertCtx = nullptr;
    std::vector<uint8_t> grayBuffer;
};

QrCodeScanner::QrCodeScanner()
    : m_impl(std::make_unique<Impl>()) {
    m_impl->detector.setEpsX(0.35);
    m_impl->detector.setEpsY(0.35);
}

QrCodeScanner::~QrCodeScanner() {
    if (m_impl && m_impl->convertCtx) {
        sws_freeContext(m_impl->convertCtx);
        m_impl->convertCtx = nullptr;
    }
}

QVariantList QrCodeScanner::scan(const std::shared_ptr<AVFrame> &frame) {
    if (!(frame && frame->data[0])) {
        return {};
    }

    return scanProfile(frame, 0);
}

QVariantList QrCodeScanner::scanProfile(const std::shared_ptr<AVFrame> &frame, int profileIndex) {
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
    std::vector<ScanImage> images;
    appendProfileImages(gray, profileIndex, images);
    for (const auto &image : images) {
        detectQrCodes(impl.detector, image, frame->width, frame->height, results);
    }

    return results;
}

QVariantList QrCodeScanner::scanRegion(const std::shared_ptr<AVFrame> &frame, int x, int y, int width, int height) {
    QVariantList results;
    if (!(frame && frame->data[0] && frame->width > 0 && frame->height > 0)) {
        return results;
    }

    const int left = std::clamp(x, 0, frame->width);
    const int top = std::clamp(y, 0, frame->height);
    const int right = std::clamp(x + width, left, frame->width);
    const int bottom = std::clamp(y + height, top, frame->height);
    const int regionWidth = right - left;
    const int regionHeight = bottom - top;
    if (regionWidth <= 0 || regionHeight <= 0) {
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
    const cv::Rect roi(left, top, regionWidth, regionHeight);
    const ScanImage scanImage { gray(roi), 1.0f, 1.0f, left, top };
    detectQrCodes(impl.detector, scanImage, frame->width, frame->height, results);

    return results;
}
