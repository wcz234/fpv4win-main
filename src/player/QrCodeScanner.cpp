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
constexpr int kQuickMaxSide = 1280;
constexpr int kCloseMaxSide = 960;
constexpr int kMaxResults = 12;
constexpr int kMinCloseBorder = 32;

bool isDuplicateResult(const QVariantList &results, const QString &text, float minX, float minY, float width, float height) {
    const float cx = minX + width * 0.5f;
    const float cy = minY + height * 0.5f;

    for (const auto &item : results) {
        const auto map = item.toMap();
        if (!text.isEmpty() && map.value("text").toString() == text) {
            return true;
        }

        const float oldX = map.value("x").toFloat();
        const float oldY = map.value("y").toFloat();
        const float oldW = map.value("width").toFloat();
        const float oldH = map.value("height").toFloat();
        const float oldCx = oldX + oldW * 0.5f;
        const float oldCy = oldY + oldH * 0.5f;
        const float centerDistance = std::hypot(cx - oldCx, cy - oldCy);
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
    double scaleX,
    double scaleY,
    double offsetX = 0.0,
    double offsetY = 0.0) {
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
                static_cast<float>((rawPoints[index * kValuesPerQr + i * 2] - offsetX) * scaleX), 0.0f,
                static_cast<float>(sourceWidth));
            mappedPoints[i * 2 + 1] = std::clamp(
                static_cast<float>((rawPoints[index * kValuesPerQr + i * 2 + 1] - offsetY) * scaleY), 0.0f,
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

        const auto normalizedX = minX / static_cast<float>(sourceWidth);
        const auto normalizedY = minY / static_cast<float>(sourceHeight);
        const auto normalizedW = qrWidth / static_cast<float>(sourceWidth);
        const auto normalizedH = qrHeight / static_cast<float>(sourceHeight);
        if (isDuplicateResult(results, text, normalizedX, normalizedY, normalizedW, normalizedH)) {
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

int tryDecode(
    cv::QRCodeDetector &detector,
    const cv::Mat &image,
    int sourceWidth,
    int sourceHeight,
    double scaleX,
    double scaleY,
    QVariantList &results,
    double offsetX = 0.0,
    double offsetY = 0.0) {
    if (image.empty() || results.size() >= kMaxResults) {
        return 0;
    }

    const auto before = results.size();
    std::vector<cv::String> decodedInfo;
    cv::Mat points;
    if (detector.detectAndDecodeMulti(image, decodedInfo, points)) {
        appendDecodedResults(results, decodedInfo, points, sourceWidth, sourceHeight, scaleX, scaleY, offsetX, offsetY);
    }

    if (results.size() == before) {
        decodedInfo.clear();
        points.release();
        const auto text = detector.detectAndDecode(image, points);
        if (!text.empty() && !points.empty()) {
            decodedInfo.push_back(text);
            appendDecodedResults(
                results, decodedInfo, points, sourceWidth, sourceHeight, scaleX, scaleY, offsetX, offsetY);
        }
    }

    return results.size() - before;
}

cv::Mat resizeToMaxSide(const cv::Mat &source, int maxSide, double &imageScale) {
    const int maxInputSide = std::max(source.cols, source.rows);
    if (maxInputSide <= maxSide) {
        imageScale = 1.0;
        return source;
    }

    imageScale = static_cast<double>(maxSide) / static_cast<double>(maxInputSide);
    cv::Mat resized;
    cv::resize(source, resized, cv::Size(), imageScale, imageScale, cv::INTER_AREA);
    return resized;
}

cv::Mat upscaleForSmallQr(const cv::Mat &source, double &imageScale) {
    const int maxInputSide = std::max(source.cols, source.rows);
    imageScale = maxInputSide <= 960 ? 2.0 : 1.5;
    cv::Mat resized;
    cv::resize(source, resized, cv::Size(), imageScale, imageScale, cv::INTER_CUBIC);
    return resized;
}

cv::Mat addWhiteBorder(const cv::Mat &source, int &borderX, int &borderY) {
    const int maxInputSide = std::max(source.cols, source.rows);
    const int border = std::max(kMinCloseBorder, maxInputSide / 10);
    borderX = border;
    borderY = border;

    cv::Mat padded;
    const cv::Scalar white = source.channels() == 1 ? cv::Scalar(255) : cv::Scalar(255, 255, 255);
    cv::copyMakeBorder(source, padded, borderY, borderY, borderX, borderX, cv::BORDER_CONSTANT, white);
    return padded;
}

} // namespace

struct QrCodeScanner::Impl {
    cv::QRCodeDetector detector;
    SwsContext *convertCtx = nullptr;
    std::vector<uint8_t> bgrBuffer;
    cv::Mat quickBgr;
    cv::Mat closeBgr;
    cv::Mat borderedBgr;
    cv::Mat gray;
    cv::Mat quickGray;
    cv::Mat closeGray;
    cv::Mat borderedGray;
    cv::Mat equalizedGray;
    cv::Mat blurredGray;
    cv::Mat sharpenedGray;
    cv::Mat binaryGray;
    cv::Mat upscaledGray;
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
    QVariantList results;
    if (!(frame && frame->data[0] && frame->width > 0 && frame->height > 0)) {
        return results;
    }

    auto &impl = *m_impl;
    impl.convertCtx = sws_getCachedContext(
        impl.convertCtx, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), frame->width,
        frame->height, AV_PIX_FMT_BGR24, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!impl.convertCtx) {
        return results;
    }

    const int stride = frame->width * 3;
    impl.bgrBuffer.resize(static_cast<size_t>(stride) * static_cast<size_t>(frame->height));

    uint8_t *dstData[4] = { impl.bgrBuffer.data(), nullptr, nullptr, nullptr };
    int dstLinesize[4] = { stride, 0, 0, 0 };
    if (sws_scale(impl.convertCtx, frame->data, frame->linesize, 0, frame->height, dstData, dstLinesize) <= 0) {
        return results;
    }

    cv::Mat bgr(frame->height, frame->width, CV_8UC3, impl.bgrBuffer.data(), stride);

    // Match the Python scanner's fast path: try OpenCV's multi decode first,
    // then single decode on the camera-like BGR frame. Return immediately on hit.
    double quickScale = 1.0;
    impl.quickBgr = resizeToMaxSide(bgr, kQuickMaxSide, quickScale);
    if (tryDecode(
            impl.detector, impl.quickBgr, frame->width, frame->height, 1.0 / quickScale, 1.0 / quickScale,
            results)
        > 0) {
        return results;
    }

    if (quickScale != 1.0 && tryDecode(impl.detector, bgr, frame->width, frame->height, 1.0, 1.0, results) > 0) {
        return results;
    }

    cv::cvtColor(bgr, impl.gray, cv::COLOR_BGR2GRAY);
    if (tryDecode(impl.detector, impl.gray, frame->width, frame->height, 1.0, 1.0, results) > 0) {
        return results;
    }

    // Close QR codes often lose their quiet zone at the image edge. Add a white
    // margin and cap the max side so the detector sees a more normal QR scale.
    double closeScale = 1.0;
    impl.closeBgr = resizeToMaxSide(bgr, kCloseMaxSide, closeScale);
    int borderX = 0;
    int borderY = 0;
    impl.borderedBgr = addWhiteBorder(impl.closeBgr, borderX, borderY);
    if (tryDecode(
            impl.detector, impl.borderedBgr, frame->width, frame->height, 1.0 / closeScale, 1.0 / closeScale,
            results, borderX, borderY)
        > 0) {
        return results;
    }

    impl.closeGray = resizeToMaxSide(impl.gray, kCloseMaxSide, closeScale);
    impl.borderedGray = addWhiteBorder(impl.closeGray, borderX, borderY);
    if (tryDecode(
            impl.detector, impl.borderedGray, frame->width, frame->height, 1.0 / closeScale, 1.0 / closeScale,
            results, borderX, borderY)
        > 0) {
        return results;
    }

    // Lightweight fallbacks for low contrast or slightly blurred video frames.
    quickScale = 1.0;
    impl.quickGray = resizeToMaxSide(impl.gray, kQuickMaxSide, quickScale);
    tryDecode(
        impl.detector, impl.quickGray, frame->width, frame->height, 1.0 / quickScale, 1.0 / quickScale, results);
    if (!results.isEmpty()) {
        return results;
    }

    cv::equalizeHist(impl.gray, impl.equalizedGray);
    tryDecode(impl.detector, impl.equalizedGray, frame->width, frame->height, 1.0, 1.0, results);
    if (!results.isEmpty()) {
        return results;
    }

    cv::GaussianBlur(impl.gray, impl.blurredGray, cv::Size(0, 0), 1.0);
    cv::addWeighted(impl.gray, 1.8, impl.blurredGray, -0.8, 0, impl.sharpenedGray);
    tryDecode(impl.detector, impl.sharpenedGray, frame->width, frame->height, 1.0, 1.0, results);
    if (!results.isEmpty()) {
        return results;
    }

    double upscale = 1.0;
    impl.upscaledGray = upscaleForSmallQr(impl.gray, upscale);
    tryDecode(impl.detector, impl.upscaledGray, frame->width, frame->height, 1.0 / upscale, 1.0 / upscale, results);
    if (!results.isEmpty()) {
        return results;
    }

    cv::adaptiveThreshold(
        impl.equalizedGray, impl.binaryGray, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C, cv::THRESH_BINARY, 31, 4);
    tryDecode(impl.detector, impl.binaryGray, frame->width, frame->height, 1.0, 1.0, results);

    return results;
}
