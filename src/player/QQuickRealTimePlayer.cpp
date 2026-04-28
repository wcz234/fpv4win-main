
#include "QQuickRealTimePlayer.h"
#include "JpegEncoder.h"
#include "QrCodeScanner.h"
#include <QDir>
#include <QDebug>
#include <QMetaObject>
#include <QOpenGLFramebufferObject>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QVariantMap>
#include <SDL2/SDL.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <future>
#include <sstream>

namespace {

void printQrCodes(const QVariantList &codes) {
    for (int i = 0; i < codes.size(); ++i) {
        const auto qrItem = codes.at(i).toMap();
        const auto text = qrItem.value("text").toString();
        if (text.isEmpty()) {
            continue;
        }

        qInfo().noquote() << QStringLiteral("QR[%1]: %2").arg(i).arg(text);
    }
}

float qrIoU(const QVariantMap &a, const QVariantMap &b) {
    const float ax1 = a.value("x").toFloat();
    const float ay1 = a.value("y").toFloat();
    const float ax2 = ax1 + a.value("width").toFloat();
    const float ay2 = ay1 + a.value("height").toFloat();
    const float bx1 = b.value("x").toFloat();
    const float by1 = b.value("y").toFloat();
    const float bx2 = bx1 + b.value("width").toFloat();
    const float by2 = by1 + b.value("height").toFloat();

    const float ix1 = std::max(ax1, bx1);
    const float iy1 = std::max(ay1, by1);
    const float ix2 = std::min(ax2, bx2);
    const float iy2 = std::min(ay2, by2);
    const float intersection = std::max(0.0f, ix2 - ix1) * std::max(0.0f, iy2 - iy1);
    const float areaA = std::max(0.0f, ax2 - ax1) * std::max(0.0f, ay2 - ay1);
    const float areaB = std::max(0.0f, bx2 - bx1) * std::max(0.0f, by2 - by1);
    const float unionArea = areaA + areaB - intersection;
    return unionArea > 0.0f ? intersection / unionArea : 0.0f;
}

bool isSameQrPosition(const QVariantMap &a, const QVariantMap &b) {
    if (qrIoU(a, b) > 0.55f) {
        return true;
    }

    const float ax = a.value("x").toFloat();
    const float ay = a.value("y").toFloat();
    const float aw = a.value("width").toFloat();
    const float ah = a.value("height").toFloat();
    const float bx = b.value("x").toFloat();
    const float by = b.value("y").toFloat();
    const float bw = b.value("width").toFloat();
    const float bh = b.value("height").toFloat();
    const float centerDistance = std::hypot((ax + aw * 0.5f) - (bx + bw * 0.5f), (ay + ah * 0.5f) - (by + bh * 0.5f));
    return centerDistance < 0.025f && std::abs(aw - bw) < 0.06f && std::abs(ah - bh) < 0.06f;
}

template <size_t WorkerCount>
QVariantList mergeQrCodeResults(const std::array<QVariantList, WorkerCount> &workerResults) {
    QVariantList merged;
    for (const auto &results : workerResults) {
        for (const auto &item : results) {
            const auto candidate = item.toMap();
            bool duplicate = false;
            for (const auto &existingItem : merged) {
                if (isSameQrPosition(existingItem.toMap(), candidate)) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                merged.push_back(item);
            }
        }
    }
    return merged;
}

} // namespace

// GIF默认帧率
#define DEFAULT_GIF_FRAMERATE 10

//************TaoItemRender************//
class TItemRender : public QQuickFramebufferObject::Renderer {
public:
    TItemRender();

    void render() override;
    QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override;
    void synchronize(QQuickFramebufferObject *) override;

private:
    RealTimeRenderer m_render;
    QQuickWindow *m_window = nullptr;
};

TItemRender::TItemRender() {
    m_render.init();
}

void TItemRender::render() {
    m_render.paint();
    m_window->resetOpenGLState();
}

QOpenGLFramebufferObject *TItemRender::createFramebufferObject(const QSize &size) {
    QOpenGLFramebufferObjectFormat format;
    format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
    format.setSamples(4);
    m_render.resize(size.width(), size.height());
    return new QOpenGLFramebufferObject(size, format);
}

