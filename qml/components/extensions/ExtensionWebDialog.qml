import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import QtWebChannel
import QtWebEngine
import HuskarUI.Basic
import Fantareal

Dialog {
    id: root

    signal operationResult(var result)

    function openExtension(extensionId) {
        const result = ExtensionWebHost.openExtension(extensionId)
        operationResult(result)
        if (result.ok)
            open()
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(1180, parent.width - 48)
    height: Math.min(820, parent.height - 48)
    modal: true
    closePolicy: Popup.CloseOnEscape
    padding: 0
    title: ExtensionWebHost.title

    onClosed: {
        extensionView.url = "about:blank"
        ExtensionWebHost.closeSession(ExtensionWebHost.sessionId)
    }

    FileDialog {
        id: inputFileDialog
        title: "选择插件输入文件"
        fileMode: FileDialog.OpenFile
        onAccepted: ExtensionWebHost.completeFileSelection(selectedFile)
        onRejected: ExtensionWebHost.cancelFileSelection()
    }

    QtObject {
        id: bridgeAdapter
        WebChannel.id: "fantarealExtensionBridge"

        signal responseReady(string requestId, bool ok, var result, string errorCode, string errorMessage)

        function request(extensionId, sessionId, method, params) {
            return ExtensionWebHost.request(extensionId, sessionId, method, params || {})
        }
    }

    WebChannel {
        id: extensionChannel
        registeredObjects: [bridgeAdapter]
    }

    Connections {
        target: ExtensionWebHost

        function onResponseReady(requestId, ok, result, errorCode, errorMessage) {
            bridgeAdapter.responseReady(requestId, ok, result, errorCode, errorMessage)
        }

        function onFileSelectionRequested(nameFilters) {
            inputFileDialog.nameFilters = nameFilters
            inputFileDialog.open()
        }

        function onCloseRequested() {
            root.close()
        }
    }

    background: Rectangle {
        radius: 14
        color: HusTheme.Primary.colorBgBase
        border.width: 1
        border.color: HusTheme.Primary.colorBorder
    }

    contentItem: ColumnLayout {
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 60
            Layout.leftMargin: 20
            Layout.rightMargin: 12
            spacing: 10

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                HusText {
                    text: ExtensionWebHost.title
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                    color: HusTheme.Primary.colorTextPrimary
                }
                HusText {
                    text: `${ExtensionWebHost.extensionId} · isolated Web session`
                    font.pixelSize: 11
                    color: HusTheme.Primary.colorTextTertiary
                }
            }

            HusTag {
                text: "NETWORK OFF"
                tagState: HusTag.State_Default
            }

            HusButton {
                text: "关闭"
                onClicked: root.close()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: HusTheme.Primary.colorBorder
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            WebEngineView {
                id: extensionView
                anchors.fill: parent
                url: ExtensionWebHost.pageUrl
                webChannel: extensionChannel
                backgroundColor: HusTheme.Primary.colorBgBase
                activeFocusOnPress: true

                settings.javascriptEnabled: true
                settings.javascriptCanOpenWindows: false
                settings.javascriptCanAccessClipboard: false
                settings.localContentCanAccessFileUrls: false
                settings.localContentCanAccessRemoteUrls: false
                settings.fullScreenSupportEnabled: false
                settings.screenCaptureEnabled: false
                settings.pluginsEnabled: false
                settings.unknownUrlSchemePolicy: WebEngineSettings.DisallowUnknownUrlSchemes

                onNavigationRequested: function(request) {
                    request.accepted = ExtensionWebHost.isAllowedNavigation(request.url)
                }

                onLoadingChanged: function(loadRequest) {
                    if (loadRequest.status === WebEngineView.LoadFailedStatus)
                        ExtensionWebHost.reportPageLoadFailure(
                            ExtensionWebHost.sessionId,
                            `${loadRequest.errorString} (${loadRequest.errorCode})`)
                }

                onNewWindowRequested: function(request) {
                    // Ignoring the request denies popups and external windows.
                }
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: visible ? 48 : 0
                visible: ExtensionWebHost.lastError.length > 0
                color: Global.accentPink
                opacity: 0.94

                HusText {
                    anchors.fill: parent
                    anchors.margins: 12
                    text: ExtensionWebHost.lastError
                    wrapMode: Text.Wrap
                    color: "white"
                }
            }
        }
    }
}
