# ENG03 spike harness — NOT in the top-level SUBDIRS build.
# Requires: cargo build (or --release) in ../servo-embed first.
#   cd spikes/servo/harness && qmake6 && make
TEMPLATE = app
TARGET = servospike
QT += widgets
CONFIG += c++17 warn_on

INCLUDEPATH += $$PWD/../servo-embed/include
SOURCES += main.cpp servoview.cpp
HEADERS += servoview.h

# The cdylib carries the whole servo dep tree — link the .so, rpath it.
SERVO_EMBED_LIB = $$PWD/../servo-embed/target/release
!exists($$SERVO_EMBED_LIB/libservo_embed.so): SERVO_EMBED_LIB = $$PWD/../servo-embed/target/debug
LIBS += -L$$SERVO_EMBED_LIB -lservo_embed
QMAKE_RPATHDIR += $$SERVO_EMBED_LIB