void TItemRender::synchronize(QQuickFramebufferObject *item) {

    auto *pItem = qobject_cast<QQuickRealTimePlayer *>(item);
    if (pItem) {
        if (!m_window) {
            m_window = pItem->window();
        }
        if (pItem->infoDirty()) {
            m_render.updateTextureInfo(pItem->videoWidth(), pItem->videoHeght(), pItem->videoFormat());
            pItem->makeInfoDirty(false);
        }
        if (pItem->playStop) {
            m_render.clear();
            return;
        }
        bool got = false;
        shared_ptr<AVFrame> frame = pItem->getFrame(got);
        if (got && frame->linesize[0]) {
            m_render.updateTextureData(frame);
        }
    }
}

//************QQuickRealTimePlayer************//
QQuickRealTimePlayer::QQuickRealTimePlayer(QQuickItem *parent)
    : QQuickFramebufferObject(parent) {
    SDL_Init(SDL_INIT_AUDIO);
    startQrScanThread();
    // FIX: 注释和代码不一致；按 60fps 刷新 UI 足够
    startTimer(1000 / 60);
}

void QQuickRealTimePlayer::timerEvent(QTimerEvent *event) {
    Q_UNUSED(event);
    update();
}

shared_ptr<AVFrame> QQuickRealTimePlayer::getFrame(bool &got) {
    got = false;
    shared_ptr<AVFrame> frame;
    {
        lock_guard<mutex> lck(mtx);
        // 帧缓冲区已被清空,跳过渲染
        if (videoFrameQueue.empty()) {
            return {};
        }
        // 从帧缓冲区取出帧
        frame = videoFrameQueue.front();
        got = true;
        // 缓冲区出队被渲染的帧
        videoFrameQueue.pop();
    }
    // 缓冲，追帧机制
    _lastFrame = frame;
    return frame;
}

QVariantList QQuickRealTimePlayer::qrCodes() const {
    lock_guard<mutex> lck(m_qrCodesMtx);
    return m_qrCodes;
}

void QQuickRealTimePlayer::onVideoInfoReady(int width, int height, int format) {
    bool changed = false;
    if (m_videoWidth != width) {
        m_videoWidth = width;
        changed = true;
    }
    if (m_videoHeight != height) {
        m_videoHeight = height;
        changed = true;
    }
    if (m_videoFormat != format) {
        m_videoFormat = format;
        changed = true;
    }
    if (changed) {
        makeInfoDirty(true);
        emit onVideoInfoChanged();
    }
}

QQuickFramebufferObject::Renderer *QQuickRealTimePlayer::createRenderer() const {
    return new TItemRender;
}

void QQuickRealTimePlayer::play(const QString &playUrl) {
    // 先停掉之前的会话（确保线程彻底回收）
    stop();

    playStop = false;
    m_qrGeneration.fetch_add(1);
    m_lastQrScanAt.store(0);
    m_lastQrHitAt.store(0);
    clearQrCodes();

    // 启动分析线程（不再 detach，stop() 中 join）
    analysisThread = std::thread([this, playUrl]() {
        auto decoder_ = make_shared<FFmpegDecoder>();
        url = playUrl.toStdString();
        // 打开并分析输入
        bool ok = decoder_->OpenInput(url);
        if (!ok) {
            emit onError("视频加载出错", -2);
            playStop = true;
            emit onPlayStopped();
            return;
        }
        decoder = decoder_;

        if (!isMuted && decoder->HasAudio()) {
            enableAudio();
        }
        emit onHasAudio(decoder->HasAudio());

        if (decoder->HasVideo()) {
            onVideoInfoReady(decoder->GetWidth(), decoder->GetHeight(), decoder->GetVideoFrameFormat());
        }

        decoder->onBitrate = [this](uint64_t bitrate) { emit onBitrate(static_cast<long>(bitrate)); };

        // 解码循环直接在本线程里跑，避免 detach 后无法 join 的问题
        while (!playStop) {
            try {
                auto frame = decoder->GetNextFrame();
                if (!frame) {
                    continue;
                }
                {
                    lock_guard<mutex> lck(mtx);
                    if (videoFrameQueue.size() > 10) {
                        videoFrameQueue.pop();
                    }
                    videoFrameQueue.push(frame);
                }
                enqueueQrScanFrame(frame);
            } catch (const exception &e) {
                emit onError(e.what(), -2);
                break;
            }
        }
        playStop = true;
        emit onPlayStopped();
    });
}

