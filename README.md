## About
Simple application for capturing images from an USB camera on Linux.

## Compiling
* `$ cmake /path/to/uvccapture2/ -DCMAKE_INSTALL_PREFIX=/usr/`
* `$ make`
* `$ cpack -G DEB` to create package in `.deb` format.

Package will be created in the `packages` folder of the build directory.

## License
See [LICENSE.md](LICENSE.md) file for license information.
