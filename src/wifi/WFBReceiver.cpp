//
// Created by Talus on 2024/6/10.
//

#include "WFBReceiver.h"
#include "QmlNativeAPI.h"
#include "RxFrame.h"
#include "WFBProcessor.h"
#include "WiFiDriver.h"
#include "logger.h"

#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>

#include "Rtp.h"

// 跨平台 socket 抽象
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
using socket_t = SOCKET;
#  define CLOSE_SOCKET(s) closesocket(s)
#else
#  include <sys/socket.h>
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <unistd.h>
using socket_t = int;
#  ifndef INVALID_SOCKET
#    define INVALID_SOCKET (-1)
#  endif
#  define CLOSE_SOCKET(s) ::close(s)
#endif

std::vector<std::string> WFBReceiver::GetDongleList() {
    std::vector<std::string> list;

    libusb_context *findctx;
    // Initialize libusb
    libusb_init(&findctx);

    // Get list of USB devices
    libusb_device **devs;
    ssize_t count = libusb_get_device_list(findctx, &devs);
    if (count < 0) {
        return list;
    }

    // Iterate over devices
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device *dev = devs[i];
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(dev, &desc) == 0) {
            // Check if the device is using libusb driver
            if (desc.bDeviceClass == LIBUSB_CLASS_PER_INTERFACE) {
                std::stringstream ss;
                ss << std::setw(4) << std::setfill('0') << std::hex << desc.idVendor << ":";
                ss << std::setw(4) << std::setfill('0') << std::hex << desc.idProduct;
                list.push_back(ss.str());
            }
        }
    }
    std::sort(list.begin(), list.end(), [](std::string &a, std::string &b) {
        static std::vector<std::string> specialStrings = { "0b05:17d2", "0bda:8812", "0bda:881a" };
        auto itA = std::find(specialStrings.begin(), specialStrings.end(), a);
        auto itB = std::find(specialStrings.begin(), specialStrings.end(), b);
        if (itA != specialStrings.end() && itB == specialStrings.end()) {
            return true;
        }
        if (itB != specialStrings.end() && itA == specialStrings.end()) {
            return false;
        }
        return a < b;
    });

    // Free the list of devices
    libusb_free_device_list(devs, 1);

    // Deinitialize libusb
    libusb_exit(findctx);
    return list;
}
bool WFBReceiver::Start(const std::string &vidPid, uint8_t channel, int channelWidth, const std::string &kPath) {
    std::lock_guard<std::mutex> lock(stateMutex);

    QmlNativeAPI::Instance().wifiFrameCount_ = 0;
    QmlNativeAPI::Instance().wfbFrameCount_ = 0;
    QmlNativeAPI::Instance().rtpPktCount_ = 0;
    QmlNativeAPI::Instance().UpdateCount();

    keyPath = kPath;
    if (running.load()) {
        return false;
    }
    running = true;
    stopping = false;
    int rc;

    // 每次启动重建 aggregator 会话
    {
        std::lock_guard<std::mutex> aggLock(aggMutex);
        const uint32_t link_id = 7669206; // sha1 hash of link_domain="default"
        const uint8_t video_radio_port = 0;
        const uint64_t epoch = 0;
        const uint32_t video_channel_id_f = (link_id << 8) + video_radio_port;
        videoChannelIdBE = htobe32(video_channel_id_f);
        auto *be8 = reinterpret_cast<uint8_t *>(&videoChannelIdBE);
        for (int i = 0; i < 4; ++i) {
            videoChannelIdBE8[i] = be8[i];
        }
        try {
            videoAggregator = std::make_unique<Aggregator>(
                keyPath.c_str(), epoch, video_channel_id_f,
                [](uint8_t *payload, uint16_t packet_size) {
                    WFBReceiver::Instance().handleRtp(payload, packet_size);
                });
        } catch (const std::exception &e) {
            QmlNativeAPI::Instance().PutLog("error", std::string("Aggregator init failed: ") + e.what());
            return false;
        }
    }

    // get vid pid
    std::istringstream iss(vidPid);
    unsigned int wifiDeviceVid, wifiDevicePid;
    char c;
    iss >> std::hex >> wifiDeviceVid >> c >> wifiDevicePid;

    auto logger = std::make_shared<Logger>(
        [](const std::string &level, const std::string &msg) { QmlNativeAPI::Instance().PutLog(level, msg); });

    rc = libusb_init(&ctx);
    if (rc < 0) {
        running = false;
        std::lock_guard<std::mutex> aggLock(aggMutex);
        videoAggregator.reset();
        return false;
    }
    dev_handle = libusb_open_device_with_vid_pid(ctx, wifiDeviceVid, wifiDevicePid);
    if (dev_handle == nullptr) {
        logger->error("Cannot find device {:04x}:{:04x}", wifiDeviceVid, wifiDevicePid);
        libusb_exit(ctx);
        ctx = nullptr;
        running = false;
        std::lock_guard<std::mutex> aggLock(aggMutex);
        videoAggregator.reset();
        return false;
    }

    /*Check if kenel driver attached*/
    if (libusb_kernel_driver_active(dev_handle, 0)) {
        rc = libusb_detach_kernel_driver(dev_handle, 0); // detach driver
    }
    rc = libusb_claim_interface(dev_handle, 0);

    if (rc < 0) {
        // FIX: 原来只 return false，未释放 dev_handle/ctx，导致下次 Start 失败
        libusb_close(dev_handle);
        dev_handle = nullptr;
        libusb_exit(ctx);
        ctx = nullptr;
        running = false;
        std::lock_guard<std::mutex> aggLock(aggMutex);
        videoAggregator.reset();
        return false;
    }

    usbThread = std::make_shared<std::thread>([=]() {
        WiFiDriver wifi_driver { logger };
        try {
            rtlDevice = wifi_driver.CreateRtlDevice(dev_handle);
            rtlDevice->Init(
                [](const Packet &p) {
                    WFBReceiver::Instance().handle80211Frame(p);
                    QmlNativeAPI::Instance().UpdateCount();
                },
                SelectedChannel {
                    .Channel = channel,
                    .ChannelOffset = 0,
                    .ChannelWidth = static_cast<ChannelWidth_t>(channelWidth),
                });
        } catch (const std::runtime_error &e) {
            logger->error(e.what());
        } catch (...) {
        }
        auto rc = libusb_release_interface(dev_handle, 0);
        (void)rc;
        logger->info("==========stoped==========");
        libusb_close(dev_handle);
        libusb_exit(ctx);
        dev_handle = nullptr;
        ctx = nullptr;
        // 通知 QML 端；真正的线程/状态清理留给主线程 Stop() 做，不在自己线程里销毁承载自己的对象
        rtlDevice.reset();
        {
            std::lock_guard<std::mutex> aggLock(aggMutex);
            videoAggregator.reset();
        }
        running = false;
        QmlNativeAPI::Instance().NotifyWifiStop();
    });
    usbThread->detach();

    return true;
}
void WFBReceiver::handle80211Frame(const Packet &packet) {

    QmlNativeAPI::Instance().wifiFrameCount_++;
    RxFrame frame(packet.Data);
    if (!frame.IsValidWfbFrame()) {
        return;
    }
    QmlNativeAPI::Instance().wfbFrameCount_++;

    static int8_t rssi[4] = { 1, 1, 1, 1 };
    static uint8_t antenna[4] = { 1, 1, 1, 1 };

    std::lock_guard<std::mutex> lock(aggMutex);
    if (!videoAggregator) {
        return;
    }
    if (frame.MatchesChannelID(videoChannelIdBE8)) {
        videoAggregator->process_packet(
            packet.Data.data() + sizeof(ieee80211_header),
            packet.Data.size() - sizeof(ieee80211_header) - 4, 0,
            antenna, rssi);
    }
}

