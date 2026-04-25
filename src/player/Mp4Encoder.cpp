//
// Created by liangzhuohua on 2022/3/1.
//

#include "Mp4Encoder.h"

Mp4Encoder::Mp4Encoder(const string &saveFilePath) {
    // 分配
    _formatCtx = shared_ptr<AVFormatContext>(avformat_alloc_context(), &avformat_free_context);
    // FIX: 原来写的 "mov"，现在根据扩展名推导；保底使用 mp4
    const AVOutputFormat *fmt = av_guess_format(nullptr, saveFilePath.c_str(), nullptr);
    if (!fmt) {
        fmt = av_guess_format("mp4", nullptr, nullptr);
    }
    _formatCtx->oformat = fmt;
    // 文件保存路径
    _saveFilePath = saveFilePath;
}

Mp4Encoder::~Mp4Encoder() {
    if (_isOpen) {
        stop();
    }
}

void Mp4Encoder::addTrack(AVStream *stream) {
    AVStream *os = avformat_new_stream(_formatCtx.get(), nullptr);
    if (!os) {
        return;
    }
    int ret = avcodec_parameters_copy(os->codecpar, stream->codecpar);
    if (ret < 0) {
        return;
    }
    os->codecpar->codec_tag = 0;
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        audioIndex = os->index;
        _originAudioTimeBase = stream->time_base;
    } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        videoIndex = os->index;
        _originVideoTimeBase = stream->time_base;
    }
}

bool Mp4Encoder::start() {
    // 初始化上下文
    if (avio_open(&_formatCtx->pb, _saveFilePath.c_str(), AVIO_FLAG_READ_WRITE) < 0) {
        return false;
    }
    // 写输出流头信息
    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "movflags", "frag_keyframe+empty_moov", 0);
    int ret = avformat_write_header(_formatCtx.get(), &opts);
    if (ret < 0) {
        return false;
    }
    _isOpen = true;
    return true;
}

void Mp4Encoder::writePacket(const shared_ptr<AVPacket> &pkt, bool isVideo) {
    if (!_isOpen || !pkt) {
        return;
    }
    // FIX: 原来被 #ifdef I_FRAME_FIRST 包住，宏从未定义 => 录像从非关键帧开始，首段花屏
    // 未获取视频关键帧前先忽略音频与非关键帧视频
    if (!writtenKeyFrame) {
        if (videoIndex < 0) {
            // 没有视频就按收到音频即开始
            if (!isVideo) {
                writtenKeyFrame = true;
            }
        } else {
            if (!isVideo) {
                return;
            }
            if (!(pkt->flags & AV_PKT_FLAG_KEY)) {
                return;
            }
            writtenKeyFrame = true;
        }
    }

    if (isVideo) {
        if (videoIndex < 0) return;
        pkt->stream_index = videoIndex;
        av_packet_rescale_ts(pkt.get(), _originVideoTimeBase, _formatCtx->streams[videoIndex]->time_base);
    } else {
        if (audioIndex < 0) return;
        pkt->stream_index = audioIndex;
        av_packet_rescale_ts(pkt.get(), _originAudioTimeBase, _formatCtx->streams[audioIndex]->time_base);
    }
    pkt->pos = -1;
    av_write_frame(_formatCtx.get(), pkt.get());
}

void Mp4Encoder::stop() {
    if (!_isOpen) {
        return;
    }
    _isOpen = false;
    if (_formatCtx) {
        av_write_trailer(_formatCtx.get());
        if (_formatCtx->pb) {
            avio_close(_formatCtx->pb);
            _formatCtx->pb = nullptr;
        }
    }
}
