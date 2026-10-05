INCLUDEPATH += $$PWD
DEPENDPATH += $$PWD

HEADERS += \
  $$PWD/cookiedialog.h \
  $$PWD/cookieexceptionsdialog.h \
  $$PWD/cookieexceptionsmodel.h \
  $$PWD/cookiejar.h \
  $$PWD/cookiemodel.h

SOURCES += \
  $$PWD/cookiedialog.cpp \
  $$PWD/cookieexceptionsmodel.cpp \
  $$PWD/cookiemodel.cpp \
  $$PWD/cookieexceptionsdialog.cpp \
  $$PWD/cookiejar.cpp

FORMS += \
    $$PWD/cookies.ui \
    $$PWD/cookiesexceptions.ui

# MIG03: CookieJar no longer derives from NetworkCookieJar — web cookies
# live in the QWebEngineProfile's QWebEngineCookieStore.  The
# networkcookiejar/ sources stay in the tree uncompiled: MIG04 can reuse
# them as the jar for the app-side QNetworkAccessManager.
# include($$PWD/networkcookiejar/networkcookiejar.pri)
