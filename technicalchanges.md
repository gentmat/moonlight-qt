# Technical changes

## Modified
- app/app.pro
  - Adds Qt WebEngine detection and `HAVE_WEBENGINE` define so CloudDeck can use WebEngine when available.
  - Adds CloudDeck sources/headers and include path so the manager is built and visible to the app.
- app/main.cpp
  - Registers `CloudDeckManager` as a QML singleton so QML can call CloudDeck APIs.
- app/gui/PcView.qml
  - Adds a `CloudDeckDialog` instance and an `openCloudDeckDialog()` helper to open it from the toolbar button.
- app/gui/main.qml
  - Adds a toolbar button (cloud icon) that opens the CloudDeck dialog when viewing PCs.
- app/gui/computermodel.cpp
  - Adds `findComputerByManualAddress()` helper to resolve a host by manual address for CloudDeck pairing flow.
- app/gui/computermodel.h
  - Declares the new `findComputerByManualAddress()` QML‑invokable method.
- app/qml.qrc
  - Registers `CloudDeckDialog.qml` in the QML resource list.
- app/resources.qrc
  - Registers the `cloud.svg` icon in the app resources.
- changes.md
  - Updated to include the clean rebuild script and documentation files in the change list.
- technicalchanges.md
  - Updated to describe the clean rebuild script and documentation updates.

## Added
- clouddeck/clouddeckmanager.cpp
  - Implements the CloudDeck web login/start/pairing flow using Qt WebEngine and signals to update QML UI.
- clouddeck/clouddeckmanager.h
  - Declares the CloudDeck manager API, signals, and internal state used by the QML dialog.
- app/gui/CloudDeckDialog.qml
  - New dialog UI for CloudDeck login/start/pairing operations and status display.
- app/res/cloud.svg
  - Cloud icon used by the new toolbar button.
- clean_rebuild.sh
  - Cleans in‑tree build artifacts and rebuilds the project from the root with `qmake6` and `make`.
