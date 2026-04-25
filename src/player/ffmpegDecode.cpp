#include "ffmpegDecode.h"
#include <QDateTime>
#include <QThread>
#include <iostream>
#include <vector>

#define MAX_AUDIO_PACKET (2 * 1024 * 1024)

bool FFmpegDecoder::OpenInput(string &inputFile) {
    CloseInput();

    if (!isHwDecoderEnable) {
        hwDecoderType = av_hwdevice_find_type_by_name("d3d11va");
        if (hwDecoderType != AV_HWDEVICE_TYPE_NONE) {
            isHwDecoderEnable = true;
        }
    }

    AVDictionary *param = nullptr;

    av_dict_set(&param, "preset", "ultrafast", 0);
    av_dict_set(&param, "tune", "zerolatency", 0);
    av_dict_set(&param, "buffer_size", "425984", 0);
    av_dict_set(&param, "rtsp_transport", "tcp", 0);
    av_dict_set(&param, "protocol_whitelist", "file,udp,tcp,rtp,rtmp,rtsp,http", 0);

    // 打开输入
    if (avformat_open_input(&pFormatCtx, inputFile.c_str(), nullptr, &param) != 0) {
        CloseInput();
        return false;
    }
    // 超时机制
    static const int timeout = 10;
    auto startTime = std::make_shared<uint64_t>();
    *startTime = QDateTime::currentSecsSinceEpoch();
    pFormatCtx->interrupt_callback.callback = [](void *ctx) -> int {
        uint64_t now = QDateTime::currentSecsSinceEpoch();
        return now - *(uint64_t *)ctx > timeout;
    };
    pFormatCtx->interrupt_callback.opaque = startTime.get();

    if (avformat_find_stream_info(pFormatCtx, nullptr) < 0) {
        CloseInput();
        return false;
    }

    // 分析超时，退出，可能格式不正确
    if (QDateTime::currentSecsSinceEpoch() - *startTime > timeout) {
        CloseInput();
        return false;
    }
    pFormatCtx->interrupt_callback.callback = nullptr;
    pFormatCtx->interrupt_callback.opaque = nullptr;

    // 打开视频/音频输入
    hasVideoStream = OpenVideo();
    hasAudioStream = OpenAudio();

    isOpen = true;

    // 转换时间基
    if (videoStreamIndex != -1) {
        videoFramePerSecond = av_q2d(pFormatCtx->streams[videoStreamIndex]->r_frame_rate);
        videoBaseTime = av_q2d(pFormatCtx->streams[videoStreamIndex]->time_base);
    }
    if (audioStreamIndex != -1) {
        audioBaseTime = av_q2d(pFormatCtx->streams[audioStreamIndex]->time_base);
    }

    // 创建音频解码缓存
    if (hasAudioStream) {
        // FIX: 原来使用 AV_FIFO_FLAG_AUTO_GROW 会无限扩展，改为固定上限 + writeAudioBuff 内丢弃最早数据
        // 上限约 1 秒音频（25 帧 × per-frame 样本 × 声道 × 2B）
        size_t cap = static_cast<size_t>(GetAudioFrameSamples())
                   * static_cast<size_t>(GetAudioChannelCount()) * 25 * 2;
        if (cap == 0) cap = 65536;
        audioFifoBuffer = shared_ptr<AVFifo>(
            av_fifo_alloc2(cap, 1, 0));
    }
    return true;
}

bool FFmpegDecoder::CloseInput() {
    isOpen = false;

    lock_guard<mutex> lck(_releaseLock);

    // 关闭流
    CloseVideo();
    CloseAudio();
    if (pFormatCtx) {
        avformat_close_input(&pFormatCtx);
        pFormatCtx = nullptr;
    }

    return true;
}

void freeFrame(AVFrame *f) {
    av_frame_free(&f);
}
void freePkt(AVPacket *f) {
    av_packet_free(&f);
}
void freeSwrCtx(SwrContext *s) {
    swr_free(&s);
}

