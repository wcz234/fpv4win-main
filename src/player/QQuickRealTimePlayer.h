#pragma once
#include "RealTimeRenderer.h"
#include "ffmpegDecode.h"
#include <QQuickFramebufferObject>
#include <QQuickItem>
#include <QVariantList>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

#include "GifEncoder.h"
#include "Mp4Encoder.h"

class TItemRender;
class QrCodeScanner;

class QQuickRealTimePlayer : public QQuickFramebufferObject {
    Q_OBJECT
    Q_PROPERTY(bool isMuted READ getMuted WRITE setMuted NOTIFY onMutedChanged)
    Q_PROPERTY(bool hasAudio READ hasAudio NOTIFY onHasAudio)
    Q_PROPERTY(bool qrScanEnabled READ qrScanEnabled WRITE setQrScanEnabled NOTIFY onQrScanEnabledChanged)
    Q_PROPERTY(QVariantList qrCodes READ qrCodes NOTIFY onQrCodesChanged)
    Q_PROPERTY(int videoFrameWidth READ videoWidth NOTIFY onVideoInfoChanged)
    Q_PROPERTY(int videoFrameHeight READ videoHeght NOTIFY onVideoInfoChanged)
public:
    explicit QQuickRealTimePlayer(QQuickItem *parent = nullptr);
    ~QQuickRealTimePlayer() override;
    void timerEvent(QTimerEvent *event) override;

    shared_ptr<AVFrame> getFrame(bool &got);

    bool infoDirty() const { return m_infoChanged; }
    void makeInfoDirty(bool dirty) { m_infoChanged = dirty; }
    int videoWidth() const { return m_videoWidth; }
    int videoHeght() const { return m_videoHeight; }
    int videoFormat() const { return m_videoFormat; }
    bool getMuted() const { return isMuted; }
    bool qrScanEnabled() const { return m_qrScanEnabled.load(); }
    QVariantList qrCodes() const;
    // 播放
    Q_INVOKABLE void play(const QString &playUrl);
    // 停止
    Q_INVOKABLE void stop();
    // 静音
    Q_INVOKABLE void setMuted(bool muted = false);
    // 二维码识别开关
    Q_INVOKABLE void setQrScanEnabled(bool enabled);
    // 截图
    Q_INVOKABLE QString captureJpeg();
    // 录像
    Q_INVOKABLE bool startRecord();
    Q_INVOKABLE QString stopRecord();
    // 录制GIF
    Q_INVOKABLE bool startGifRecord();
    Q_INVOKABLE void stopGifRecord();
    // 获取视频宽度
    Q_INVOKABLE int getVideoWidth();
    // 获取视频高度
    Q_INVOKABLE int getVideoHeight();

signals:
    // 播放已经停止
    void onPlayStopped();
    // 出错
    void onError(QString msg, int code);
    // 获取录音音量
    void gotRecordVol(double vol);
    // 获得码率
    void onBitrate(long bitrate);
    // 静音
    void onMutedChanged(bool muted);
    // 是否有音频
    void onHasAudio(bool has);
    // 二维码结果更新
    void onQrCodesChanged();
    void onQrScanEnabledChanged(bool enabled);
    // 视频尺寸/格式更新
    void onVideoInfoChanged();

    friend class TItemRender;

protected:
    // ffmpeg解码器
    shared_ptr<FFmpegDecoder> decoder;
    // 播放地址
    string url;
    // 播放标记位
    volatile bool playStop = true;
    // 静音标记位
    volatile bool isMuted = true;
    // 帧队列
    std::queue<shared_ptr<AVFrame>> videoFrameQueue;
    mutex mtx;
    // 解码线程
    std::thread decodeThread;
    // 分析线程
    std::thread analysisThread;
    // 最后输出的帧
    shared_ptr<AVFrame> _lastFrame;
    // 视频是否ready
    void onVideoInfoReady(int width, int height, int format);
    // 播放音频
    bool enableAudio();
    // 停止播放音频
    void disableAudio();
    // MP4录制器
    shared_ptr<Mp4Encoder> _mp4Encoder;
    // GIF录制器
    shared_ptr<GifEncoder> _gifEncoder;
    unique_ptr<QrCodeScanner> m_qrScanner;
    std::thread qrScanThread;
    std::mutex m_qrFrameMtx;
    std::condition_variable m_qrFrameCv;
    shared_ptr<AVFrame> m_qrPendingFrame;
    uint64_t m_qrPendingGeneration = 0;
    uint64_t m_qrPendingAt = 0;
    std::atomic_bool m_qrThreadStop = false;
    QVariantList m_qrCodes;
    mutable mutex m_qrCodesMtx;
    std::atomic_bool m_qrScanEnabled = true;
    std::atomic_uint64_t m_qrGeneration = 0;
    std::atomic_uint64_t m_lastQrScanAt = 0;
    std::atomic_uint64_t m_lastQrHitAt = 0;
    static constexpr uint64_t QR_SCAN_INTERVAL_MS = 200;
    static constexpr uint64_t QR_HOLD_MS = 800;
    // 是否有声音
    bool hasAudio() {
        if (!decoder) {
            return false;
        }
        return decoder->HasAudio();
    }
    void startQrScanThread();
    void stopQrScanThread();
    void enqueueQrScanFrame(const shared_ptr<AVFrame> &frame);
    void scanQrCodes(const shared_ptr<AVFrame> &frame, uint64_t generation, uint64_t queuedAt);
    void updateQrCodes(const QVariantList &codes);
    void clearQrCodes();

public:
    Renderer *createRenderer() const override;
    int m_videoWidth {};
    int m_videoHeight {};
    int m_videoFormat {};
    bool m_infoChanged = false;
};
