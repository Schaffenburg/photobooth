# Schaffenburg e.V.'s Photobooth Software

## OwO What's this?
This is a linux GUI application which captures live preview and exposures from a DSLR Camera and allows printing or social media sharing the photos.

(CC) 2016-2019 by Andreas Frisch (Fraxinas) <photobooth@schaffenburg.org>

## Features
* Uses libgphoto2 to acquire live preview, trigger exposures and download photos via USB-tethering from ~380 supported DSLR models [1]
* Support for dye-sublimation printers through gutenprint
* Placement of individual full-screen overlay image (PNG with alpha transparency)
* GDPR-aware: allows for photos to be automatically kept or deleted or prompted each time
* Photos can be privately uploaded to a linx server with a QR code for the user to download them
* Photos can be published to imgur / facebook (with optional twitter bridge)
* GUI designed for single-touch screens
* GUI is fully customizable, the widgets can be positioned in a template `.ui` file and styled in a `.css` file
* Slider for choosing of how many copies to print (thresholds can be set in the config)
* Strings can easily be replaced/translated, with UTF-8 & color emoji support
* All times can be customized (exposure countdown, save/upload/print idle timeouts etc.)
* Optional face detection for automatic placing of mask overlays
* Optional ICC color correction
* Sound output for countdown beep and GUI feedback
* Controller for optional arduino-driven LED effects

## Building
Initially developed and tested under `ARCH Linux` [2].

Requires `GTK3+ >3.20`

To install this under *ubuntuish distros:
```
sudo add-apt-repository ppa:gnome3-team/gnome3-staging
sudo add-apt-repository ppa:gnome3-team/gnome3
sudo apt-get update
```

Photobooth uses the `meson` [3] build system
```
pip3 install meson
```
`pacman -S ninja` or `apt-get install ninja-build`

To build, please run:

```
meson build
ninja -C build
```

## Running
```
build/photobooth
```
will run the software with the default configuration from `default.ini`
* the only command line argument is an alternative config file, where you can specify behaviour, graphics, texts etc.
* for troubleshooting, use the `GST_DEBUG=*photobooth*:LOG` environmental variable
* optionally uses the `facedetect` element from `gst-plugins-bad` which depends on `OpenCV` [4]
* optionally uses my fork of the `qroverlay` element [5]

## References
* https://wiki.schaffenburg.org/Projekt:Photobooth
* [1] http://www.gphoto.org/proj/libgphoto2/support.php
* [2] https://archlinux.org
* [3] https://mesonbuild.com/
* [4] https://gitlab.freedesktop.org/gstreamer/gst-plugins-bad
* [5] https://github.com/fraxinas/gst-qroverlay
