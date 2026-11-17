SUMMARY = "JSON-configured launcher for TGBS domains"
DESCRIPTION = "Launch JSON-configured TGBS domains in the background through libtgbs, then exit."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://launcher.c file://config.c file://launcher.h"
S = "${WORKDIR}"
DEPENDS = "libtgbs json-c"

CFLAGS:append = " -Wall -Wextra"

do_compile() {
    ${CC} ${CFLAGS} ${LDFLAGS} launcher.c config.c \
        -o tgbs-launcher -ltgbs -ljson-c
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 tgbs-launcher ${D}${bindir}
}
