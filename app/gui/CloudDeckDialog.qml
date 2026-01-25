import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import CloudDeckManager 1.0
import ComputerManager 1.0
import ComputerModel 1.0

NavigableDialog {
    id: cloudDeckDialog

    property ComputerModel computerModel
    property bool busy: false
    property bool manualStartInProgress: false
    property bool pairingFlowInProgress: false
    property bool awaitingAddComplete: false
    property bool hasStoredCredentials: false
    property bool autoStartInstance: false
    property string pendingAddress: ""
    property string statusText: ""
    property string errorText: ""
    property int retryCount: 0
    property int maxRetries: 10

    title: qsTr("CloudDeck")

    function resetState() {
        busy = false
        manualStartInProgress = false
        pairingFlowInProgress = false
        awaitingAddComplete = false
        pendingAddress = ""
        statusText = ""
        errorText = ""
        retryCount = 0
        pairingRetryTimer.stop()
    }

    function failWithError(message) {
        errorText = message
        statusText = ""
        busy = false
        manualStartInProgress = false
        pairingFlowInProgress = false
        awaitingAddComplete = false
        retryCount = 0
        pairingRetryTimer.stop()
    }

    function startPairing() {
        var emailValue = emailField.text.trim()
        if (emailValue.length === 0 || passwordField.text.length === 0) {
            failWithError(qsTr("Please enter your CloudDeck email and password."))
            return
        }

        resetState()
        busy = true
        statusText = qsTr("Connecting to CloudDeck...")
        CloudDeckManager.startPairingWithCredentials(emailValue, passwordField.text)
    }

    function startInstance() {
        var emailValue = emailField.text.trim()
        var hasInputCredentials = emailValue.length > 0 && passwordField.text.length > 0

        if (!hasInputCredentials && !hasStoredCredentials) {
            failWithError(qsTr("Please enter your CloudDeck email and password."))
            return
        }

        resetState()
        busy = true
        manualStartInProgress = true
        statusText = qsTr("Starting CloudDeck instance...")

        if (hasInputCredentials) {
            CloudDeckManager.startInstanceWithCredentials(emailValue, passwordField.text)
        } else {
            CloudDeckManager.startCloudDeckInstance()
        }
    }

    function tryStartPairing() {
        if (!computerModel || pendingAddress.length === 0) {
            return false
        }

        var index = computerModel.findComputerByManualAddress(pendingAddress)
        if (index < 0) {
            return false
        }

        var pin = computerModel.generatePinString()
        statusText = qsTr("Pairing with CloudDeck...")
        computerModel.pairComputer(index, pin)
        CloudDeckManager.enterPinAndPair(pin)
        awaitingAddComplete = false
        pairingFlowInProgress = true
        return true
    }

    onOpened: {
        resetState()
        hasStoredCredentials = CloudDeckManager.hasStoredCredentials()
        emailField.text = CloudDeckManager.getStoredEmail()
        passwordField.text = CloudDeckManager.getStoredPassword()
        emailField.forceActiveFocus()

        if (autoStartInstance) {
            autoStartInstance = false
            startInstance()
        }
    }

    onClosed: {
        if (busy) {
            CloudDeckManager.cancelCurrentOperation()
        }
        resetState()
    }

    Timer {
        id: pairingRetryTimer
        interval: 1000
        repeat: true
        running: false

        onTriggered: {
            if (tryStartPairing()) {
                stop()
                return
            }

            retryCount += 1
            if (retryCount >= maxRetries) {
                stop()
                failWithError(qsTr("Unable to find the CloudDeck host in Moonlight. Please try again."))
            }
        }
    }

    Connections {
        target: CloudDeckManager

        function onLoginCompleted(success, error) {
            if (!success) {
                failWithError(error)
                return
            }
            hasStoredCredentials = true
        }

        function onPairingStatusChanged(status) {
            statusText = status
        }

        function onInstanceStatusChanged(status) {
            statusText = status
            if (status.startsWith("Error:") || status.startsWith("Timeout:")) {
                failWithError(status)
            }
        }

        function onServerAddressReady(serverAddress) {
            pendingAddress = serverAddress
            statusText = qsTr("Found CloudDeck host %1. Adding to Moonlight...").arg(serverAddress)
            awaitingAddComplete = true
            ComputerManager.addNewHostManually(serverAddress)
        }

        function onPairingCompleted(success, error) {
            if (!success) {
                failWithError(error)
            }
        }

        function onInstanceReady() {
            if (manualStartInProgress) {
                statusText = qsTr("CloudDeck instance is running.")
                manualStartInProgress = false
                busy = false
            }
        }
    }

    Connections {
        target: ComputerManager

        function onComputerAddCompleted(success) {
            if (!awaitingAddComplete) {
                return
            }

            if (!success) {
                failWithError(qsTr("Unable to add the CloudDeck host to Moonlight."))
                return
            }

            if (!tryStartPairing()) {
                retryCount = 0
                pairingRetryTimer.start()
            }
        }
    }

    Connections {
        target: computerModel

        function onPairingCompleted(error) {
            if (!pairingFlowInProgress) {
                return
            }

            pairingFlowInProgress = false
            busy = false

            if (error !== undefined) {
                failWithError(error)
            } else {
                statusText = qsTr("CloudDeck paired successfully.")
            }
        }
    }

    contentItem: ColumnLayout {
        id: cloudDeckContent
        spacing: 12
        implicitWidth: 460

        Label {
            text: qsTr("Connect your CloudDeck account to Moonlight.")
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }

        TextField {
            id: emailField
            placeholderText: qsTr("Email")
            Layout.fillWidth: true
            enabled: !busy
        }

        TextField {
            id: passwordField
            placeholderText: qsTr("Password")
            echoMode: TextInput.Password
            Layout.fillWidth: true
            enabled: !busy
        }

        Button {
            text: qsTr("Clear saved credentials")
            visible: hasStoredCredentials
            enabled: !busy
            onClicked: {
                clearCredentialsDialog.open()
            }
        }

        Label {
            text: errorText
            visible: errorText.length > 0
            color: "#ff6b6b"
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }

        RowLayout {
            spacing: 10
            Layout.fillWidth: true

            BusyIndicator {
                visible: busy
                running: visible
            }

            Label {
                text: statusText
                visible: statusText.length > 0
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
        }
    }

    footer: DialogButtonBox {
        id: buttonBox

        Button {
            text: qsTr("Add / Connect")
            enabled: !busy && emailField.text.trim().length > 0 && passwordField.text.length > 0
            onClicked: startPairing()
        }

        Button {
            text: qsTr("Close")
            enabled: true
            onClicked: cloudDeckDialog.close()
        }
    }

    NavigableMessageDialog {
        id: clearCredentialsDialog
        text: qsTr("Clear saved CloudDeck credentials? This will remove the stored email and password.")
        standardButtons: Dialog.Yes | Dialog.No

        onAccepted: {
            CloudDeckManager.clearStoredCredentials()
            hasStoredCredentials = false
            emailField.text = ""
            passwordField.text = ""
        }
    }
}
