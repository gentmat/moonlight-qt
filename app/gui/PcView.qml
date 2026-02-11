import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import ComputerModel 1.0
import CloudDeckManagerApi 1.0

import ComputerManager 1.0
import StreamingPreferences 1.0
import SystemProperties 1.0
import SdlGamepadKeyNavigation 1.0

CenteredGridView {
    property ComputerModel computerModel : createModel()

    id: pcGrid
    focus: true
    activeFocusOnTab: true
    topMargin: 20
    bottomMargin: 5
    cellWidth: 310; cellHeight: 330;
    objectName: qsTr("Computers")

    Component.onCompleted: {
        // Don't show any highlighted item until interacting with them.
        // We do this here instead of onActivated to avoid losing the user's
        // selection when backing out of a different page of the app.
        currentIndex = -1
    }

    // Note: Any initialization done here that is critical for streaming must
    // also be done in CliStartStreamSegue.qml, since this code does not run
    // for command-line initiated streams.
    StackView.onActivated: {
        // Setup signals on CM
        ComputerManager.computerAddCompleted.connect(addComplete)

        // Highlight the first item if a gamepad is connected
        if (currentIndex === -1 && SdlGamepadKeyNavigation.getConnectedGamepads() > 0) {
            currentIndex = 0
        }
    }

    StackView.onDeactivating: {
        ComputerManager.computerAddCompleted.disconnect(addComplete)
    }

    function pairingComplete(error)
    {
        // Close the PIN dialog
        pairDialog.close()

        // Display a failed dialog if we got an error
        if (error !== undefined) {
            errorDialog.text = error
            errorDialog.helpText = ""
            errorDialog.open()
        }
    }

    function addComplete(success, detectedPortBlocking)
    {
        if (!success) {
            errorDialog.text = qsTr("Unable to connect to the specified PC.")

            if (detectedPortBlocking) {
                errorDialog.text += "\n\n" + qsTr("This PC's Internet connection is blocking Moonlight. Streaming over the Internet may not work while connected to this network.")
            }
            else {
                errorDialog.helpText = qsTr("Click the Help button for possible solutions.")
            }

            errorDialog.open()
        }
    }

    function createModel()
    {
        var model = Qt.createQmlObject('import ComputerModel 1.0; ComputerModel {}', parent, '')
        model.initialize(ComputerManager)
        model.pairingCompleted.connect(pairingComplete)
        model.connectionTestCompleted.connect(testConnectionDialog.connectionTestComplete)
        return model
    }

    function openCloudDeckDialog()
    {
        cloudDeckDialog.open()
    }

    Row {
        anchors.centerIn: parent
        spacing: 5
        visible: pcGrid.count === 0

        BusyIndicator {
            id: searchSpinner
            visible: StreamingPreferences.enableMdns
            running: visible
        }

        Label {
            height: searchSpinner.height
            elide: Label.ElideRight
            text: StreamingPreferences.enableMdns ? qsTr("Searching for compatible hosts on your local network...")
                                                  : qsTr("Automatic PC discovery is disabled. Add your PC manually.")
            font.pointSize: 20
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.Wrap
        }
    }

    model: computerModel

    delegate: NavigableItemDelegate {
        width: 300; height: 320;
        grid: pcGrid

        property alias pcContextMenu : pcContextMenuLoader.item
        property string cloudDeckMatchAddress: model.manualAddress && model.manualAddress.length > 0 ? model.manualAddress : model.activeAddress
        property bool cloudDeckHostKnown: false
        property bool cloudDeckHasCredentials: false

        function refreshCloudDeckMenuState() {
            cloudDeckHostKnown = CloudDeckManagerApi.isCloudDeckHost(cloudDeckMatchAddress)
            cloudDeckHasCredentials = CloudDeckManagerApi.hasStoredCredentials()
        }

        Image {
            id: pcIcon
            anchors.horizontalCenter: parent.horizontalCenter
            source: "qrc:/res/desktop_windows-48px.svg"
            sourceSize {
                width: 200
                height: 200
            }
        }

        Image {
            // TODO: Tooltip
            id: stateIcon
            anchors.horizontalCenter: pcIcon.horizontalCenter
            anchors.verticalCenter: pcIcon.verticalCenter
            anchors.verticalCenterOffset: !model.online ? -18 : -16
            visible: !model.statusUnknown && (!model.online || !model.paired)
            source: !model.online ? "qrc:/res/warning_FILL1_wght300_GRAD200_opsz24.svg" : "qrc:/res/baseline-lock-24px.svg"
            sourceSize {
                width: !model.online ? 75 : 70
                height: !model.online ? 75 : 70
            }
        }

        BusyIndicator {
            id: statusUnknownSpinner
            anchors.horizontalCenter: pcIcon.horizontalCenter
            anchors.verticalCenter: pcIcon.verticalCenter
            anchors.verticalCenterOffset: -15
            width: 75
            height: 75
            visible: model.statusUnknown
            running: visible
        }

        Label {
            id: pcNameText
            text: model.name

            width: parent.width
            anchors.top: pcIcon.bottom
            anchors.bottom: parent.bottom
            font.pointSize: 36
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            elide: Text.ElideRight
        }

        Loader {
            id: pcContextMenuLoader
            asynchronous: true
            sourceComponent: NavigableMenu {
                id: pcContextMenu
                Component.onCompleted: {
                    refreshCloudDeckMenuState()
                }

                Connections {
                    target: pcContextMenu
                    function onOpened() {
                        refreshCloudDeckMenuState()
                    }
                }
                MenuItem {
                    text: qsTr("PC Status: %1").arg(model.online ? qsTr("Online") : qsTr("Offline"))
                    font.bold: true
                    enabled: false
                }
                NavigableMenuItem {
                    text: qsTr("View All Apps")
                    onTriggered: {
                        var component = Qt.createComponent("AppView.qml")
                        var appView = component.createObject(stackView, {"computerIndex": index, "objectName": model.name, "showHiddenGames": true})
                        stackView.push(appView)
                    }
                    visible: model.online && model.paired
                }
                NavigableMenuItem {
                    text: qsTr("Wake PC")
                    onTriggered: computerModel.wakeComputer(index)
                    visible: !model.online && model.wakeable
                }
                NavigableMenuItem {
                    text: qsTr("Turn CloudDeck ON")
                    onTriggered: {
                        cloudDeckDialog.autoStartInstance = cloudDeckHasCredentials
                        cloudDeckDialog.open()
                    }
                    visible: !model.online && model.paired && cloudDeckHostKnown
                }
                NavigableMenuItem {
                    text: qsTr("Test Network")
                    onTriggered: {
                        computerModel.testConnectionForComputer(index)
                        testConnectionDialog.open()
                    }
                }

                NavigableMenuItem {
                    text: qsTr("Rename PC")
                    onTriggered: {
                        renamePcDialog.pcIndex = index
                        renamePcDialog.originalName = model.name
                        renamePcDialog.open()
                    }
                }
                NavigableMenuItem {
                    text: qsTr("Delete PC")
                    onTriggered: {
                        deletePcDialog.pcIndex = index
                        deletePcDialog.pcName = model.name
                        deletePcDialog.open()
                    }
                }
                NavigableMenuItem {
                    text: qsTr("View Details")
                    onTriggered: {
                        showPcDetailsDialog.pcDetails = model.details
                        showPcDetailsDialog.manualAddress = model.manualAddress
                        showPcDetailsDialog.activeAddress = model.activeAddress
                        showPcDetailsDialog.open()
                    }
                }
                NavigableMenuItem {
                    text: qsTr("CloudDeck Settings")
                    visible: cloudDeckHostKnown
                    onTriggered: {
                        cloudDeckSettingsDialog.manualAddress = model.manualAddress
                        cloudDeckSettingsDialog.activeAddress = model.activeAddress
                        cloudDeckSettingsDialog.open()
                    }
                }
                NavigableMenuItem {
                    text: qsTr("Session Timer Settings")
                    visible: cloudDeckHostKnown
                    onTriggered: {
                        sessionTimerSettingsDialog.manualAddress = model.manualAddress
                        sessionTimerSettingsDialog.activeAddress = model.activeAddress
                        sessionTimerSettingsDialog.open()
                    }
                }
            }
        }

        onClicked: {
            if (model.online) {
                if (!model.serverSupported) {
                    errorDialog.text = qsTr("The version of GeForce Experience on %1 is not supported by this build of Moonlight. You must update Moonlight to stream from %1.").arg(model.name)
                    errorDialog.helpText = ""
                    errorDialog.open()
                }
                else if (model.paired) {
                    // go to game view
                    var component = Qt.createComponent("AppView.qml")
                    var appView = component.createObject(stackView, {"computerIndex": index, "objectName": model.name})
                    stackView.push(appView)
                }
                else {
                    var pin = computerModel.generatePinString()

                    // Kick off pairing in the background
                    computerModel.pairComputer(index, pin)

                    // Display the pairing dialog
                    pairDialog.pin = pin
                    pairDialog.open()
                }
            } else if (!model.online) {
                // Using open() here because it may be activated by keyboard
                pcContextMenu.open()
            }
        }

        onPressAndHold: {
            // popup() ensures the menu appears under the mouse cursor
            if (pcContextMenu.popup) {
                pcContextMenu.popup()
            }
            else {
                // Qt 5.9 doesn't have popup()
                pcContextMenu.open()
            }
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton;
            onClicked: {
                parent.pressAndHold()
            }
        }

        Keys.onMenuPressed: {
            // We must use open() here so the menu is positioned on
            // the ItemDelegate and not where the mouse cursor is
            pcContextMenu.open()
        }

        Keys.onDeletePressed: {
            deletePcDialog.pcIndex = index
            deletePcDialog.pcName = model.name
            deletePcDialog.open()
        }
    }

    ErrorMessageDialog {
        id: errorDialog

        // Using Setup-Guide here instead of Troubleshooting because it's likely that users
        // will arrive here by forgetting to enable GameStream or not forwarding ports.
        helpUrl: "https://github.com/moonlight-stream/moonlight-docs/wiki/Setup-Guide"
    }

    NavigableMessageDialog {
        id: pairDialog

        // Pairing dialog must be modal to prevent double-clicks from triggering
        // pairing twice
        modal: true
        closePolicy: Popup.CloseOnEscape

        // don't allow edits to the rest of the window while open
        property string pin : "0000"
        text:qsTr("Please enter %1 on your host PC. This dialog will close when pairing is completed.").arg(pin)+"\n\n"+
             qsTr("If your host PC is running Sunshine, navigate to the Sunshine web UI to enter the PIN.")
        standardButtons: Dialog.Cancel
        onRejected: {
            // FIXME: We should interrupt pairing here
        }
    }

    NavigableMessageDialog {
        id: deletePcDialog
        // don't allow edits to the rest of the window while open
        property int pcIndex : -1
        property string pcName : ""
        text: qsTr("Are you sure you want to remove '%1'?").arg(pcName)
        standardButtons: Dialog.Yes | Dialog.No

        onAccepted: {
            computerModel.deleteComputer(pcIndex)
        }
    }

    NavigableMessageDialog {
        id: testConnectionDialog
        closePolicy: Popup.CloseOnEscape
        standardButtons: Dialog.Ok

        onAboutToShow: {
            testConnectionDialog.text = qsTr("Moonlight is testing your network connection to determine if any required ports are blocked.") + "\n\n" + qsTr("This may take a few seconds…")
            showSpinner = true
        }

        function connectionTestComplete(result, blockedPorts)
        {
            if (result === -1) {
                text = qsTr("The network test could not be performed because none of Moonlight's connection testing servers were reachable from this PC. Check your Internet connection or try again later.")
                imageSrc = "qrc:/res/baseline-warning-24px.svg"
            }
            else if (result === 0) {
                text = qsTr("This network does not appear to be blocking Moonlight. If you still have trouble connecting, check your PC's firewall settings.") + "\n\n" + qsTr("If you are trying to stream over the Internet, install the Moonlight Internet Hosting Tool on your gaming PC and run the included Internet Streaming Tester to check your gaming PC's Internet connection.")
                imageSrc = "qrc:/res/baseline-check_circle_outline-24px.svg"
            }
            else {
                text = qsTr("Your PC's current network connection seems to be blocking Moonlight. Streaming over the Internet may not work while connected to this network.") + "\n\n" + qsTr("The following network ports were blocked:") + "\n"
                text += blockedPorts
                imageSrc = "qrc:/res/baseline-error_outline-24px.svg"
            }

            // Stop showing the spinner and show the image instead
            showSpinner = false
        }
    }

    CloudDeckDialog {
        id: cloudDeckDialog
        computerModel: pcGrid.computerModel
    }

    NavigableDialog {
        id: renamePcDialog
        property string label: qsTr("Enter the new name for this PC:")
        property string originalName
        property int pcIndex : -1;

        standardButtons: Dialog.Ok | Dialog.Cancel

        onOpened: {
            // Force keyboard focus on the textbox so keyboard navigation works
            editText.forceActiveFocus()
        }

        onClosed: {
            editText.clear()
        }

        onAccepted: {
            if (editText.text) {
                computerModel.renameComputer(pcIndex, editText.text)
            }
        }

        ColumnLayout {
            Label {
                text: renamePcDialog.label
                font.bold: true
            }

            TextField {
                id: editText
                placeholderText: renamePcDialog.originalName
                Layout.fillWidth: true
                focus: true

                Keys.onReturnPressed: {
                    renamePcDialog.accept()
                }

                Keys.onEnterPressed: {
                    renamePcDialog.accept()
                }
            }
        }
    }

    NavigableDialog {
        id: showPcDetailsDialog
        property string pcDetails: ""
        property string manualAddress: ""
        property string activeAddress: ""
        title: qsTr("View Details")

        contentItem: ColumnLayout {
            spacing: 8
            implicitWidth: 520

            Label {
                text: showPcDetailsDialog.pcDetails
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                visible: text.length > 0
            }

            Label {
                text: qsTr("Manual address: %1").arg(showPcDetailsDialog.manualAddress.length > 0 ? showPcDetailsDialog.manualAddress : qsTr("N/A"))
                Layout.fillWidth: true
                wrapMode: Text.Wrap
            }

            Label {
                text: qsTr("Active address: %1").arg(showPcDetailsDialog.activeAddress.length > 0 ? showPcDetailsDialog.activeAddress : qsTr("N/A"))
                Layout.fillWidth: true
                wrapMode: Text.Wrap
            }
        }

        footer: DialogButtonBox {
            standardButtons: Dialog.Ok
        }
    }

    NavigableDialog {
        id: cloudDeckSettingsDialog
        title: qsTr("CloudDeck Settings")
        property string manualAddress: ""
        property string activeAddress: ""
        property string cloudDeckUser: ""
        property string cloudDeckPassword: ""
        property bool cloudDeckHost: false

        onOpened: {
            var hostAddress = manualAddress.length > 0 ? manualAddress : activeAddress
            cloudDeckHost = CloudDeckManagerApi.isCloudDeckHost(hostAddress)
            if (cloudDeckHost) {
                cloudDeckUser = "user"
                cloudDeckPassword = CloudDeckManagerApi.getStoredHostPassword()
            } else {
                cloudDeckUser = ""
                cloudDeckPassword = ""
            }
        }

        contentItem: ColumnLayout {
            spacing: 8
            implicitWidth: 520

            Label {
                visible: !cloudDeckSettingsDialog.cloudDeckHost
                text: qsTr("This host is not recognized as a CloudDeck instance.")
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }

            ColumnLayout {
                visible: cloudDeckSettingsDialog.cloudDeckHost
                spacing: 6
                Layout.fillWidth: true

                Label {
                    text: qsTr("CloudDeck")
                    font.bold: true
                }

                Label {
                    text: qsTr("User: %1").arg(cloudDeckSettingsDialog.cloudDeckUser.length > 0 ? cloudDeckSettingsDialog.cloudDeckUser : qsTr("Unknown"))
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }

                RowLayout {
                    spacing: 6
                    Layout.fillWidth: true

                    Label {
                        text: qsTr("Password:")
                    }

                    TextField {
                        id: cloudDeckSettingsPasswordField
                        text: cloudDeckSettingsDialog.cloudDeckPassword.length > 0 ? cloudDeckSettingsDialog.cloudDeckPassword : qsTr("Unknown")
                        readOnly: true
                        Layout.fillWidth: true
                    }

                    ToolButton {
                        text: qsTr("Copy")
                        enabled: cloudDeckSettingsDialog.cloudDeckPassword.length > 0
                        onClicked: {
                            cloudDeckSettingsPasswordField.forceActiveFocus()
                            cloudDeckSettingsPasswordField.selectAll()
                            cloudDeckSettingsPasswordField.copy()
                            cloudDeckSettingsPasswordField.deselect()
                        }
                    }
                }

                Label {
                    text: qsTr("Sunshine")
                    font.bold: true
                    Layout.topMargin: 4
                }

                RowLayout {
                    spacing: 6
                    Layout.fillWidth: true

                    Label { text: qsTr("Username:") }
                    Label { text: qsTr("sunshine"); font.bold: true }
                    Label { text: qsTr("Password:") }
                    Label { text: qsTr("sunshine"); font.bold: true }
                }
            }
        }

        footer: DialogButtonBox {
            standardButtons: Dialog.Ok
        }
    }

    NavigableDialog {
        id: sessionTimerSettingsDialog
        title: qsTr("Session Timer Settings")
        property string manualAddress: ""
        property string activeAddress: ""
        property bool cloudDeckHost: false
        property int sessionTimerHours: 8
        property int sessionTimerDisplayMode: 1
        property int sessionTimerWarnMinutes: 5
        property bool sessionTimerHourlyReminderEnabled: true
        property int sessionTimerHourlyReminderSeconds: 5
        property int compactFieldWidth: 58

        onOpened: {
            var hostAddress = manualAddress.length > 0 ? manualAddress : activeAddress
            cloudDeckHost = CloudDeckManagerApi.isCloudDeckHost(hostAddress)
            if (cloudDeckHost) {
                sessionTimerHours = CloudDeckManagerApi.getSessionTimerHours()
                sessionTimerDisplayMode = CloudDeckManagerApi.getSessionTimerDisplayMode()
                sessionTimerWarnMinutes = CloudDeckManagerApi.getSessionTimerWarnMinutes()
                sessionTimerHourlyReminderEnabled = CloudDeckManagerApi.getSessionTimerHourlyReminderEnabled()
                sessionTimerHourlyReminderSeconds = CloudDeckManagerApi.getSessionTimerHourlyReminderSeconds()
            } else {
                sessionTimerHours = 8
                sessionTimerDisplayMode = 1
                sessionTimerWarnMinutes = 5
                sessionTimerHourlyReminderEnabled = true
                sessionTimerHourlyReminderSeconds = 5
            }

            sessionTimerHoursField.text = String(sessionTimerHours)
            sessionTimerBeforeEndMinutesField.text = String(sessionTimerWarnMinutes)
            sessionTimerHourlyReminderSecondsField.text = String(sessionTimerHourlyReminderSeconds)
        }

        contentItem: ColumnLayout {
            spacing: 8
            implicitWidth: 540

            Label {
                visible: !sessionTimerSettingsDialog.cloudDeckHost
                text: qsTr("This host is not recognized as a CloudDeck instance.")
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }

            ColumnLayout {
                visible: sessionTimerSettingsDialog.cloudDeckHost
                spacing: 6
                Layout.fillWidth: true

                RowLayout {
                    spacing: 6
                    Layout.fillWidth: true

                    Label {
                        text: qsTr("Session duration:")
                    }

                    TextField {
                        id: sessionTimerHoursField
                        text: "8"
                        validator: IntValidator {
                            bottom: 1
                            top: 24
                        }
                        inputMethodHints: Qt.ImhDigitsOnly
                        Layout.preferredWidth: sessionTimerSettingsDialog.compactFieldWidth

                        onEditingFinished: {
                            var parsedHours = parseInt(text)
                            if (isNaN(parsedHours)) {
                                parsedHours = sessionTimerSettingsDialog.sessionTimerHours
                            }

                            parsedHours = Math.max(1, Math.min(24, parsedHours))
                            sessionTimerSettingsDialog.sessionTimerHours = parsedHours
                            text = String(parsedHours)
                            CloudDeckManagerApi.setSessionTimerHours(parsedHours)
                        }
                    }

                    Label { text: qsTr("hours") }
                }

                Label {
                    text: qsTr("Timer visibility")
                    font.bold: true
                }

                ButtonGroup {
                    id: sessionTimerSettingsModeGroup
                }

                RadioButton {
                    text: qsTr("Always show timer")
                    ButtonGroup.group: sessionTimerSettingsModeGroup
                    checked: sessionTimerSettingsDialog.sessionTimerDisplayMode === 0
                    onToggled: {
                        if (checked) {
                            sessionTimerSettingsDialog.sessionTimerDisplayMode = 0
                            CloudDeckManagerApi.setSessionTimerDisplayMode(0)
                        }
                    }
                }

                RowLayout {
                    spacing: 6
                    Layout.fillWidth: true

                    RadioButton {
                        text: qsTr("Only before end")
                        ButtonGroup.group: sessionTimerSettingsModeGroup
                        checked: sessionTimerSettingsDialog.sessionTimerDisplayMode === 1
                        onToggled: {
                            if (checked) {
                                sessionTimerSettingsDialog.sessionTimerDisplayMode = 1
                                CloudDeckManagerApi.setSessionTimerDisplayMode(1)
                            }
                        }
                    }

                    TextField {
                        id: sessionTimerBeforeEndMinutesField
                        text: "5"
                        validator: IntValidator {
                            bottom: 1
                            top: 120
                        }
                        inputMethodHints: Qt.ImhDigitsOnly
                        Layout.preferredWidth: sessionTimerSettingsDialog.compactFieldWidth
                        enabled: sessionTimerSettingsDialog.sessionTimerDisplayMode === 1

                        onEditingFinished: {
                            var parsedMinutes = parseInt(text)
                            if (isNaN(parsedMinutes)) {
                                parsedMinutes = sessionTimerSettingsDialog.sessionTimerWarnMinutes
                            }

                            parsedMinutes = Math.max(1, Math.min(120, parsedMinutes))
                            sessionTimerSettingsDialog.sessionTimerWarnMinutes = parsedMinutes
                            text = String(parsedMinutes)
                            CloudDeckManagerApi.setSessionTimerWarnMinutes(parsedMinutes)
                        }
                    }

                    Label {
                        text: qsTr("min")
                        enabled: sessionTimerSettingsDialog.sessionTimerDisplayMode === 1
                    }
                }

                RadioButton {
                    text: qsTr("Hide timer always")
                    ButtonGroup.group: sessionTimerSettingsModeGroup
                    checked: sessionTimerSettingsDialog.sessionTimerDisplayMode === 2
                    onToggled: {
                        if (checked) {
                            sessionTimerSettingsDialog.sessionTimerDisplayMode = 2
                            CloudDeckManagerApi.setSessionTimerDisplayMode(2)
                        }
                    }
                }

                RowLayout {
                    spacing: 6
                    Layout.fillWidth: true
                    Layout.topMargin: 4

                    CheckBox {
                        id: sessionTimerHourlyReminderCheck
                        text: qsTr("Hourly reminder")
                        checked: sessionTimerSettingsDialog.sessionTimerHourlyReminderEnabled
                        onToggled: {
                            sessionTimerSettingsDialog.sessionTimerHourlyReminderEnabled = checked
                            CloudDeckManagerApi.setSessionTimerHourlyReminderEnabled(checked)
                        }
                    }

                    Label {
                        text: qsTr("for")
                        enabled: sessionTimerHourlyReminderCheck.checked
                    }

                    TextField {
                        id: sessionTimerHourlyReminderSecondsField
                        text: "5"
                        validator: IntValidator {
                            bottom: 1
                            top: 60
                        }
                        inputMethodHints: Qt.ImhDigitsOnly
                        Layout.preferredWidth: sessionTimerSettingsDialog.compactFieldWidth
                        enabled: sessionTimerHourlyReminderCheck.checked

                        onEditingFinished: {
                            var parsedSeconds = parseInt(text)
                            if (isNaN(parsedSeconds)) {
                                parsedSeconds = sessionTimerSettingsDialog.sessionTimerHourlyReminderSeconds
                            }

                            parsedSeconds = Math.max(1, Math.min(60, parsedSeconds))
                            sessionTimerSettingsDialog.sessionTimerHourlyReminderSeconds = parsedSeconds
                            text = String(parsedSeconds)
                            CloudDeckManagerApi.setSessionTimerHourlyReminderSeconds(parsedSeconds)
                        }
                    }

                    Label {
                        text: qsTr("seconds")
                        enabled: sessionTimerHourlyReminderCheck.checked
                    }
                }
            }
        }

        footer: DialogButtonBox {
            standardButtons: Dialog.Ok
        }
    }

    ScrollBar.vertical: ScrollBar {}
}
