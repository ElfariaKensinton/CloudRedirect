import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Page {
    title: "Setup"

    Dialog {
        id: purgeDialog
        title: "Remove All Data"
        modal: true
        standardButtons: Dialog.NoButton
        anchors.centerIn: parent
        width: Math.min(parent.width - 80, 480)

        property int countdown: 5
        property bool canPurge: false

        Timer {
            id: purgeCountdownTimer
            interval: 1000
            repeat: true
            onTriggered: {
                purgeDialog.countdown--
                if (purgeDialog.countdown <= 0) {
                    purgeCountdownTimer.stop()
                    purgeDialog.canPurge = true
                }
            }
        }

        onOpened: {
            countdown = 5
            canPurge = false
            purgeCountdownTimer.start()
        }

        onClosed: {
            purgeCountdownTimer.stop()
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 10

            Label {
                text: "This will permanently remove:"
                font.bold: true
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            ColumnLayout {
                spacing: 2
                Layout.leftMargin: 12

                Label { text: "• CloudRedirect from Steam (LD_AUDIT hook)"; opacity: 0.8; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                Label { text: "• cloud_redirect.so and CLI binary"; opacity: 0.8; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                Label { text: "• All configuration and settings"; opacity: 0.8; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                Label { text: "• OAuth tokens (Google Drive / OneDrive)"; opacity: 0.8; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                Label { text: "• All cached saves and cloud data"; opacity: 0.8; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                Label { text: "• Logs and backups"; opacity: 0.8; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            }

            Label {
                text: "This cannot be undone. You will need to re-authenticate and re-deploy to use CloudRedirect again."
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                opacity: 0.7
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 4

                Item { Layout.fillWidth: true }

                Button {
                    text: "Cancel"
                    onClicked: purgeDialog.close()
                }

                Button {
                    text: purgeDialog.canPurge ? "Remove All Data" : "Remove (" + purgeDialog.countdown + ")"
                    enabled: purgeDialog.canPurge
                    onClicked: {
                        if (deployer) deployer.purgeAll()
                        purgeDialog.close()
                    }
                }
            }
        }
    }

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth

        ColumnLayout {
            width: parent.width
            spacing: 12

            Item { height: 8 }

            Label {
                text: "Setup"
                font.pointSize: 16
                font.bold: true
                Layout.leftMargin: 20
            }

            Label {
                text: "v" + (backend ? backend.version : "")
                Layout.leftMargin: 20
                opacity: 0.7
            }

            Label {
                text: "Deploy or remove CloudRedirect."
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                opacity: 0.7
            }

            // Target app IDs
            Frame {
                Layout.fillWidth: true
                Layout.leftMargin: 20
                Layout.rightMargin: 20

                ColumnLayout {
                    width: parent.width
                    spacing: 6

                    Label { text: "Preseeded AppIDs (optional)"; font.bold: true }
                    Label {
                        text: "Optional AppIDs to preseed local/cloud stats. macOS CloudRedirect otherwise handles all non-zero Steam AppIDs."
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        opacity: 0.7
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        TextField {
                            id: targetAppIdField
                            placeholderText: "AppID, e.g. 123456"
                            inputMethodHints: Qt.ImhDigitsOnly
                            validator: RegularExpressionValidator { regularExpression: /^[1-9][0-9]{0,9}$/ }
                            Layout.fillWidth: true
                        }
                        Button {
                            text: "Add"
                            enabled: backend && targetAppIdField.acceptableInput
                            onClicked: {
                                if (backend && backend.addTargetApp(Number(targetAppIdField.text)))
                                    targetAppIdField.clear()
                            }
                        }
                    }

                    Repeater {
                        model: backend ? backend.targetAppIds : []
                        delegate: Frame {
                            Layout.fillWidth: true
                            RowLayout {
                                anchors.fill: parent
                                Label { text: String(modelData); font.family: "monospace"; Layout.fillWidth: true }
                                Button {
                                    text: "Remove"
                                    onClicked: { if (backend) backend.removeTargetApp(Number(modelData)) }
                                }
                            }
                        }
                    }
                }
            }

            // CloudRedirect status
            Frame {
                Layout.fillWidth: true
                Layout.leftMargin: 20
                Layout.rightMargin: 20

                ColumnLayout {
                    width: parent.width
                    spacing: 4

                    Label {
                        text: "CloudRedirect"
                        font.bold: true
                    }
                    Label {
                        text: {
                            if (!deployer) return "Unknown"
                            if (!deployer.alreadyDeployed) return "Not deployed"
                            if (deployer.updateAvailable) return "Update available"
                            return "Deployed, up to date"
                        }
                        opacity: 0.7
                    }
                    Label {
                        visible: deployer && deployer.updateAvailable && deployer.deployedVersion && deployer.bundledVersion
                        text: deployer ? (deployer.deployedVersion + " → " + deployer.bundledVersion) : ""
                        font.family: "monospace"
                        opacity: 0.6
                    }
                }
            }

            // Status message
            Label {
                text: deployer ? deployer.statusMessage : ""
                visible: deployer && deployer.statusMessage !== ""
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                opacity: 0.7
            }

            // Action buttons
            RowLayout {
                Layout.leftMargin: 20
                spacing: 8

                Button {
                    text: "Deploy"
                    enabled: deployer && !deployer.alreadyDeployed && !deployer.steamRunning
                    highlighted: deployer && !deployer.alreadyDeployed && !deployer.steamRunning
                    onClicked: { if (deployer) deployer.deploy() }
                }

                Button {
                    text: "Update"
                    visible: deployer && deployer.updateAvailable
                    highlighted: true
                    onClicked: { if (deployer) deployer.update() }
                }

                Button {
                    text: "Remove"
                    enabled: deployer && deployer.alreadyDeployed
                    onClicked: { if (deployer) deployer.undeploy() }
                }

                Button {
                    text: "Launch Steam with CloudRedirect"
                    visible: deployer && deployer.alreadyDeployed
                    enabled: deployer && deployer.alreadyDeployed && !deployer.steamRunning
                    highlighted: deployer && deployer.alreadyDeployed && !deployer.steamRunning
                    onClicked: { if (deployer) deployer.launchSteamWithCloudRedirect() }
                }

                Button {
                    text: "Remove All Data"
                    onClicked: purgeDialog.open()
                }

                Button {
                    text: "Open Log"
                    onClicked: { if (deployer) deployer.openLog() }
                }

                Button {
                    text: "Refresh"
                    onClicked: { if (deployer) deployer.checkPrerequisites() }
                }
            }

            Item { Layout.fillHeight: true }
        }
    }
}