void QQuickRealTimePlayer::stop() {
    playStop = true;
    m_qrGeneration.fetch_add(1);
    m_lastQrScanAt.store(0);
    m_lastQrHitAt.store(0);
    {
        lock_guard<mutex> lck(m_qrFrameMtx);
        m_qrPendingFrame.reset();
        m_qrPendingGeneration = 0;
        m_qrPendingAt = 0;
        ++m_qrFrameSequence;
        std::fill(m_qrWorkerResultSequences.begin(), m_qrWorkerResultSequences.end(), 0);
    }

    // 打断 avformat_read_frame 的阻塞
    if (decoder && decoder->pFormatCtx) {
        decoder->pFormatCtx->interrupt_callback.callback = [](void *) { return 1; };
    }

    // 清掉录像回调（避免 stop 时回调访问悬垂的 encoder）
    if (decoder) {
        decoder->_gotPktCallback = nullptr;
        decoder->_gotFrameCallback = nullptr;
    }

    // 等线程自然退出
    if (analysisThread.joinable()) {
        analysisThread.join();
    }
    if (decodeThread.joinable()) {
        decodeThread.join();
    }

    {
        lock_guard<mutex> lck(mtx);
        while (!videoFrameQueue.empty()) {
            videoFrameQueue.pop();
        }
    }
    SDL_CloseAudio();
    clearQrCodes();
    if (decoder) {
        decoder->CloseInput();
    }
}

void QQuickRealTimePlayer::setMuted(bool muted) {
    // FIX: 原来未判 decoder 可空，首次调用直接崩溃
    if (!decoder) {
        isMuted = muted;
        emit onMutedChanged(muted);
        return;
    }
    if (!decoder->HasAudio()) {
        isMuted = muted;
        emit onMutedChanged(muted);
        return;
    }
    if (!muted) {
        decoder->ClearAudioBuff();
        if (!enableAudio()) {
            return;
        }
    } else {
        disableAudio();
    }
    isMuted = muted;
    emit onMutedChanged(muted);
}

void QQuickRealTimePlayer::setQrScanEnabled(bool enabled) {
    if (m_qrScanEnabled.exchange(enabled) == enabled) {
        return;
    }
    m_qrGeneration.fetch_add(1);
    m_lastQrScanAt.store(0);
    m_lastQrHitAt.store(0);
    if (!enabled) {
        {
            lock_guard<mutex> lck(m_qrFrameMtx);
            m_qrPendingFrame.reset();
            m_qrPendingGeneration = 0;
            m_qrPendingAt = 0;
            ++m_qrFrameSequence;
            std::fill(m_qrWorkerResultSequences.begin(), m_qrWorkerResultSequences.end(), 0);
        }
        clearQrCodes();
    }
    emit onQrScanEnabledChanged(enabled);
}

QQuickRealTimePlayer::~QQuickRealTimePlayer() {
    stop();
    stopQrScanThread();
}

void QQuickRealTimePlayer::startQrScanThread() {
    if (std::any_of(qrScanThreads.begin(), qrScanThreads.end(), [](const std::thread &thread) {
            return thread.joinable();
        })) {
        return;
    }
    m_qrThreadStop.store(false);
    {
        lock_guard<mutex> lck(m_qrFrameMtx);
        std::fill(m_qrWorkerResultSequences.begin(), m_qrWorkerResultSequences.end(), 0);
    }

    for (size_t workerIndex = 0; workerIndex < QR_SCAN_WORKER_COUNT; ++workerIndex) {
        qrScanThreads[workerIndex] = std::thread([this, workerIndex]() {
            QrCodeScanner scanner;
            uint64_t lastSequence = 0;
            while (!m_qrThreadStop.load()) {
                shared_ptr<AVFrame> frame;
                uint64_t generation = 0;
                uint64_t queuedAt = 0;
                uint64_t sequence = 0;
                {
                    std::unique_lock<std::mutex> lck(m_qrFrameMtx);
                    m_qrFrameCv.wait(lck, [this, &lastSequence]() {
                        return m_qrThreadStop.load() || (m_qrPendingFrame && m_qrFrameSequence != lastSequence);
                    });
                    if (m_qrThreadStop.load()) {
                        return;
                    }
                    frame = m_qrPendingFrame;
                    generation = m_qrPendingGeneration;
                    queuedAt = m_qrPendingAt;
                    sequence = m_qrFrameSequence;
                    lastSequence = sequence;
                }
                scanQrCodes(workerIndex, scanner, frame, generation, queuedAt, sequence);
            }
        });
    }
}

