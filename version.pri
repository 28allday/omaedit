# One version for the app: pkgver in the package's PKGBUILD.
OMAEDIT_PKGBUILD_LINES = $$cat($$PWD/pkgbuild/PKGBUILD, lines)
for(line, OMAEDIT_PKGBUILD_LINES) {
    contains(line, ^pkgver=.*): OMAEDIT_VERSION = $$replace(line, ^pkgver=, )
}
isEmpty(OMAEDIT_VERSION): error("pkgver is missing from pkgbuild/PKGBUILD")
DEFINES += OMAEDIT_VERSION=\\\"$$OMAEDIT_VERSION\\\"
