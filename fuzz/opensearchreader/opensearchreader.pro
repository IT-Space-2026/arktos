TEMPLATE = app
TARGET = fuzz_opensearchreader
QT = core gui network

INCLUDEPATH += $$PWD/../../src/opensearch
DEPENDPATH += $$PWD/../../src/opensearch

include(../fuzz.pri)

HEADERS += \
    $$PWD/../../src/opensearch/opensearchengine.h \
    $$PWD/../../src/opensearch/opensearchenginedelegate.h \
    $$PWD/../../src/opensearch/opensearchreader.h \
    $$PWD/../../src/opensearch/opensearchwriter.h

SOURCES += fuzz_opensearchreader.cpp \
    $$PWD/../../src/opensearch/opensearchengine.cpp \
    $$PWD/../../src/opensearch/opensearchenginedelegate.cpp \
    $$PWD/../../src/opensearch/opensearchreader.cpp \
    $$PWD/../../src/opensearch/opensearchwriter.cpp