void QQuickRealTimePlayer::stopQrScanThread() {
    m_qrThreadStop.store(true);
    {
        lock_guard<mutex> lck(m_qrFrameMtx);
        m_qrPendingFrame.reset();
        ++m_qrFrameSequence;
    }
    m_qrFrameCv.notify_all();
    for (auto &thread : qrScanThreads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

void QQuickRealTimePlayer::enqueueQrScanFrame(const shared_ptr<AVFrame> &frame) {
    if (!(frame && m_qrScanEnabled.load())) {
        return;
    }
    const auto now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
    if (now < m_lastQrScanAt.load() + QR_SCAN_INTERVAL_MS) {
        return;
    }
    m_lastQrScanAt.store(now);

    {
        lock_guard<mutex> lck(m_qrFrameMtx);
        m_qrPendingFrame = frame;
        m_qrPendingGeneration = m_qrGeneration.load();
        m_qrPendingAt = now;
        ++m_qrFrameSequence;
        std::fill(m_qrWorkerResultSequences.begin(), m_qrWorkerResultSequences.end(), 0);
    }
    m_qrFrameCv.notify_all();
}

void QQuickRealTimePlayer::scanQrCodes(
    size_t workerIndex, QrCodeScanner &scanner, const shared_ptr<AVFrame> &frame, uint64_t generation,
    uint64_t queuedAt, uint64_t sequence) {
    if (!(frame && m_qrScanEnabled.load())) {
        return;
    }

    const auto codes = scanner.scanProfile(frame, static_cast<int>(workerIndex));
    QVariantList mergedCodes;
    bool ready = false;
    {
        lock_guard<mutex> lck(m_qrFrameMtx);
        if (sequence != m_qrFrameSequence) {
            return;
        }

        m_qrWorkerResults[workerIndex] = codes;
        m_qrWorkerResultSequences[workerIndex] = sequence;
        ready = std::all_of(
            m_qrWorkerResultSequences.begin(), m_qrWorkerResultSequences.end(),
            [sequence](uint64_t value) { return value == sequence; });
        if (ready) {
            mergedCodes = mergeQrCodeResults(m_qrWorkerResults);
        }
    }
    if (!ready) {
        return;
    }

    QMetaObject::invokeMethod(
        this,
        [this, codes = mergedCodes, generation, queuedAt, sequence]() {
            if (generation != m_qrGeneration.load()) {
                return;
            }
            {
                lock_guard<mutex> lck(m_qrFrameMtx);
                if (sequence != m_qrFrameSequence) {
                    return;
                }
            }
            if (!codes.isEmpty()) {
                m_lastQrHitAt.store(queuedAt);
                updateQrCodes(codes);
                return;
            }

            const auto lastHitAt = m_lastQrHitAt.load();
            if (lastHitAt != 0 && queuedAt < lastHitAt + QR_HOLD_MS) {
                return;
            }

            updateQrCodes(codes);
        },
        Qt::QueuedConnection);
}

void QQuickRealTimePlayer::updateQrCodes(const QVariantList &codes) {
    {
        lock_guard<mutex> lck(m_qrCodesMtx);
        if (m_qrCodes == codes) {
            return;
        }
        m_qrCodes = codes;
    }
    printQrCodes(codes);
    emit onQrCodesChanged();
}

void QQuickRealTimePlayer::clearQrCodes() {
    updateQrCodes({});
}

QString QQuickRealTimePlayer::captureJpeg() {
    if (!_lastFrame) {
        return "";
    }
    QString dirPath = QFileInfo("jpg/l").absolutePath();
    QDir dir(dirPath);
    if (!dir.exists()) {
        dir.mkpath(dirPath);
    }
    stringstream ss;
    ss << "jpg/";
    ss << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
              .count()
       << ".jpg";
    auto ok = JpegEncoder::encodeJpeg(ss.str(), _lastFrame);
    // 截图
    return ok ? QString(ss.str().c_str()) : "";
}

bool QQuickRealTimePlayer::startRecord() {
    if (playStop && !_lastFrame) {
        return false;
    }
    QString dirPath = QFileInfo("mp4/l").absolutePath();
    QDir dir(dirPath);
    if (!dir.exists()) {
        dir.mkpath(dirPath);
    }
    // 保存路径
    stringstream ss;
    ss << "mp4/";
    ss << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
              .count()
       << ".mp4";
    // 创建MP4编码器
    _mp4Encoder = make_shared<Mp4Encoder>(ss.str());

    // 添加音频流
    if (decoder->HasAudio()) {
        _mp4Encoder->addTrack(decoder->pFormatCtx->streams[decoder->audioStreamIndex]);
    }
    // 添加视频流
    if (decoder->HasVideo()) {
        _mp4Encoder->addTrack(decoder->pFormatCtx->streams[decoder->videoStreamIndex]);
    }
    if (!_mp4Encoder->start()) {
        return false;
    }
    // FIX: 回调按值捕获 shared_ptr，即使成员被置空仍能安全写入当前录像
    auto encoder = _mp4Encoder;
    int videoStreamIndex = decoder->videoStreamIndex;
    decoder->_gotPktCallback = [encoder, videoStreamIndex](const shared_ptr<AVPacket> &packet) {
        if (!encoder) return;
        encoder->writePacket(packet, packet->stream_index == videoStreamIndex);
    };
    return true;
}

QString QQuickRealTimePlayer::stopRecord() {
    if (!_mp4Encoder) {
        return {};
    }
    // FIX: 原来先 stop 再清回调 => 解码线程可能在 stop 后访问已关闭 encoder
    if (decoder) {
        decoder->_gotPktCallback = nullptr;
    }
    // 此时若解码线程正好在回调中，会持 shared_ptr 副本写入；我们等它自然返回。
    // 通过临时置空成员再 stop 来保证后续不会再入。
    auto encoder = _mp4Encoder;
    _mp4Encoder.reset();
    encoder->stop();
    return { encoder->_saveFilePath.c_str() };
}

int QQuickRealTimePlayer::getVideoWidth() {
    if (!decoder) {
        return 0;
    }
    return decoder->width;
}

int QQuickRealTimePlayer::getVideoHeight() {
    if (!decoder) {
        return 0;
    }
    return decoder->height;
}

bool QQuickRealTimePlayer::enableAudio() {
    if (!decoder->HasAudio()) {
        return false;
    }
    // 音频参数
    SDL_AudioSpec audioSpec;
    audioSpec.freq = decoder->GetAudioSampleRate();
    audioSpec.format = AUDIO_S16;
    audioSpec.channels = decoder->GetAudioChannelCount();
    audioSpec.silence = 1;
    audioSpec.samples = decoder->GetAudioFrameSamples();
    audioSpec.padding = 0;
    audioSpec.size = 0;
    audioSpec.userdata = this;
    // 音频样本读取回调
    audioSpec.callback = [](void *Thiz, Uint8 *stream, int len) {
        auto *pThis = static_cast<QQuickRealTimePlayer *>(Thiz);
        SDL_memset(stream, 0, len);
        pThis->decoder->ReadAudioBuff(stream, len);
        if (pThis->isMuted) {
            SDL_memset(stream, 0, len);
        }
    };
    // 关闭音频
    SDL_CloseAudio();
    // 开启声音
    if (SDL_OpenAudio(&audioSpec, nullptr) == 0) {
        // 播放声音
        SDL_PauseAudio(0);
    } else {
        emit onError("开启音频出错，如需听声音请插入音频外设\n" + QString(SDL_GetError()), -1);
        return false;
    }
    return true;
}

void QQuickRealTimePlayer::disableAudio() {
    SDL_CloseAudio();
}

bool QQuickRealTimePlayer::startGifRecord() {
    if (playStop) {
        return false;
    }
    // 保存路径
    stringstream ss;
    ss << QStandardPaths::writableLocation(QStandardPaths::DesktopLocation).toStdString() << "/";
    ss << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
              .count()
       << ".gif";
    if (!(decoder && decoder->HasVideo())) {
        return false;
    }
    // 创建gif编码器
    _gifEncoder = make_shared<GifEncoder>();
    if (!_gifEncoder->open(
            decoder->width, decoder->height, decoder->GetVideoFrameFormat(), DEFAULT_GIF_FRAMERATE, ss.str())) {
        return false;
    }
    // FIX: 回调按值捕获 shared_ptr
    auto enc = _gifEncoder;
    decoder->_gotFrameCallback = [enc](const shared_ptr<AVFrame> &frame) {
        if (!enc || !enc->isOpened()) {
            return;
        }
        uint64_t now
            = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                  .count();
        if (enc->getLastEncodeTime() + 1000 / enc->getFrameRate() > now) {
            return;
        }
        enc->encodeFrame(frame);
    };

    return true;
}

void QQuickRealTimePlayer::stopGifRecord() {
    if (decoder) {
        decoder->_gotFrameCallback = nullptr;
    }
    if (!_gifEncoder) {
        return;
    }
    auto enc = _gifEncoder;
    _gifEncoder.reset();
    enc->close();
}
