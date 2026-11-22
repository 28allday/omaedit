# Build from source

On an up-to-date Omarchy or Arch Linux system:

```sh
git clone https://github.com/28allday/omaedit.git ~/omaedit
cd ~/omaedit
./bin/install
```

This installs what building needs, builds the app, its own FFmpeg and the
OpenAPV encoder, copies in the libraries that travel with the package, makes the
package and installs it. Expect it to take a good
while the first time, because of FFmpeg. It fetches two things as it goes:
FFmpeg at a pinned commit and the OpenAPV 1.1.3.0 release, whose checksum is
checked. To update later, run `git pull` and `./bin/install` again.

To build without installing anything, run `./bin/build`. It leaves the program in
`build/omaedit`, and needs the packages listed as `depends` and `makedepends` in
[pkgbuild/PKGBUILD](pkgbuild/PKGBUILD). APV export also needs the encoder that
`./bin/setup-openapv` builds into `build/deps/openapv`.

The version number is `pkgver` in `pkgbuild/PKGBUILD`; the program reads it from
there when it is built.
