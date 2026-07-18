import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import HuskarUI.Basic
import Fantareal

Item {
    id: page

    property string operationMessage: ""
    property bool operationOk: true
    property string githubUrl: "https://github.com/cOkieeman/Fantareal-tavern-card-converter"
    property string pendingUninstallId: ""
    property string pendingUninstallName: ""

    ExtensionWebDialog {
        id: extensionWebDialog
        onOperationResult: result => page.showResult(result)
    }

    function showResult(result) {
        operationOk = Boolean(result.ok)
        operationMessage = String(result.message || "操作完成")
    }

    Connections {
        target: ExtensionManager

        function onOperationFinished(result) {
            page.showResult(result)
        }
    }

    FolderDialog {
        id: extensionFolderDialog
        title: "选择包含 fantareal-extension.json 的插件目录"
        onAccepted: page.showResult(ExtensionManager.installFromLocalDirectory(selectedFolder.toString()))
    }

    Dialog {
        id: uninstallDialog
        anchors.centerIn: parent
        modal: true
        width: Math.min(468, parent.width - 80)
        title: "卸载插件"
        standardButtons: Dialog.Yes | Dialog.No
        onAccepted: {
            page.showResult(ExtensionManager.uninstallExtension(page.pendingUninstallId))
            page.pendingUninstallId = ""
            page.pendingUninstallName = ""
        }
        onRejected: {
            page.pendingUninstallId = ""
            page.pendingUninstallName = ""
        }

        contentItem: HusText {
            text: `确定卸载「${page.pendingUninstallName}」？插件 package 会删除，Fantareal 主数据不会被修改。`
            wrapMode: Text.Wrap
            color: HusTheme.Primary.colorTextPrimary
            width: uninstallDialog.availableWidth
        }
    }

    Flickable {
        id: scroller
        anchors.fill: parent
        contentWidth: width
        contentHeight: pageColumn.implicitHeight + 88
        clip: true

        ColumnLayout {
            id: pageColumn
            width: Math.min(parent.width - 88, 1120)
            x: 44
            y: 44
            spacing: 20

            RowLayout {
                Layout.fillWidth: true
                spacing: 14

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    HusText {
                        text: "插件中心"
                        font.pixelSize: 34
                        font.weight: Font.DemiBold
                        color: HusTheme.Primary.colorTextPrimary
                    }

                    HusText {
                        text: "Extension Platform v1 · 本地目录安装"
                        font.pixelSize: 14
                        color: HusTheme.Primary.colorTextSecondary
                    }
                }

                HusButton {
                    text: "刷新"
                    enabled: !ExtensionManager.busy
                    onClicked: page.showResult(ExtensionManager.refresh())
                }

                HusButton {
                    text: "从本地目录安装"
                    type: HusButton.Type_Primary
                    enabled: !ExtensionManager.busy
                    onClicked: extensionFolderDialog.open()
                }
            }

            GlassCard {
                Layout.fillWidth: true
                Layout.preferredHeight: 228
                accentColor: Global.accentCyan

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 24
                    spacing: 10

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        StatPill {
                            text: "EXTENSION V1"
                            accentColor: Global.accentCyan
                        }
                        HusTag {
                            text: `${ExtensionManager.extensionCount} 个已安装插件`
                            tagState: HusTag.State_Processing
                        }
                        HusTag {
                            text: ExtensionManager.busy ? "正在处理 GitHub package" : "GitHub commit 固定安装"
                            tagState: ExtensionManager.busy ? HusTag.State_Processing : HusTag.State_Default
                        }
                    }

                    HusText {
                        Layout.fillWidth: true
                        text: "插件先复制到 staging，通过 manifest 与路径校验后才进入 immutable package。Python、TTS 和模型不会随主程序预装；声明 Python service 的插件会在首次运行时安装固定版本 runtime 与独立环境。"
                        wrapMode: Text.Wrap
                        color: HusTheme.Primary.colorTextSecondary
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        HusInput {
                            Layout.fillWidth: true
                            text: page.githubUrl
                            placeholderText: "https://github.com/<owner>/<repo>"
                            clearEnabled: "active"
                            iconSource: HusIcon.GithubOutlined
                            iconPosition: HusInput.Position_Left
                            enabled: !ExtensionManager.busy
                            onTextEdited: page.githubUrl = text
                        }

                        HusButton {
                            text: ExtensionManager.busy ? "安装中…" : "从 GitHub 安装"
                            type: HusButton.Type_Primary
                            enabled: !ExtensionManager.busy && page.githubUrl.trim().length > 0
                            onClicked: page.showResult(ExtensionManager.installFromGitHub(page.githubUrl))
                        }
                    }

                    HusText {
                        Layout.fillWidth: true
                        text: `数据目录：${ExtensionManager.rootPath}`
                        elide: Text.ElideMiddle
                        color: HusTheme.Primary.colorTextTertiary
                        font.pixelSize: 12
                    }
                }
            }

            GlassCard {
                visible: page.operationMessage.length > 0 || ExtensionManager.lastError.length > 0
                Layout.fillWidth: true
                Layout.preferredHeight: visible ? 72 : 0
                accentColor: page.operationOk && ExtensionManager.lastError.length === 0 ? Global.accentCyan : Global.accentPink

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 20
                    spacing: 12

                    HusTag {
                        text: page.operationOk && ExtensionManager.lastError.length === 0 ? "OK" : "ERROR"
                        tagState: page.operationOk && ExtensionManager.lastError.length === 0
                            ? HusTag.State_Processing
                            : HusTag.State_Default
                    }
                    HusText {
                        Layout.fillWidth: true
                        text: ExtensionManager.lastError.length > 0 ? ExtensionManager.lastError : page.operationMessage
                        wrapMode: Text.Wrap
                        color: HusTheme.Primary.colorTextPrimary
                    }
                    HusButton {
                        text: "关闭"
                        onClicked: page.operationMessage = ""
                    }
                }
            }

            GlassCard {
                visible: ExtensionManager.extensionCount === 0
                Layout.fillWidth: true
                Layout.preferredHeight: visible ? 180 : 0
                accentColor: Global.accentViolet

                ColumnLayout {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 48, 620)
                    spacing: 12

                    HusText {
                        Layout.alignment: Qt.AlignHCenter
                        text: "还没有安装插件"
                        font.pixelSize: 22
                        font.weight: Font.DemiBold
                    }
                    HusText {
                        Layout.fillWidth: true
                        text: "可以先选择 Fantareal-tavern-card-converter 的本地仓库目录进行验证。安装前不会执行其中的 Python 代码。"
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        color: HusTheme.Primary.colorTextSecondary
                    }
                }
            }

            Repeater {
                model: ExtensionManager.extensions

                delegate: GlassCard {
                    required property var modelData

                    Layout.fillWidth: true
                    Layout.preferredHeight: 238
                    accentColor: modelData.enabled ? Global.accentCyan : Global.accentViolet

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 24
                        spacing: 10

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10

                            HusText {
                                Layout.fillWidth: true
                                text: modelData.name
                                font.pixelSize: 22
                                font.weight: Font.DemiBold
                                color: HusTheme.Primary.colorTextPrimary
                            }
                            HusTag {
                                text: `v${modelData.version}`
                                tagState: HusTag.State_Default
                            }
                            HusTag {
                                text: modelData.enabled ? "已启用" : "已停用"
                                tagState: modelData.enabled ? HusTag.State_Processing : HusTag.State_Default
                            }
                            HusTag {
                                visible: modelData.hasPage
                                text: "WEB"
                                tagState: HusTag.State_Default
                            }
                            HusTag {
                                visible: modelData.hasService
                                text: "PYTHON"
                                tagState: HusTag.State_Default
                            }
                        }

                        HusText {
                            Layout.fillWidth: true
                            text: modelData.description || "没有提供插件说明"
                            wrapMode: Text.Wrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                            color: HusTheme.Primary.colorTextSecondary
                        }

                        HusText {
                            Layout.fillWidth: true
                            text: `${modelData.id} · ${modelData.publisher || "未知发布者"} · SHA-256 ${String(modelData.digest).slice(0, 12)}`
                            elide: Text.ElideRight
                            color: HusTheme.Primary.colorTextTertiary
                            font.pixelSize: 12
                        }

                        HusText {
                            visible: modelData.sourceType === "github"
                            Layout.fillWidth: true
                            text: `GitHub commit：${String(modelData.resolvedCommit).slice(0, 12)} · ${modelData.sourceUrl}`
                            elide: Text.ElideMiddle
                            color: HusTheme.Primary.colorTextTertiary
                            font.pixelSize: 12
                        }

                        HusText {
                            Layout.fillWidth: true
                            text: `权限：${modelData.permissions && modelData.permissions.length ? modelData.permissions.join(" · ") : "无"}`
                            elide: Text.ElideRight
                            color: HusTheme.Primary.colorTextTertiary
                            font.pixelSize: 12
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10

                            Item {
                                Layout.fillWidth: true
                            }
                            HusButton {
                                visible: Boolean(modelData.hasPage)
                                text: "打开"
                                type: HusButton.Type_Primary
                                enabled: !ExtensionManager.busy && Boolean(modelData.enabled)
                                onClicked: extensionWebDialog.openExtension(modelData.id)
                            }
                            HusButton {
                                visible: modelData.sourceType === "github"
                                text: "检查并安装最新版本"
                                enabled: !ExtensionManager.busy
                                onClicked: page.showResult(ExtensionManager.installFromGitHub(modelData.sourceUrl))
                            }
                            HusButton {
                                visible: Boolean(modelData.canRollback)
                                text: "回滚"
                                enabled: !ExtensionManager.busy
                                onClicked: page.showResult(ExtensionManager.rollbackExtension(modelData.id))
                            }
                            HusButton {
                                text: modelData.enabled ? "停用" : "启用"
                                enabled: !ExtensionManager.busy
                                onClicked: page.showResult(
                                    ExtensionManager.setExtensionEnabled(modelData.id, !modelData.enabled))
                            }
                            HusButton {
                                text: "卸载"
                                enabled: !ExtensionManager.busy
                                onClicked: {
                                    page.pendingUninstallId = modelData.id
                                    page.pendingUninstallName = modelData.name
                                    uninstallDialog.open()
                                }
                            }
                        }
                    }
                }
            }
        }

        ScrollBar.vertical: HusScrollBar { }
    }

    SmoothWheelArea {
        anchors.fill: scroller
        target: scroller
    }
}
