INCLUDEPATH += $$PWD
DEPENDPATH += $$PWD

FORMS += \
    $$PWD/passworddialog.ui \
    $$PWD/proxy.ui

HEADERS += \
    $$PWD/fileaccesshandler.h \
    $$PWD/networkaccessmanager.h \
    $$PWD/networkdiskcache.h \
    $$PWD/networkproxyfactory.h \
    $$PWD/privacyrequestinterceptor.h \
    $$PWD/schemeaccesshandler.h

SOURCES += \
    $$PWD/fileaccesshandler.cpp \
    $$PWD/networkaccessmanager.cpp \
    $$PWD/networkdiskcache.cpp \
    $$PWD/networkproxyfactory.cpp \
    $$PWD/privacyrequestinterceptor.cpp \
    $$PWD/schemeaccesshandler.cpp

# cookiejar/cookiejar.pri is included directly by src.pri (MIG03); the
# networkcookiejar is the volatile jar for the app-side QNAM (MIG04).
include($$PWD/cookiejar/networkcookiejar/networkcookiejar.pri)