static socket_t sendFd = INVALID_SOCKET;
static std::atomic<bool> playing { false };

// H.264 / H.265 首包识别
// H.264：NAL Unit Header 第 0 字节低 5 位为 nal_unit_type (1..23 单 NAL, 24..29 聚合/分片)
// H.265：NAL Unit Header 第 0 字节 bit1~6 为 nal_unit_type (0..47 单 NAL, 48..50 聚合/分片)
// 因此只看第 0 字节末 5 位无法区分；需要用 H.265 专属 type 识别，或用 forbidden_zero_bit+layer 组合判定。
// 这里采用启发式：剥离可能的 FU/AP 头后看真实 NAL type。H.265 的 VPS(32)/SPS(33)/PPS(34)/IDR(19,20)
// 都 >= 16，而 H.264 的常见 type 不会超过 12（除了 24~29 的封装类型）。
static inline bool looksLikeH265(const uint8_t *nalu, size_t len) {
    if (len < 2) return false;
    uint8_t b0 = nalu[0];
    // H.264 的 forbidden_zero_bit 也是 bit7=0；这里主要看 type 字段
    uint8_t h264_type = b0 & 0x1F;
    uint8_t h265_type = (b0 >> 1) & 0x3F;
    // H.264 分片/聚合头之后真正的 type 在后面字节
    if (h264_type == 24 || h264_type == 28 || h264_type == 29) {
        // STAP-A / FU-A / FU-B，下一字节或下下字节才是真实 NAL type
        if (h264_type == 24 && len >= 4) {
            uint8_t real = nalu[3] & 0x1F;
            if (real >= 1 && real <= 12) return false;
        } else if ((h264_type == 28 || h264_type == 29) && len >= 2) {
            uint8_t real = nalu[1] & 0x1F;
            if (real >= 1 && real <= 12) return false;
        }
    }
    // H.265 关键 NAL type
    if (h265_type == 32 || h265_type == 33 || h265_type == 34 ||
        h265_type == 19 || h265_type == 20 || h265_type == 21 ||
        h265_type == 48 || h265_type == 49) {
        return true;
    }
    // 典型 H.264 范围
    if (h264_type >= 1 && h264_type <= 12) return false;
    // 默认落到 H.264
    return false;
}

