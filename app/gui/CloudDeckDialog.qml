import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import CloudDeckManagerApi 1.0
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
    property int pairingRetryCount: 0
    property int maxPairingRetries: 5
    property int pairingSuccessCloseDelayMs: 1500
    property int pendingPairIndex: -1
    property string pendingPin: ""
    property string accessToken: ""
    property string machineId: ""
    property bool startIssued: false

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
        pairingRetryCount = 0
        pendingPairIndex = -1
        pendingPin = ""
        accessToken = ""
        machineId = ""
        startIssued = false
        pairingRetryTimer.stop()
        pinSubmitTimer.stop()
        successCloseTimer.stop()
        manualStartCloseTimer.stop()
    }

    function failWithError(message) {
        errorText = message
        statusText = ""
        busy = false
        manualStartInProgress = false
        pairingFlowInProgress = false
        awaitingAddComplete = false
        retryCount = 0
        pairingRetryCount = 0
        pendingPairIndex = -1
        pendingPin = ""
        pairingRetryTimer.stop()
        pinSubmitTimer.stop()
        successCloseTimer.stop()
        manualStartCloseTimer.stop()
    }

    function isFlowActive() {
        return busy && (pairingFlowInProgress || manualStartInProgress)
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
        pairingFlowInProgress = true
        manualStartInProgress = false
        CloudDeckManagerApi.loginWithCredentials(emailValue, passwordField.text)
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
            CloudDeckManagerApi.loginWithCredentials(emailValue, passwordField.text)
        } else {
            CloudDeckManagerApi.loginWithCredentials(CloudDeckManagerApi.getStoredEmail(),
                                                     CloudDeckManagerApi.getStoredPassword())
        }
    }

    function resolvePendingPairIndex() {
        if (!computerModel || pendingAddress.length === 0) {
            return -1
        }
        pendingPairIndex = computerModel.findComputerByManualAddress(pendingAddress)
        return pendingPairIndex
    }

    function startAutoPairing(index) {
        if (index < 0) {
            failWithError(qsTr("Unable to find the CloudDeck host in Moonlight. Please try again."))
            return
        }

        pendingPairIndex = index
        pendingPin = computerModel.generatePinString()
        statusText = qsTr("Pairing with CloudDeck...")
        computerModel.pairComputer(index, pendingPin)
        pinSubmitTimer.restart()
    }

    function tryStartPairing() {
        if (!computerModel || pendingAddress.length === 0) {
            return false
        }

        var index = computerModel.findComputerByManualAddress(pendingAddress)
        if (index < 0) {
            return false
        }

        startAutoPairing(index)
        return true
    }

    function isPinMismatchError(message) {
        if (!message || message.length === 0) {
            return false
        }
        var lower = message.toLowerCase()
        if (lower.indexOf("pin") === -1) {
            return false
        }
        return lower.indexOf("didn't match") >= 0 ||
               lower.indexOf("did not match") >= 0 ||
               lower.indexOf("mismatch") >= 0 ||
               lower.indexOf("invalid") >= 0
    }

    function normalizeHostAddress(address) {
        if (address === undefined || address === null) {
            return ""
        }
        return String(address).trim()
    }

    function persistCloudDeckHostUuidFromIndex(index) {
        if (!computerModel || index < 0) {
            return false
        }

        var resolvedUuid = computerModel.getComputerUuid(index)
        if (resolvedUuid === undefined || resolvedUuid === null || resolvedUuid.length === 0) {
            return false
        }

        CloudDeckManagerApi.setCloudDeckHostUuid(resolvedUuid)
        return true
    }

    function persistCloudDeckHostUuidByAddress(address) {
        if (!computerModel) {
            return false
        }

        var normalized = normalizeHostAddress(address)
        if (normalized.length === 0) {
            return false
        }

        var index = computerModel.findComputerByManualAddress(normalized)
        if (index < 0) {
            return false
        }

        pendingPairIndex = index
        return persistCloudDeckHostUuidFromIndex(index)
    }

    function isValidHostAddress(address) {
        var normalized = normalizeHostAddress(address)
        if (normalized.length === 0) {
            return false
        }
        if (normalized.indexOf(" ") >= 0 || normalized.indexOf("/") >= 0) {
            return false
        }
        return true
    }

    function schedulePairingRetry(message) {
        if (pairingRetryCount >= maxPairingRetries) {
            failWithError(message)
            return
        }

        pairingRetryCount += 1
        pendingPin = ""
        statusText = qsTr("PIN mismatch. Retrying pairing (%1/%2)...")
                     .arg(pairingRetryCount)
                     .arg(maxPairingRetries)

        var index = resolvePendingPairIndex()
        if (index < 0) {
            failWithError(message)
            return
        }

        pendingPairIndex = index
        pendingPin = computerModel.generatePinString()
        computerModel.pairComputer(index, pendingPin)
        pinSubmitTimer.restart()
    }

    onOpened: {
        resetState()
        hasStoredCredentials = CloudDeckManagerApi.hasStoredCredentials()
        emailField.text = CloudDeckManagerApi.getStoredEmail()
        passwordField.text = CloudDeckManagerApi.getStoredPassword()
        emailField.forceActiveFocus()

        if (autoStartInstance) {
            autoStartInstance = false
            startInstance()
        }
    }

    onClosed: {
        if (busy) {
            CloudDeckManagerApi.cancelCurrentOperation()
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

    Timer {
        id: pinSubmitTimer
        interval: 3000
        repeat: false

        onTriggered: {
            if (!pairingFlowInProgress || pendingPin.length === 0) {
                return
            }
            if (accessToken.length === 0 || machineId.length === 0) {
                failWithError(qsTr("Unable to submit PIN. Please try again."))
                return
            }
            CloudDeckManagerApi.addMachineClient(accessToken, machineId, pendingPin)
        }
    }

    Timer {
        id: successCloseTimer
        interval: 1500
        repeat: false

        onTriggered: {
            cloudDeckDialog.close()
        }
    }

    Timer {
        id: manualStartCloseTimer
        interval: 1000
        repeat: false

        onTriggered: {
            cloudDeckDialog.close()
        }
    }

    Connections {
        target: CloudDeckManagerApi

        function onLoginCompleted(status, accessTokenValue, expiresIn, idToken, refreshToken, tokenType, errorCode, errorMessage, challengeName, challengeParameters) {
            if (!busy) {
                return
            }

            if (status !== CloudDeckManagerApi.AuthSuccess) {
                failWithError(errorMessage.length > 0 ? errorMessage : errorCode)
                return
            }

            hasStoredCredentials = true
            accessToken = accessTokenValue
            statusText = pairingFlowInProgress ? qsTr("Login successful! Fetching machine info...")
                                               : qsTr("Fetching CloudDeck status...")
            CloudDeckManagerApi.fetchMachineId(accessToken)
        }

        function onMachineIdFetched(success, machineIdValue, errorCode, errorMessage) {
            if (!isFlowActive()) {
                return
            }

            if (!success) {
                failWithError(errorMessage.length > 0 ? errorMessage : errorCode)
                return
            }

            if (machineIdValue.length === 0) {
                failWithError(qsTr("CloudDeck machine ID is missing. Please try again."))
                return
            }

            machineId = machineIdValue
            statusText = qsTr("Fetching CloudDeck status...")
            CloudDeckManagerApi.fetchMachineStatus(machineId, accessToken)
        }

        function onMachineStatusUpdated(status, publicIp, password, lastStarted, createdAt) {
            if (!isFlowActive()) {
                return
            }

            var lowerStatus = status ? String(status).toLowerCase() : ""

            if (lowerStatus === "running") {
                if (pairingFlowInProgress) {
                    if (awaitingAddComplete) {
                        return
                    }
                    if (!isValidHostAddress(publicIp)) {
                        failWithError(qsTr("CloudDeck returned an invalid host address. Please try again."))
                        return
                    }
                    pendingAddress = normalizeHostAddress(publicIp)
                    statusText = qsTr("Found CloudDeck host %1. Adding to Moonlight...").arg(pendingAddress)
                    awaitingAddComplete = true
                    ComputerManager.addNewHostManually(pendingAddress)
                } else if (manualStartInProgress) {
                    if (isValidHostAddress(publicIp)) {
                        // Refresh saved UUID when instance identity changed but address stayed stable.
                        persistCloudDeckHostUuidByAddress(publicIp)
                    }
                    statusText = qsTr("CloudDeck instance is running.")
                    manualStartInProgress = false
                    busy = false
                    manualStartCloseTimer.restart()
                }
                return
            }

            if (lowerStatus === "off" || lowerStatus === "stopping" || lowerStatus === "starting") {
                if (lowerStatus === "off" || lowerStatus === "stopping") {
                    statusText = qsTr("Instance is off. Starting CloudDeck...")
                } else {
                    statusText = qsTr("Instance is starting...")
                }
                if (!startIssued) {
                    if (machineId.length === 0 || accessToken.length === 0) {
                        failWithError(qsTr("Missing CloudDeck session data. Please try again."))
                        return
                    }

                    startIssued = true
                    CloudDeckManagerApi.startMachine(machineId, accessToken)
                }
                return
            }

            statusText = qsTr("Instance status: %1").arg(status)
        }

        function onMachineStatusFailed(errorCode, errorMessage) {
            if (!isFlowActive()) {
                return
            }

            failWithError(errorMessage.length > 0 ? errorMessage : errorCode)
        }

        function onMachineStartFinished(success, status, errorCode, errorMessage) {
            if (!isFlowActive()) {
                return
            }

            if (!success) {
                failWithError(errorMessage.length > 0 ? errorMessage : errorCode)
            }
        }

        function onMachineClientAdded(success, response, errorCode, errorMessage) {
            if (!pairingFlowInProgress) {
                return
            }
            if (!success) {
                failWithError(errorMessage.length > 0 ? errorMessage : errorCode)
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

            // Persist UUID as soon as the host resolves in Moonlight, even before pairing completes.
            persistCloudDeckHostUuidByAddress(pendingAddress)
            retryCount = 0
            statusText = qsTr("Waiting for CloudDeck host to appear...")
            pairingRetryTimer.start()
        }
    }

    Connections {
        target: computerModel

        function onPairingCompleted(error) {
            if (!pairingFlowInProgress) {
                return
            }

            if (error !== undefined) {
                if (isPinMismatchError(error)) {
                    schedulePairingRetry(error)
                    return
                }
                failWithError(error)
                return
            }

            pairingFlowInProgress = false
            busy = false
            pairingRetryCount = 0

            // Persist the host UUID for reliable CloudDeck host identification
            var resolvedIndex = resolvePendingPairIndex()
            if (resolvedIndex >= 0) {
                persistCloudDeckHostUuidFromIndex(resolvedIndex)
            }

            statusText = qsTr("CloudDeck paired successfully.")
            successCloseTimer.interval = pairingSuccessCloseDelayMs
            successCloseTimer.restart()
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
            text: qsTr("Close")
            enabled: true
            onClicked: cloudDeckDialog.close()
        }

        Button {
            text: qsTr("Add / Connect")
            enabled: !busy && emailField.text.trim().length > 0 && passwordField.text.length > 0
            onClicked: startPairing()
        }
    }

    NavigableMessageDialog {
        id: clearCredentialsDialog
        text: qsTr("Clear saved CloudDeck credentials? This will remove the stored email and password.")
        standardButtons: Dialog.Yes | Dialog.No

        onAccepted: {
            CloudDeckManagerApi.clearStoredCredentials()
            hasStoredCredentials = false
            emailField.text = ""
            passwordField.text = ""
        }
    }
}
