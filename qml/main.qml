import QtQuick 2.15
import QtQuick.Controls 2.15
import realTimePlayer 1.0
import Qt.labs.platform 1.1


ApplicationWindow {
    visible: true
    width: 1024
    height: 768
    id:window
    title: qsTr("")



    QQuickRealTimePlayer {
        x: 0
        y: 0
        id: player
        width: parent.width - 200
        height:parent.height
        property var playingFile
        // FIX: 重连定时器，避免无限死循环占满 CPU
        Timer {
            id: reconnectTimer
            interval: 500
            repeat: false
            onTriggered: {
                if (player.playingFile) {
                    player.stop();
                    player.play(player.playingFile);
                }
            }
        }
        Component.onCompleted: {
            NativeApi.onRtpStream.connect((sdpFile)=>{
                playingFile = sdpFile;
                play(sdpFile)
            });
            onPlayStopped.connect(()=>{
                // 500ms 后再重连，避免 RTP 未到 / 文件刚关闭时立即重开死循环
                if (playingFile) {
                    reconnectTimer.restart();
                }
            });
            NativeApi.onWifiStop.connect(()=>{
                // WiFi 接收已停止时不要再尝试重连
                reconnectTimer.stop();
                playingFile = null;
            });
        }
        TipsBox{
            id:tips
            z:999
            tips:''
        }
        Text {
            z: 900
            anchors.centerIn: parent
            visible: player.videoFrameWidth <= 0 || player.videoFrameHeight <= 0
            text: "Waiting for video stream"
            color: "#d9ffffff"
            font.pixelSize: 22
            horizontalAlignment: Text.AlignHCenter
        }
        Item {
            id: qrOverlay
            z: 950
            anchors.fill: parent
            visible: player.qrScanEnabled
            clip: true

            function videoViewport() {
                let vw = player.videoFrameWidth;
                let vh = player.videoFrameHeight;
                if (vw <= 0 || vh <= 0) {
                    return Qt.rect(0, 0, player.width, player.height);
                }
                let scale = Math.min(player.width / vw, player.height / vh);
                let viewWidth = vw * scale;
                let viewHeight = vh * scale;
                return Qt.rect((player.width - viewWidth) / 2, (player.height - viewHeight) / 2, viewWidth, viewHeight);
            }

            Repeater {
                model: player.qrCodes
                delegate: Item {
                    id: qrItem
                    readonly property var viewport: qrOverlay.videoViewport()
                    readonly property var qrPoints: modelData.points || []
                    x: viewport.x + (modelData.x || 0) * viewport.width
                    y: viewport.y + (modelData.y || 0) * viewport.height
                    width: (modelData.width || 0) * viewport.width
                    height: (modelData.height || 0) * viewport.height

                    Canvas {
                        id: qrFrame
                        anchors.fill: parent
                        antialiasing: true

                        onPaint: {
                            const ctx = getContext("2d");
                            ctx.clearRect(0, 0, width, height);
                            ctx.strokeStyle = "#1cff2f";
                            ctx.lineWidth = 4;
                            ctx.lineJoin = "round";
                            ctx.lineCap = "round";
                            ctx.beginPath();

                            if (qrItem.qrPoints.length >= 4) {
                                ctx.moveTo(qrItem.qrPoints[0].x * width, qrItem.qrPoints[0].y * height);
                                for (let i = 1; i < 4; ++i) {
                                    ctx.lineTo(qrItem.qrPoints[i].x * width, qrItem.qrPoints[i].y * height);
                                }
                                ctx.closePath();
                            } else {
                                ctx.rect(2, 2, Math.max(0, width - 4), Math.max(0, height - 4));
                            }

                            ctx.stroke();
                        }

                        Component.onCompleted: requestPaint()
                        onWidthChanged: requestPaint()
                        onHeightChanged: requestPaint()
                    }

                    Rectangle {
                        id: qrLabel
                        x: 0
                        y: qrItem.y > 34 ? -34 : Math.min(qrItem.height + 4, qrOverlay.height - qrItem.y - height)
                        width: Math.min(qrOverlay.width - qrItem.x, Math.max(48, qrText.implicitWidth + 14))
                        height: Math.max(28, qrText.implicitHeight + 8)
                        radius: 4
                        color: "#cc063b16"
                        border.color: "#1cff2f"
                        border.width: 1

                        Text {
                            id: qrText
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: 7
                            anchors.rightMargin: 7
                            text: (index + 1) + ": " + (modelData.text || "")
                            color: "#ffffff"
                            font.pixelSize: 16
                            font.bold: true
                            elide: Text.ElideRight
                            horizontalAlignment: Text.AlignLeft
                        }
                    }
                }
            }
        }
        Rectangle {
            width: parent.width
            height:30
            anchors.bottom : parent.bottom
            color: Qt.rgba(0,0,0,0.3)
            border.color: "#55222222"
            border.width: 1
            Row{
                height:parent.height
                padding:5
                spacing:5
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "0bps"
                    font.pixelSize: 12
                    width:60
                    horizontalAlignment: Text.Center
                    color: "#ffffff"
                    Component.onCompleted: {
                        player.onBitrate.connect((btr)=>{
                            if(btr>1000*1000){
                                text = Number(btr/1000/1000).toFixed(2) + 'Mbps';
                            }else if(btr>1000){
                                text = Number(btr/1000).toFixed(2) + 'Kbps';
                            }else{
                                text = btr+ 'bps';
                            }
                        });
                    }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 60
                    horizontalAlignment: Text.AlignHCenter
                    font.pixelSize: 12
                    color: "#ffffff"
                    text: player.qrScanEnabled ? ("QR " + player.qrCodes.length) : "QR OFF"
                }


            }
            Row{
                anchors.right:parent.right
                height:parent.height
                padding:5
                spacing:5
                Rectangle {
                    height:20
                    width:30
                    radius:5
                    color: "#55222222"
                    border.color: "#88ffffff"
                    border.width: 1
                    Text {
                        horizontalAlignment: Text.Center
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "JPG"
                        font.pixelSize: 12
                        color: "#ffffff"
                    }
                    MouseArea {
                        cursorShape: Qt.PointingHandCursor
                        anchors.fill: parent
                        onClicked:{
                            let f = player.captureJpeg();
                            if(f!==''){
                                tips.showPop('Saved '+f,3000);
                            }else{
                                tips.showPop('Capture failed! '+f,3000);
                            }
                        }
                    }
                }
                Rectangle {
                    height: 20
                    width: 50
                    radius: 5
                    color: "#55222222"
                    border.color: "#88ffffff"
                    border.width: 1
                    Text {
                        visible:!recordTimer.started
                        horizontalAlignment: Text.Center
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "MP4"
                        font.pixelSize: 12
                        color: "#ffffff"
                    }
                    RecordTimer{
                        id:recordTimer
                        width:parent.width
                        height: parent.height
                        property bool started:false
                        function clickEvent() {
                            if(!recordTimer.started){
                                recordTimer.started = player.startRecord();
                                if(recordTimer.started){
                                    recordTimer.start();
                                }else{
                                    tips.showPop('Record failed! ',3000);
                                }
                            }else{
                                recordTimer.started = false;
                                let f = player.stopRecord();
                                if(f!==''){
                                    tips.showPop('Saved '+f,3000);
                                }else{
                                    tips.showPop('Record failed! ',3000);
                                }
                                recordTimer.stop();
                            }
                        }
                    }
                    MouseArea {
                        cursorShape: Qt.PointingHandCursor
                        anchors.fill: parent
                        onClicked:{
                            recordTimer.clickEvent();
                        }
                    }
                }
                Rectangle {
                    height: 20
                    width: 58
                    radius: 5
                    color: player.qrScanEnabled ? "#3358b36c" : "#55222222"
                    border.color: player.qrScanEnabled ? "#1cff97" : "#88ffffff"
                    border.width: 1
                    Text {
                        horizontalAlignment: Text.Center
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: player.qrScanEnabled ? ("QR " + player.qrCodes.length) : "QR OFF"
                        font.pixelSize: 12
                        color: "#ffffff"
                    }
                    MouseArea {
                        cursorShape: Qt.PointingHandCursor
                        anchors.fill: parent
                        onClicked: {
                            player.qrScanEnabled = !player.qrScanEnabled;
                        }
                    }
                }
            }
        }
    }
    Rectangle {
        x: parent.width - 200
        y: 0
        width: 200
        height: parent.height
        color: '#cccccc'


        Column {
            padding: 5
            anchors.left: parent.left

            Rectangle {
                // Size of the background adapts to the text size plus some padding
                width: 190
                height: selDevText.height + 10
                color: "#1c80c9"

                Text {
                    id: selDevText
                    x: 5
                    anchors.verticalCenter: parent.verticalCenter
                    text: "RTL8812AU VID:PID"
                    font.pixelSize: 16
                    color: "#ffffff"
                }
            }
            ComboBox {
                id: selectDev
                width: 190
                model: ListModel {
                    id: comboBoxModel
                    Component.onCompleted: {
                        var dongleList = NativeApi.GetDongleList();
                        for (var i = 0; i < dongleList.length; i++) {
                            comboBoxModel.append({text: dongleList[i]});
                        }
                        selectDev.currentIndex = 0; // Set default selection
                    }
                }
                currentIndex: 0
            }
            Row{
                width: 190
                Column {
                    width:95
                    Rectangle {
                        // Size of the background adapts to the text size plus some padding
                        width: parent.width
                        height: selChText.height + 10
                        color: "#1c80c9"

                        Text {
                            width: parent.width
                            id: selChText
                            x: 5
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Channel"
                            font.pixelSize: 16
                            color: "#ffffff"
                        }
                    }
                    ComboBox {
                        id: selectChannel
                        width: parent.width
                        model: [
                            '1','2','3','4','5','6','7','8','9','10','11','12','13',
                            '32','36','40','44','48','52','56','60','64','68','96','100','104','108','112','116','120',
                            '124','128','132','136','140','144','149','153','157','161','169','173','177'
                        ]
                        currentIndex: 39
                        Component.onCompleted: {
                            let ch = NativeApi.GetConfig()["config.channel"];
                            if(ch&&ch!==''){
                                currentIndex = model.indexOf(ch);
                            }
                        }
                    }
                }
                Column {
                    width:95
                    Rectangle {
                        // Size of the background adapts to the text size plus some padding
                        width: parent.width
                        height: selCodecText.height + 10
                        color: "#1c80c9"

                        Text {
                            width: parent.width
                            id: selCodecText
                            x: 5
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Codec"
                            font.pixelSize: 16
                            color: "#ffffff"
                        }
                    }
                    ComboBox {
                        id: selectCodec
                        width: parent.width
                        model: ['AUTO','H264','H265']
                        currentIndex: 0
                        Component.onCompleted: {
                            let codec = NativeApi.GetConfig()["config.codec"];
                            if (codec&&codec !== '') {
                                currentIndex = model.indexOf(codec);
                            }
                        }
                    }
                }
            }
            Column {
                width:190
                Rectangle {
                    // Size of the background adapts to the text size plus some padding
                    width: parent.width
                    height: selBwText.height + 10
                    color: "#1c80c9"

                    Text {
                        width: parent.width
                        id: selBwText
                        x: 5
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Channel Width"
                        font.pixelSize: 16
                        color: "#ffffff"
                    }
                }
                ComboBox {
                    id: selectBw
                    width: parent.width
                    model: [
                        '20',
                        '40',
                        '80',
                        '160',
                        '80_80',
                        '5',
                        '10',
                        'MAX'
                    ]
                    currentIndex: 0
                    Component.onCompleted: {
                        let chw = NativeApi.GetConfig()["config.channelWidth"];
                        if (chw&&chw !== '') {
                            currentIndex = Number(chw);
                        }
                    }
                }
            }
            Rectangle {
                // Size of the background adapts to the text size plus some padding
                width: 190
                height: actionText.height + 10
                color: "#1c80c9"

                Text {
                    id: keyText
                    x: 5
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Key"
                    font.pixelSize: 16
                    color: "#ffffff"
                }
            }
            Column {
                FileDialog {
                    id: fileDialog
                    title: "Select key File"
                    nameFilters: ["Key Files (*.key)"]

                    onAccepted: {
                        keySelector.text = file;
                        keySelector.text = keySelector.text.replace('file:///','')
                    }
                }
                Button {
                    width: 190
                    id:keySelector
                    text: "gs.key"
                    onClicked: fileDialog.open()
                    Component.onCompleted: {
                        let key = NativeApi.GetConfig()["config.key"];
                        if (key && key !== '') {
                            text = key;
                        }
                    }
                }
            }
            Rectangle {
                // Size of the background adapts to the text size plus some padding
                width: 190
                height: actionText.height + 10
                color: "#1c80c9"

                Text {
                    id: actionText
                    x: 5
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Action"
                    font.pixelSize: 16
                    color: "#ffffff"
                }
            }
            Column {
                padding:5
                Rectangle {
                    // Size of the background adapts to the text size plus some padding
                    width: 180
                    height: actionStartText.height + 10
                    color: "#2fdcf3"
                    radius: 10

                    Text {
                        id: actionStartText
                        property bool started : false;
                        x: 5
                        anchors.centerIn: parent
                        text: started?"STOP":"START"
                        font.pixelSize: 32
                        color: "#ffffff"
                    }
                    MouseArea{
                        cursorShape: Qt.PointingHandCursor
                        anchors.fill: parent
                        Component.onCompleted: {
                            NativeApi.onWifiStop.connect(()=>{
                                actionStartText.started = false;
                                player.stop();
                            });
                        }
                        onClicked: function(){
                            if(!actionStartText.started){
                                actionStartText.started = NativeApi.Start(
                                    selectDev.currentText,
                                    Number(selectChannel.currentText),
                                    Number(selectBw.currentIndex),
                                    keySelector.text,
                                    selectCodec.currentText
                                );
                            }else{
                                NativeApi.Stop();
                                player.stop();
                                if(recordTimer.started){
                                    recordTimer.clickEvent();
                                }
                            }
                        }
                    }
                }
            }
            Rectangle {
                // Size of the background adapts to the text size plus some padding
                width: 190
                height: countText.height + 10
                color: "#1c80c9"

                Text {
                    id: countText
                    x: 5
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Packet(RTP/WFB/802.11)"
                    font.pixelSize: 16
                    color: "#ffffff"
                }
            }
            Row {
                padding:5
                width: 190
                Text {
                    id: rtpPktCountText
                    x: 5
                    text: ""+NativeApi.rtpPktCount
                    font.pixelSize: 16
                    color: "#000000"
                }
                Text {
                    x: 5
                    text: "/"
                    font.pixelSize: 16
                    color: "#000000"
                }
                Text {
                    id: wfbPktCountText
                    x: 5
                    text: ""+NativeApi.wfbFrameCount
                    font.pixelSize: 16
                    color: "#000000"
                }
                Text {
                    x: 5
                    text: "/"
                    font.pixelSize: 16
                    color: "#000000"
                }
                Text {
                    id: airPktCountText
                    x: 5
                    text: ""+NativeApi.wifiFrameCount
                    font.pixelSize: 16
                    color: "#000000"
                }
            }
            Rectangle {
                id:logTitle
                z:2
                // Size of the background adapts to the text size plus some padding
                width: 190
                height: logText.height + 10
                color: "#1c80c9"

                Text {
                    id: logText
                    x: 5
                    anchors.verticalCenter: parent.verticalCenter
                    text: "WiFi Driver Log"
                    font.pixelSize: 16
                    color: "#FFFFFF"
                }
            }
            Rectangle {
                width:190
                height:window.height - 430
                color:"#f3f1f1"
                clip:true

                Component {
                    id: contactDelegate
                    Item {
                        height:log.height
                        Row {
                            padding:2
                            Text {
                                id:log
                                width: 190
                                wrapMode: Text.Wrap
                                font.pixelSize: 10
                                text: '['+level+'] '+msg
                                color: {
                                    let colors = {
                                        error: "#ff0000",
                                        info: "#0f7340",
                                        warn: "#e8c538",
                                        debug: "#3296de",
                                    }
                                    return colors[level];
                                }
                            }
                        }
                    }
                }

                ListView {
                    z:1
                    anchors.fill: parent
                    anchors.margins:5
                    model: ListModel {}
                    delegate: contactDelegate
                    Component.onCompleted: {
                        NativeApi.onLog.connect((level,msg)=>{
                            model.append({"level": level, "msg": msg});
                            positionViewAtIndex(count - 1, ListView.End)
                        });
                    }
                }
            }
        }
    }
}