shared_ptr<AVFrame> FFmpegDecoder::GetNextFrame() {
    // 加锁，避免在此方法执行过程中解码器释放，导致崩溃
    lock_guard<mutex> lck(_releaseLock);
    shared_ptr<AVFrame> res;
    if (videoStreamIndex == -1 && audioStreamIndex == -1) {
        return res;
    }
    if (!isOpen) {
        return res;
    }

    // 读输入流
    while (true) {
        if (!pFormatCtx) {
            throw runtime_error("分配解析器出错");
        }
        shared_ptr<AVPacket> packet = shared_ptr<AVPacket>(av_packet_alloc(), &freePkt);
        int ret = av_read_frame(pFormatCtx, packet.get());
        if (ret < 0) {
            char errStr[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errStr, AV_ERROR_MAX_STRING_SIZE);
            throw runtime_error("解析视频出错 " + string(errStr));
        }
        // 计算码率
        {
            bytesSecond += packet->size;
            uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
            if (now - lastCountBitrateTime >= 1000) {
                // 计算码率定时器
                bitrate = bytesSecond * 8 * 1000 / (now - lastCountBitrateTime);
                bytesSecond = 0;
                if (onBitrate) {
                    onBitrate(bitrate);
                }
                lastCountBitrateTime = now;
            }
        }
        if (packet->stream_index == videoStreamIndex) {
            // 回调nalu
            if (_gotPktCallback) {
                _gotPktCallback(packet);
            }
            // 处理视频数据
            shared_ptr<AVFrame> pVideoYuv = shared_ptr<AVFrame>(av_frame_alloc(), &freeFrame);
            // 解码视频祯
            bool isDecodeComplite = DecodeVideo(packet.get(), pVideoYuv);
            if (isDecodeComplite) {
                res = pVideoYuv;
            }
            // 回调frame
            if (_gotFrameCallback) {
                _gotFrameCallback(pVideoYuv);
            }
            break;
        } else if (packet->stream_index == audioStreamIndex) {
            // 回调nalu
            if (_gotPktCallback) {
                _gotPktCallback(packet);
            }
            // 处理音频数据
            if (packet->dts != AV_NOPTS_VALUE) {
                int audioFrameSize = MAX_AUDIO_PACKET;
                shared_ptr<uint8_t> pFrameAudio = shared_ptr<uint8_t>(new uint8_t[audioFrameSize]);
                // 解码音频祯
                int nDecodedSize = DecodeAudio(audioStreamIndex, packet.get(), pFrameAudio.get(), audioFrameSize);
                // 解码成功，解码数据写入音频缓存
                if (nDecodedSize > 0) {
                    writeAudioBuff(pFrameAudio.get(), nDecodedSize);
                }
            }
            if (!HasVideo()) {
                return res;
            }
        }
    }
    return res;
}

bool FFmpegDecoder::hwDecoderInit(AVCodecContext *ctx, const enum AVHWDeviceType type) {
    if (av_hwdevice_ctx_create(&hwDeviceCtx, type, nullptr, nullptr, 0) < 0) {
        return false;
    }
    ctx->hw_device_ctx = av_buffer_ref(hwDeviceCtx);

    return true;
}

bool FFmpegDecoder::OpenVideo() {
    bool res = false;

    if (pFormatCtx) {
        videoStreamIndex = -1;

        for (unsigned int i = 0; i < pFormatCtx->nb_streams; i++) {
            if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                videoStreamIndex = i;
                const AVCodec *codec = avcodec_find_decoder(pFormatCtx->streams[i]->codecpar->codec_id);

                // 如果有存在视频，检测硬件解码器
                if (codec && isHwDecoderEnable) {
                    for (int configIndex = 0;; configIndex++) {
                        const AVCodecHWConfig *config = avcodec_get_hw_config(codec, configIndex);
                        if (!config) {
                            isHwDecoderEnable = false;
                            break;
                        }
                        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX
                            && config->device_type == hwDecoderType) {
                            hwPixFmt = config->pix_fmt;
                            break;
                        }
                    }
                }

                if (codec) {
                    pVideoCodecCtx = avcodec_alloc_context3(codec);
                    if (pVideoCodecCtx) {
                        if (isHwDecoderEnable) {
                            isHwDecoderEnable = hwDecoderInit(pVideoCodecCtx, hwDecoderType);
                        }

                        if (avcodec_parameters_to_context(pVideoCodecCtx, pFormatCtx->streams[i]->codecpar) >= 0) {
                            res = !(avcodec_open2(pVideoCodecCtx, codec, nullptr) < 0);
                            if (res) {
                                width = pVideoCodecCtx->width;
                                height = pVideoCodecCtx->height;
                            }
                        }
                    }
                }

                break;
            }
        }

        if (!res) {
            CloseVideo();
        }
    }

    return res;
}