void WFBReceiver::handleRtp(uint8_t *payload, uint16_t packet_size) {
    QmlNativeAPI::Instance().rtpPktCount_++;
    QmlNativeAPI::Instance().UpdateCount();
    if (stopping.load()) {
        return;
    }
    if (!rtlDevice || rtlDevice->should_stop) {
        return;
    }
    if (packet_size < 12) { // RTP 固定头 12 字节
        return;
    }

    sockaddr_in serverAddr {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(QmlNativeAPI::Instance().playerPort);
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");

    auto *header = (RtpHeader *)payload;

    bool expected = false;
    if (playing.compare_exchange_strong(expected, true)) {
        if (QmlNativeAPI::Instance().playerCodec == "AUTO") {
            uint8_t *nalu = header->getPayloadData();
            ssize_t naluLen = header->getPayloadSize(packet_size);
            if (naluLen < 0) naluLen = 0;
            bool h265 = looksLikeH265(nalu, static_cast<size_t>(naluLen));
            QmlNativeAPI::Instance().playerCodec = h265 ? "H265" : "H264";
            QmlNativeAPI::Instance().PutLog(
                "debug", "judge Codec " + QmlNativeAPI::Instance().playerCodec.toStdString());
        }
        QmlNativeAPI::Instance().NotifyRtpStream(header->pt, ntohl(header->ssrc));
    }

    // send video to player
    sendto(
        sendFd, reinterpret_cast<const char *>(payload), packet_size, 0,
        (sockaddr *)&serverAddr, sizeof(serverAddr));
}

bool WFBReceiver::Stop() {
    std::lock_guard<std::mutex> lock(stateMutex);
    if (stopping.exchange(true)) {
        // 已经在停
        return true;
    }
    playing = false;
    if (rtlDevice) {
        rtlDevice->should_stop = true;
    }
    // 不在此处 join/reset usbThread：usbThread 是 detach 的，它会在 Start() lambda
    // 退出时自行清理资源并通知 NotifyWifiStop。这里只做信号量通知。
    return true;
}

WFBReceiver::WFBReceiver() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed." << std::endl;
        return;
    }
#endif
    sendFd = socket(AF_INET, SOCK_DGRAM, 0);
}

WFBReceiver::~WFBReceiver() {
    if (sendFd != INVALID_SOCKET) {
        CLOSE_SOCKET(sendFd);
        sendFd = INVALID_SOCKET;
    }
#ifdef _WIN32
    WSACleanup();
#endif
    Stop();
}
