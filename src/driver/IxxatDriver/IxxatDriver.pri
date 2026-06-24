CONFIG += c++20

SOURCES += \
    $$PWD/IxxatInterface.cpp \
    $$PWD/IxxatDriver.cpp

HEADERS  += \
    $$PWD/IxxatInterface.h \
    $$PWD/IxxatDriver.h

# Windows: IXXAT VCI4 SDK.
# Option 1: Extract the SDK's vci/ folder (inc/, lib/) to vci-sdk/ next to this file.
# Option 2: Set IXXAT_VCI_SDK_DIR via environment or qmake argument, pointing at the
#           SDK's "vci" folder, e.g. "C:/Program Files/IXXAT/VCI 3.5/sdk/vci".
win32 {
    isEmpty(IXXAT_VCI_SDK_DIR) {
        exists($$PWD/vci-sdk/inc/vcinpl2.h) {
            IXXAT_VCI_SDK_DIR = $$PWD/vci-sdk
        } else {
            IXXAT_VCI_SDK_DIR = $$(IXXAT_VCI_SDK_DIR)
        }
    }
    isEmpty(IXXAT_VCI_SDK_DIR): error("IxxatDriver: IXXAT VCI4 SDK not found. \
        Either extract its vci/ folder to $$PWD/vci-sdk/ or set IXXAT_VCI_SDK_DIR.")
    INCLUDEPATH += $$IXXAT_VCI_SDK_DIR/inc
    LIBS += $$IXXAT_VCI_SDK_DIR/lib/x64/release/vciapi.lib
    LIBS += $$IXXAT_VCI_SDK_DIR/lib/x64/release/vcinpl2.lib
}

FORMS +=
