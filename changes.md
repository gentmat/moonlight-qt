# Changes from original fork

## Modified
- app/app.pro
- app/main.cpp
- app/gui/PcView.qml
- app/gui/CloudDeckDialog.qml
- app/gui/StreamSegue.qml
- app/gui/main.qml
- app/gui/computermodel.cpp
- app/gui/computermodel.h
- app/streaming/session.cpp
- app/streaming/session.h
- app/streaming/video/overlaymanager.cpp
- app/streaming/video/overlaymanager.h
- app/streaming/video/ffmpeg-renderers/d3d11va.cpp
- app/streaming/video/ffmpeg-renderers/drm.cpp
- app/streaming/video/ffmpeg-renderers/dxva2.cpp
- app/streaming/video/ffmpeg-renderers/eglvid.cpp
- app/streaming/video/ffmpeg-renderers/plvk.cpp
- app/streaming/video/ffmpeg-renderers/sdlvid.cpp
- app/streaming/video/ffmpeg-renderers/vaapi.cpp
- app/streaming/video/ffmpeg-renderers/vdpau.cpp
- app/streaming/video/ffmpeg-renderers/vt_avsamplelayer.mm
- app/streaming/video/ffmpeg-renderers/vt_metal.mm
- clouddeck/clouddeckmanagerapi.cpp
- clouddeck/clouddeckmanagerapi.h
- app/qml.qrc
- app/resources.qrc
- changes.md
- clean_rebuild.bat
- technicalchanges.md

## Added
- clouddeck/clouddeckmanagerapi.cpp
- clouddeck/clouddeckmanagerapi.h
- app/gui/CloudDeckDialog.qml
- app/res/cloud.svg
- clean_rebuild.sh
- clean_rebuild.bat

## Recent updates
- Split right-click options into three views: `View Details`, `CloudDeck Settings`, and `Session Timer Settings` for CloudDeck hosts.
- Refactored CloudDeck/session timer dialogs to a more compact design.
- Set defaults to `Only show before end` at `5` minutes and `Show each hour passed` enabled for `5` seconds.
- Hardened CloudDeck start/session-timer flow to avoid stale callback races while instances are still `starting`.
- Handled CloudDeck machine-command `409` conflicts during restart/transition as non-fatal and continued polling instead of failing the flow.
- Guarded CloudDeck pairing against invalid/empty `public_ip` responses and normalized status parsing to avoid QML runtime crashes.
- Added a safety guard in `ComputerModel::handleComputerStateChanged()` for stale computer pointers to prevent invalid `dataChanged()` index emissions.
