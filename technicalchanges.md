# Technical changes

## Modified
- app/app.pro
  - Removes Qt WebEngine dependency; CloudDeck now uses REST/Cognito APIs.
  - Adds CloudDeck API sources/headers and include path so the manager is built and visible to the app.
- app/main.cpp
  - Registers `CloudDeckManagerApi` as a QML singleton for CloudDeck API access.
- app/gui/PcView.qml
  - Adds CloudDeck-aware context menu actions and a richer PC details dialog with CloudDeck credentials and Sunshine defaults.
- app/gui/main.qml
  - Updates the Add PC dialog to offer credential login vs manual entry and removes the separate CloudDeck toolbar button.
- app/gui/computermodel.cpp
  - Adds `findComputerByManualAddress()` helper to resolve a host by manual address for CloudDeck pairing flow.
  - Exposes active/manual address strings for CloudDeck host detection in QML.
- app/gui/computermodel.h
  - Declares the new `findComputerByManualAddress()` QML‑invokable method.
  - Adds QML roles for active/manual addresses.
- clouddeck/clouddeckmanagerapi.cpp
  - Implements CloudDeck REST/Cognito flows (login, machine status/start/stop, add client).
- clouddeck/clouddeckmanagerapi.h
  - Declares CloudDeck REST API surface and QML-facing helpers.
- app/qml.qrc
  - Registers `CloudDeckDialog.qml` in the QML resource list.
- app/resources.qrc
  - Registers the `cloud.svg` icon in the app resources.
- changes.md
  - Updated to include the clean rebuild script and documentation files in the change list.
- technicalchanges.md
  - Updated to describe the clean rebuild script and documentation updates.

## Added
- clouddeck/clouddeckmanagerapi.cpp
  - Implements the CloudDeck REST/Cognito API client with machine lifecycle polling.
- clouddeck/clouddeckmanagerapi.h
  - Declares the CloudDeck REST API surface and QML helpers.
- app/gui/CloudDeckDialog.qml
  - New dialog UI for CloudDeck login/pairing operations and status display.
- app/res/cloud.svg
  - Cloud icon used by the new toolbar button.
- clean_rebuild.sh
  - Cleans in‑tree build artifacts and rebuilds the project from the root with `qmake6` and `make`.
- clean_rebuild.bat
  - Adds a Windows clean rebuild script that bootstraps the VS toolchain/Qt bin path and rebuilds using `qmake` plus `jom`/`nmake`.
  - Copies runtime DLLs from `libs/windows/lib/x64` plus `AntiHooking.dll` into the build output so the release exe has required dependencies (SDL2, SDL2_ttf, etc.).

## Removed
- build_with_vs_env.bat
  - Removed the now-redundant VS environment wrapper script since `clean_rebuild.bat` handles setup.
- clouddeck/clouddeckmanager.cpp
  - Removed the old Qt WebEngine-based CloudDeck manager.
- clouddeck/clouddeckmanager.h
  - Removed the old Qt WebEngine-based CloudDeck manager.
