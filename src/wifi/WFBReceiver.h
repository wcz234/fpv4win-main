//
// Created by Talus on 2024/6/10.
//

#ifndef WFBRECEIVER_H
#define WFBRECEIVER_H
#include "FrameParser.h"
#include "Rtl8812aDevice.h"
#include "WFBProcessor.h"
#include <QUdpSocket>
#include <atomic>
#include <libusb.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class WFBReceiver {
public:
    WFBReceiver();
    ~WFBReceiver();
    static WFBReceiver &Instance() {
        static WFBReceiver wfb_receiver;
        return wfb_receiver;
    }
    std::vector<std::string> GetDongleList();
    bool Start(const std::string &vidPid, uint8_t channel, int channelWidth, const std::string &keyPath);
    bool Stop();
    void handle80211Frame(const Packet &pkt);
    void handleRtp(uint8_t *payload, uint16_t packet_size);

protected:
    libusb_context *ctx = nullptr;
    libusb_device_handle *dev_handle = nullptr;
    std::shared_ptr<std::thread> usbThread;
    std::unique_ptr<Rtl8812aDevice> rtlDevice;
    std::string keyPath;

    // 每次 Start 时重建的会话状态
    std::mutex aggMutex;
    std::unique_ptr<Aggregator> videoAggregator;
    uint32_t videoChannelIdBE = 0;
    uint8_t videoChannelIdBE8[4] = { 0 };

    // 并发/重入控制
    std::mutex stateMutex;
    std::atomic<bool> stopping { false };
    std::atomic<bool> running { false };
};

#endif // WFBRECEIVER_H
