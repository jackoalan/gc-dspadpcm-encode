TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt

SOURCES += main.c \
    grok.c
linux:LIBS += -lasound

DISTFILES += \
    README.md
