CC ?= gcc
PKGCONFIG = $(shell which pkg-config)
CFLAGS = $(shell $(PKGCONFIG) --cflags gtk+-3.0 gstreamer-1.0 gstreamer-video-1.0 gstreamer-app-1.0 libgphoto2 libcurl x11 libcanberra-gtk3) -Wl,--export-dynamic -rdynamic -g
LIBS = $(shell $(PKGCONFIG) --libs gtk+-3.0 gstreamer-1.0 gstreamer-video-1.0 gstreamer-app-1.0 libgphoto2 gmodule-export-2.0 libcurl x11 libcanberra-gtk3)
GLIB_COMPILE_RESOURCES = $(shell $(PKGCONFIG) --variable=glib_compile_resources gio-2.0)

SRC = photobooth.c photoboothwin.c focus.c photoboothled.c
BUILT_SRC = resources.c

OBJS = $(BUILT_SRC:.c=.o) $(SRC:.c=.o)

all: photobooth

resources.c: photobooth.gresource.xml $(shell $(GLIB_COMPILE_RESOURCES) --sourcedir=. --generate-dependencies photobooth.gresource.xml)
	$(GLIB_COMPILE_RESOURCES) photobooth.gresource.xml --target=$@ --sourcedir=. --generate-source

%.o: %.c
	$(CC) -c -o $(@F) $(CFLAGS) $<

photobooth: $(OBJS)
	$(CC) -o $(@F) $(LIBS) $(OBJS)

clean:
	rm -f $(BUILT_SRC)
	rm -f $(OBJS)
	rm -f photobooth
