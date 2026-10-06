TEMPLATE = app
TARGET = tst_languagemanager
DEPENDPATH += .
INCLUDEPATH += .

include(../../autotests.pri)

# Input
SOURCES += tst_languagemanager.cpp

# The test looks up translations in <bindir>/.qm/locale
QMAKE_POST_LINK = $(COPY_DIR) $$shell_path($$PWD/../../../src/.qm) $$shell_path($$OUT_PWD)
