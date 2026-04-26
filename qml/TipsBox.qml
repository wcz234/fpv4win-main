import QtQuick 2.12
import QtQuick.Controls 2.5

Rectangle{
    id:tipsBox
    visible: false
    color:"#bb333333"
    width: Math.max(20, Math.min(tipText.implicitWidth + 20, parent ? parent.width - 40 : tipText.implicitWidth + 20))
    height: tipText.implicitHeight + 20
    anchors.verticalCenter: parent.verticalCenter
    anchors.horizontalCenter: parent.horizontalCenter
    property string tips: ''
    property var timeout: 3000
    radius: 5
    property var showPop : function(msg,time){
        tips = msg;
        hideTimer.interval = time?time:timeout
        tipsBox.visible = true;
        hideTimer.restart();
    }
    property var hide : function(){
        tipsBox.visible = false;
        tipsBox.tips = ""
        hideTimer.stop();
    }
    Timer {
        id:hideTimer
        interval: tipsBox.timeout;
        running: false;
        repeat: false;
        onTriggered: ()=>{
            tipsBox.hide();
        }
     }
    Text {
        id: tipText
        anchors.verticalCenter: parent.verticalCenter
        anchors.horizontalCenter: parent.horizontalCenter
        text: tipsBox.tips
        font.pointSize: 16
        color: "#ffffff"
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        width: Math.max(0, tipsBox.width - 20)
    }
}
