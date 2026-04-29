!win32: error("ZsCanFdDriver is only supported on Windows")

CONFIG += c++20

# Qt serialbus custom qt_zscanfd canbus plugin must be downloaded on the target machine.
# The zscanfd.dll must be downloaded on the target machine.
QT += serialbus

SOURCES += \
    $$PWD/ZsCanFdInterface.cpp \
    $$PWD/ZsCanFdDriver.cpp

HEADERS += \
    $$PWD/ZsCanFdInterface.h \
    $$PWD/ZsCanFdDriver.h

FORMS +=
