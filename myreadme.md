# fpv4win —— Windows 平台 WiFi Broadcast FPV 接收客户端

## 一、项目简介

`fpv4win` 是一个运行在 **Windows** 上的 FPV（First Person View，第一人称穿越机图传）地面站客户端。它通过 **RTL8812AU** 系列 USB WiFi 适配器，以监听模式（Monitor Mode）接收无人机（通常搭载 [OpenIPC](https://github.com/OpenIPC) 固件）通过 [wfb-ng](https://github.com/svpcom/wfb-ng) 协议广播出来的 **H.264 / H.265** 视频流，并在本地完成 FEC 解码、解密、RTP 组装、视频解码与实时渲染。

除实时预览外，还集成了 **截图、MP4 录像、GIF 录制、二维码识别** 等附加功能。

> 仅支持 RTL8812AU 芯片的 USB WiFi 网卡。

---

## 二、功能特性

- 列出当前系统中所有可用的 USB 设备（VID:PID），支持一键启动接收
- 支持 Channel（1–177，含 2.4G / 5G / DFS）、Channel Width（20/40/80/160/80_80/5/10/MAX）切换
- 支持加载 `gs.key` 密钥文件进行 wfb-ng 解密
- 支持 H264 / H265 / AUTO（根据首个 RTP 负载自动判别）三种编码
- 实时显示 **码率（bps/Kbps/Mbps）** 以及 **RTP / WFB / 802.11** 三级包计数
- WiFi 驱动日志实时滚动显示，按级别（error/info/warn/debug）着色
- **JPG 截图**：保存当前帧到 `jpg/<timestamp>.jpg`
- **MP4 录像**：直接透传原始 NALU，不二次编码，保存到 `mp4/<timestamp>.mp4`
- **GIF 录制**：按目标帧率跳帧压制 GIF
- **二维码识别**：基于 OpenCV，画面中二维码实时框选与文本显示（节流 200ms）
- 配置项（channel / width / codec / key）自动落盘到 `config.ini`，下次启动自动恢复
- Debug 构建下遇到未处理异常自动生成 `dump.dmp`（MiniDump）

---

## 三、整体架构与数据管线

```
┌──────────────────────────────────────────────────────────────────┐
│  RTL8812AU USB 网卡（Monitor Mode）                               │
└────────────────────────┬─────────────────────────────────────────┘
                         │ libusb-1.0
                         ▼
┌──────────────────────────────────────────────────────────────────┐
│  devourer 用户态驱动 (3rd/rtl8812au-monitor-pcap)                 │
│  —— WiFiDriver / Rtl8812aDevice                                   │
└────────────────────────┬─────────────────────────────────────────┘
                         │ Packet 回调（含 802.11 帧）
                         ▼
┌──────────────────────────────────────────────────────────────────┐
│  WFBReceiver::handle80211Frame                                   │
│  · RxFrame 过滤合法 WFB 帧                                        │
│  · Aggregator：ChaCha20-Poly1305 解密 + Reed-Solomon FEC 恢复      │
└────────────────────────┬─────────────────────────────────────────┘
                         │ 解出的 RTP 负载
                         ▼
┌──────────────────────────────────────────────────────────────────┐
│  handleRtp → UDP 127.0.0.1:52356                                 │
│  · 首包自动识别 H264/H265 → BuildSdp                              │
└────────────────────────┬─────────────────────────────────────────┘
                         │ FFmpeg OpenInput(sdp://...)
                         ▼
┌──────────────────────────────────────────────────────────────────┐
│  FFmpegDecoder（解封装 + 软/硬解码 + 重采样 + 码率统计）            │
└────────────────────────┬─────────────────────────────────────────┘
                         │ AVFrame 队列
         ┌───────────────┼───────────────────────────┐
         ▼               ▼                           ▼
┌──────────────┐ ┌────────────────┐     ┌────────────────────────┐
│ OpenGL 渲染   │ │ SDL2 音频播放   │     │ 录像/截图/GIF/二维码    │
│ RealTimeRend. │ │                 │     │ Mp4/Jpeg/Gif/OpenCV    │
└──────────────┘ └────────────────┘     └────────────────────────┘
```

---

## 四、目录结构

```
fpv4win-main/
├── CMakeLists.txt                   # 构建脚本（vcpkg + CMake）
├── README.md                        # 原始 README
├── qml.qrc                          # Qt 资源文件
├── gs.key                           # wfb-ng 解密密钥示例
├── img/                             # 截图示例
├── qml/
│   ├── main.qml                     # 主界面 UI
│   ├── RecordTimer.qml              # 录像计时器
│   └── TipsBox.qml                  # 提示气泡
├── 3rd/
│   └── rtl8812au-monitor-pcap/      # devourer 用户态驱动（Rtl8812aDevice/WiFiDriver）
└── src/
    ├── main.cpp                     # Qt 程序入口
    ├── QmlNativeAPI.h               # QML↔C++ 桥
    ├── util/                        # base64 / mINI 配置 / 通用工具
    ├── wifi/                        # 无线 & wfb-ng 协议处理
    │   ├── WFBReceiver.{h,cpp}      # USB 管理 + 802.11 帧入口
    │   ├── WFBProcessor.{h,cpp}     # Aggregator：解密 + FEC 重组
    │   ├── WFBDefine.h              # 协议常量与包头结构体
    │   ├── RxFrame.h / Rtp.h        # 帧/RTP 解析
    │   └── fec.{c,h}                # Reed-Solomon FEC 实现
    └── player/                      # 播放与后处理
        ├── QQuickRealTimePlayer.*   # QML 播放器控件（FBO + 多线程）
        ├── RealTimeRenderer.*       # OpenGL YUV/NV12 渲染
        ├── ffmpegDecode.*           # FFmpeg 解码封装
        ├── ffmpegInclude.h          # FFmpeg 头聚合
        ├── Mp4Encoder.*             # MP4 录制（透传 NALU）
        ├── GifEncoder.*             # GIF 录制
        ├── JpegEncoder.*            # JPG 截图
        └── QrCodeScanner.*          # OpenCV 二维码识别
```

---

## 五、核心实现细节

### 1. USB 与 WiFi 驱动层
- [src/wifi/WFBReceiver.cpp](src/wifi/WFBReceiver.cpp) 中 `GetDongleList()` 通过 `libusb_get_device_list` 枚举所有 USB 设备，并对常见 8812au 型号（`0bda:8812`、`0bda:881a`、`0b05:17d2`）做置顶排序。
- `Start()` 通过 `libusb_open_device_with_vid_pid` 打开目标设备，detach 内核驱动并 `libusb_claim_interface`，随后在独立线程中：
  - 构造 `WiFiDriver`（3rd/rtl8812au-monitor-pcap）
  - 调 `CreateRtlDevice(dev_handle)` 创建 `Rtl8812aDevice`
  - `Init()` 时传入 `SelectedChannel { channel, offset, width }` 和帧回调，正式进入 monitor 模式收包
- 所有 USB 和网卡的生命周期都在 `usbThread` 里自洽管理：停止时 `should_stop = true`，释放接口、关闭设备、退出 libusb。

### 2. wfb-ng 协议解析
- 每个 802.11 帧先用 `RxFrame::IsValidWfbFrame()` 过滤；合法者进入 `Aggregator::process_packet`。
- `Aggregator`（[src/wifi/WFBProcessor.h](src/wifi/WFBProcessor.h)）实现了：
  - 会话密钥协商：通过 `crypto_box_open_easy`（libsodium，Curve25519 + XSalsa20-Poly1305）用 `gs.key` 对应私钥解密对端公钥签发的 `wsession_data_t`，拿到 `chacha20poly1305` 会话密钥。
  - 数据包解密：`crypto_aead_chacha20poly1305_decrypt`。
  - **FEC（Reed-Solomon）** 重组：使用 [src/wifi/fec.c](src/wifi/fec.c)，维护大小为 `RX_RING_SIZE = 40` 的接收环，在一个 block 内缓存 k 个数据分片和 n-k 个冗余分片，丢包时通过范德蒙矩阵恢复。
- 恢复出的 `wpacket_hdr_t` 负载即为原始 RTP 包，通过回调 `handleRtp` 向外输出。

### 3. RTP → 本地 UDP → FFmpeg
- [`WFBReceiver::handleRtp`](src/wifi/WFBReceiver.cpp) 将 RTP 负载通过 `sendto` 送到 `127.0.0.1:52356`。
- 首包到来时：
  - 若用户选择 `AUTO`，读取首个 NAL 单元类型（`& 0x1F`，H.264 FU-A/STAP-A 特征为 24/28）判定 H264/H265；
  - 调 `NotifyRtpStream` 由 `QmlNativeAPI::BuildSdp` 写出一个最小 SDP：
    ```
    v=0
    o=- 0 0 IN IP4 127.0.0.1
    s=No Name
    c=IN IP4 127.0.0.1
    t=0 0
    m=video <port> RTP/AVP <pt>
    a=rtpmap:<pt> H264/90000
    ```
- QML 侧 `NativeApi.onRtpStream` 信号触发，`QQuickRealTimePlayer::play(sdpFile)` 开始用 FFmpeg 打开该 SDP 作为输入。

### 4. 播放与渲染
- [src/player/QQuickRealTimePlayer.cpp](src/player/QQuickRealTimePlayer.cpp) 继承 `QQuickFramebufferObject`，三线程协作：
  - **分析线程**：`FFmpegDecoder::OpenInput` 打开 SDP。
  - **解码线程**：循环 `GetNextFrame()`，产出的 `AVFrame` 入 `videoFrameQueue`（容量 10，溢出丢旧帧，用于追帧低延迟）。
  - **GL 渲染线程**：`TItemRender::synchronize` 在 Qt 定时器（100Hz）驱动下从队列取帧，`RealTimeRenderer::updateTextureData` 上传 YUV/NV12 纹理，着色器直接在 GPU 上做 YUV→RGB 转换。
- 音频通过 **SDL2** 以回调形式拉取 `FFmpegDecoder::ReadAudioBuff`（内部是 `AVFifo` 环形队列 + `SwrContext` 重采样到 `AUDIO_S16`）。

### 5. 录像 / 截图 / GIF / 二维码
- **MP4**（[Mp4Encoder](src/player/Mp4Encoder.cpp)）：通过 `decoder->_gotPktCallback` 获取尚未解码的 `AVPacket`，直接 `av_write_frame` 写入 MP4 容器，**不做二次编码**，从 I 帧起录以保证可播放。
- **JPG**（[JpegEncoder](src/player/JpegEncoder.cpp)）：取 `_lastFrame`（最近渲染的帧）用 FFmpeg 的 MJPEG 编码器编为 JPG。
- **GIF**（[GifEncoder](src/player/GifEncoder.cpp)）：按目标帧率（默认 10 fps）跳帧编码。
- **QR**（[QrCodeScanner](src/player/QrCodeScanner.cpp)）：OpenCV `QRCodeDetector::detectAndDecodeMulti`，在解码线程中以 200ms 节流运行，结果通过 `QMetaObject::invokeMethod` 投递回 GUI 线程，QML `Repeater` 在视频视口中画出边框 + 文本。

### 6. QML ↔ C++ 交互
- [src/QmlNativeAPI.h](src/QmlNativeAPI.h) 暴露给 QML 的方法：
  - `GetDongleList()` / `Start(vidPid, channel, width, keyPath, codec)` / `Stop()`
  - `GetConfig()` / `GetPlayerPort()` / `GetPlayerCodec()`
- 信号：`onLog / onWifiStop / onRtpStream / onWifiFrameCount / onWfbFrameCount / onRtpPktCount`
- `QQuickRealTimePlayer` 作为 QML 类型 `realTimePlayer.QQuickRealTimePlayer` 注册，QML 直接当作控件使用（[qml/main.qml](qml/main.qml)）。

---

## 六、用到的技术/第三方库

| 类别 | 组件 | 作用 |
|---|---|---|
| GUI 框架 | **Qt 5 Quick / QML** | 界面、属性绑定、信号槽 |
| GPU 渲染 | **OpenGL** via `QQuickFramebufferObject` | YUV/NV12 着色器渲染 |
| USB | **libusb-1.0** | 用户态访问 8812au |
| WiFi 驱动 | **devourer**（rtl8812au-monitor-pcap） | 用户态 RTL8812AU Monitor 驱动 |
| 链路层协议 | **wfb-ng** | 广播式 FPV 图传协议 |
| 加密 | **libsodium** | ChaCha20-Poly1305、crypto_box |
| FEC | **Reed-Solomon**（Vandermonde 矩阵） | 丢包恢复 |
| 音视频 | **FFmpeg** | RTP 解封装、H.264/H.265 解码、MP4/GIF/JPEG |
| 音频输出 | **SDL2** | 扬声器播放 |
| 计算机视觉 | **OpenCV**（core + objdetect） | 二维码识别 |
| 构建 | **CMake ≥ 3.20**、**vcpkg** | 依赖管理 |
| 系统 | Winsock (`ws2_32`)、DbgHelp（MiniDump） | UDP socket / 崩溃转储 |
| 工具 | **mINI** | `config.ini` 配置读写 |

---

## 七、构建与运行

### 构建依赖（建议用 vcpkg）
```
qt5-base qt5-declarative qt5-multimedia
ffmpeg[core,avcodec,avformat,swscale,swresample]
sdl2
opencv[core,objdetect]
libusb
libsodium
```

### CMake 配置关键
见 [CMakeLists.txt](CMakeLists.txt)：
- C++20
- `CMAKE_AUTOMOC / AUTORCC / AUTOUIC` 开启
- `find_package(FFmpeg / OpenCV / SDL2 / Qt5 / unofficial-sodium)`
- `pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)`
- Windows 下自动把 `qwindows.dll` 和 `Qt5Core.dll` 拷贝到输出目录

### 运行前准备
1. 下载 [Zadig](https://github.com/pbatard/libwdi/releases/download/v1.5.0/zadig-2.8.exe)；
2. 菜单 **Options → List All Devices**，选中你的 8812au 网卡，安装 **libusb-win32 / WinUSB** 驱动；
3. 安装 [vcredist_x64.exe](https://aka.ms/vs/17/release/vc_redist.x64.exe)；
4. 启动程序后，依次选择 **VID:PID / Channel / Channel Width / Codec / Key**，点击 **START**。

---

## 八、路线图（摘自原 README）

- [ ] OSD（屏显信息叠加）
- [ ] 硬件加速解码
- [x] MP4 录制
- [x] JPG 截图
- [ ] 推流到 RTMP / RTSP / SRT / WHIP
- [ ] 单网卡接收多路视频流
- [ ] ONVIF / GB28181 / SIP 客户端

---

## 九、参考项目

- [devourer](https://github.com/openipc/devourer) —— 用户态 rtl8812au 驱动
- [wfb-ng](https://github.com/svpcom/wfb-ng) —— WiFi Broadcast 协议实现
- [OpenIPC](https://github.com/OpenIPC) —— 开源 IP Camera / FPV 固件
