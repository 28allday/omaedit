# One version for the app: pkgver in the package's PKGBUILD.
OMAEDIT_PKGBUILD_LINES = $$cat($$PWD/pkgbuild/PKGBUILD, lines)
for(line, OMAEDIT_PKGBUILD_LINES) {
    contains(line, ^pkgver=.*): OMAEDIT_VERSION = $$replace(line, ^pkgver=, )
}
isEmpty(OMAEDIT_VERSION): error("pkgver is missing from pkgbuild/PKGBUILD")
# Written into a header (rewritten only when the version changes), so the files
# that show it are compiled again when the PKGBUILD's version changes; the
# Makefile is made again whenever the PKGBUILD changes.
OMAEDIT_VERSION_HEADER = $$OUT_PWD/omaedit_version.h
OMAEDIT_VERSION_LINE = "$${LITERAL_HASH}define OMAEDIT_VERSION \"$$OMAEDIT_VERSION\""
OMAEDIT_VERSION_WRITTEN = $$cat($$OMAEDIT_VERSION_HEADER, lines)
!equals(OMAEDIT_VERSION_WRITTEN, $$OMAEDIT_VERSION_LINE) {
    write_file($$OMAEDIT_VERSION_HEADER, OMAEDIT_VERSION_LINE)|error("cannot write $$OMAEDIT_VERSION_HEADER")
}
INCLUDEPATH *= $$OUT_PWD
QMAKE_INTERNAL_INCLUDED_FILES *= $$PWD/pkgbuild/PKGBUILD