bool FFmpegDecoder::DecodeVideo(const AVPacket *av_pkt, shared_ptr<AVFrame> &pOutFrame) {
    bool res = false;

    if (pVideoCodecCtx && av_pkt && pOutFrame) {
        int ret = avcodec_send_packet(pVideoCodecCtx, av_pkt);
        if (ret < 0) {
            char errStr[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errStr, AV_ERROR_MAX_STRING_SIZE);
            throw runtime_error("发送视频包出错 " + string(errStr));
        }

        if (isHwDecoderEnable) {
            // Initialize the hardware frame.
            if (!hwFrame) {
                hwFrame = shared_ptr<AVFrame>(av_frame_alloc(), &freeFrame);
            }

            ret = avcodec_receive_frame(pVideoCodecCtx, hwFrame.get());
        } else {
            ret = avcodec_receive_frame(pVideoCodecCtx, pOutFrame.get());
        }

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            // No output available right now or end of stream
            return false;
        } else if (ret < 0) {
            char errStr[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errStr, AV_ERROR_MAX_STRING_SIZE);
            throw runtime_error("解码视频出错 " + string(errStr));
        }
        res = true;

        if (isHwDecoderEnable) {
            if (dropCurrentVideoFrame) {
                pOutFrame.reset();
                return false;
            }

            // Copy data from the hw surface to the out frame.
            ret = av_hwframe_transfer_data(pOutFrame.get(), hwFrame.get(), 0);

            if (ret < 0) {
                char errStr[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errStr, AV_ERROR_MAX_STRING_SIZE);
                throw runtime_error("Decode video frame error. " + string(errStr));
            }
        }
    }

    return res;
}

bool FFmpegDecoder::OpenAudio() {
    bool res = false;

    if (pFormatCtx) {
        audioStreamIndex = -1;

        for (unsigned int i = 0; i < pFormatCtx->nb_streams; i++) {
            if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audioStreamIndex = i;
                const AVCodec *codec = avcodec_find_decoder(pFormatCtx->streams[i]->codecpar->codec_id);

                if (codec) {
                    pAudioCodecCtx = avcodec_alloc_context3(codec);
                    if (pAudioCodecCtx) {
                        if (avcodec_parameters_to_context(pAudioCodecCtx, pFormatCtx->streams[i]->codecpar) >= 0) {
                            res = !(avcodec_open2(pAudioCodecCtx, codec, nullptr) < 0);
                        }
                    }
                }

                break;
            }
        }

        if (!res) {
            CloseAudio();
        }
    }

    return res;
}

void FFmpegDecoder::CloseVideo() {
    if (pVideoCodecCtx) {
        avcodec_close(pVideoCodecCtx);
        avcodec_free_context(&pVideoCodecCtx);
        pVideoCodecCtx = nullptr;
    }
    videoStreamIndex = -1;
    hasVideoStream = false;
    if (hwDeviceCtx) {
        av_buffer_unref(&hwDeviceCtx);
        hwDeviceCtx = nullptr;
    }
    hwFrame.reset();
    isHwDecoderEnable = false;
}

void FFmpegDecoder::CloseAudio() {
    if (pAudioCodecCtx) {
        avcodec_close(pAudioCodecCtx);
        avcodec_free_context(&pAudioCodecCtx);
        pAudioCodecCtx = nullptr;
    }
    audioStreamIndex = -1;
    hasAudioStream = false;
    swrCtx.reset();
}

