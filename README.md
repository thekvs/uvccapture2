## About
Simple application for capturing images from an USB camera on Linux.

## Compiling
* `$ cmake /path/to/uvccapture2/ -DCMAKE_INSTALL_PREFIX=/usr/`
* `$ make`
* `$ cpack -G DEB` to create package in `.deb` format.

Package will be created in the `packages` folder of the build directory.

## Usage
`
$ uvccapture2 -h
Capture images from an USB camera on Linux
Usage:
  uvccapture2 [OPTION...]

  -h, --help            show this help and exit
      --result arg      jpeg image name template
      --device arg      camera's device device use (default: /dev/video0)
      --resolution arg  image's resolution (default: 640x480)
      --quality arg     compression quality for jpeg file
      --skip arg        skip specified number of frames before first capture
      --count arg       number of images to capture
      --pause arg       pause between subsequent captures in seconds
      --loop            run in loop mode, overrides --count
      --strftime        expand the filename with date and time information
      --save-jpeg-asis  store jpeg as we have received it from USB camera
`

## License
See [LICENSE.md](LICENSE.md) file for license information.