int FFmpegDecoder::DecodeAudio(int nStreamIndex, const AVPacket *avpkt, uint8_t *pOutBuffer, size_t nOutBufferSize) {
    (void)nStreamIndex;
    if (!pAudioCodecCtx || !avpkt) {
        return 0;
    }

    // 标准 send / receive 循环
    int ret = avcodec_send_packet(pAudioCodecCtx, avpkt);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        char errStr[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errStr, AV_ERROR_MAX_STRING_SIZE);
        // 不抛异常：音频错误不应中断视频解码
        return 0;
    }

    int decodedSize = 0;
    AVFrame *audioFrame = av_frame_alloc();
    if (!audioFrame) {
        return 0;
    }

    while (true) {
        ret = avcodec_receive_frame(pAudioCodecCtx, audioFrame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            break;
        }

        uint8_t *pDest = pOutBuffer + decodedSize;
        size_t remaining = (decodedSize >= static_cast<int>(nOutBufferSize))
            ? 0
            : (nOutBufferSize - static_cast<size_t>(decodedSize));
        if (remaining == 0) {
            av_frame_unref(audioFrame);
            break;
        }

        int sizeToDecode = 0;
        if (audioFrame->format != AV_SAMPLE_FMT_S16) {
            if (!swrCtx) {
                SwrContext *ptr = nullptr;
                if (swr_alloc_set_opts2(
                        &ptr, &pAudioCodecCtx->ch_layout, AV_SAMPLE_FMT_S16, pAudioCodecCtx->sample_rate,
                        &pAudioCodecCtx->ch_layout, static_cast<AVSampleFormat>(audioFrame->format),
                        pAudioCodecCtx->sample_rate, 0, nullptr) < 0
                    || swr_init(ptr) < 0) {
                    if (ptr) swr_free(&ptr);
                    av_frame_unref(audioFrame);
                    break;
                }
                swrCtx = shared_ptr<SwrContext>(ptr, &freeSwrCtx);
            }

            int samples = swr_convert(
                swrCtx.get(), &pDest, audioFrame->nb_samples, (const uint8_t **)audioFrame->data,
                audioFrame->nb_samples);
            if (samples > 0) {
                sizeToDecode = samples * pAudioCodecCtx->ch_layout.nb_channels
                    * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
            }
        } else {
            sizeToDecode = av_samples_get_buffer_size(
                nullptr, pAudioCodecCtx->ch_layout.nb_channels, audioFrame->nb_samples, AV_SAMPLE_FMT_S16, 1);
            if (sizeToDecode > 0 && static_cast<size_t>(sizeToDecode) <= remaining) {
                memcpy(pDest, audioFrame->data[0], sizeToDecode);
            } else {
                sizeToDecode = 0;
            }
        }

        if (sizeToDecode > 0) {
            decodedSize += sizeToDecode;
        }
        av_frame_unref(audioFrame);
    }

    av_frame_free(&audioFrame);
    return decodedSize;
}

void FFmpegDecoder::writeAudioBuff(uint8_t *aSample, size_t aSize) {
    lock_guard<mutex> lck(abBuffMtx);
    if (!audioFifoBuffer || aSize == 0) {
        return;
    }
    // 若单次写入超过整个 fifo 容量，截尾只写末尾一部分
    size_t cap = av_fifo_can_write(audioFifoBuffer.get()) + av_fifo_can_read(audioFifoBuffer.get());
    if (aSize > cap) {
        aSample += (aSize - cap);
        aSize = cap;
    }
    // 腾空间（丢弃最老的样本）
    while (av_fifo_can_write(audioFifoBuffer.get()) < aSize) {
        size_t toDrop = std::min<size_t>(aSize, av_fifo_can_read(audioFifoBuffer.get()));
        if (toDrop == 0) break;
        std::vector<uint8_t> tmp(toDrop);
        av_fifo_read(audioFifoBuffer.get(), tmp.data(), toDrop);
    }
    av_fifo_write(audioFifoBuffer.get(), aSample, aSize);
}

size_t FFmpegDecoder::ReadAudioBuff(uint8_t *aSample, size_t aSize) {
    lock_guard<mutex> lck(abBuffMtx);
    if (av_fifo_elem_size(audioFifoBuffer.get()) < aSize) {
        return 0;
    }
    av_fifo_read(audioFifoBuffer.get(), aSample, aSize);
    return aSize;
}
void FFmpegDecoder::ClearAudioBuff() {
    lock_guard<mutex> lck(abBuffMtx);
    av_fifo_reset2(audioFifoBuffer.get());
}
