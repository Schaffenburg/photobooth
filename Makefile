CC ?= gcc
PKGCONFIG = $(shell which pkg-config)
CFLAGS = $(shell $(PKGCONFIG) --cflags gtk+-3.0 gstreamer-1.0 gstreamer-video-1.0 gstreamer-app-1.0 libgphoto2) -Wl,--export-dynamic -rdynamic -g
LIBS = $(shell $(PKGCONFIG) --libs gtk+-3.0 gstreamer-1.0 gstreamer-video-1.0 gstreamer-app-1.0 libgphoto2 gmodule-export-2.0)
GLIB_COMPILE_RESOURCES = $(shell $(PKGCONFIG) --variable=glib_compile_resources gio-2.0)
GLIB_COMPILE_SCHEMAS = $(shell $(PKGCONFIG) --variable=glib_compile_schemas gio-2.0)

SRC = photobooth.c photoboothwin.c focus.c
BUILT_SRC = resources.c

OBJS = $(BUILT_SRC:.c=.o) $(SRC:.c=.o)

all: photobooth

org.schaffenburg.photobooth.gschema.valid: org.schaffenburg.photobooth.gschema.xml
	$(GLIB_COMPILE_SCHEMAS) --strict --dry-run --schema-file=$< && mkdir -p $(@D) && touch $@

gschemas.compiled: org.schaffenburg.photobooth.gschema.valid
	$(GLIB_COMPILE_SCHEMAS) .
	
resources.c: photobooth.gresource.xml $(shell $(GLIB_COMPILE_RESOURCES) --sourcedir=. --generate-dependencies photobooth.gresource.xml)
	$(GLIB_COMPILE_RESOURCES) photobooth.gresource.xml --target=$@ --sourcedir=. --generate-source

%.o: %.c
	$(CC) -c -o $(@F) $(CFLAGS) $<

photobooth: $(OBJS) gschemas.compiled
	$(CC) -o $(@F) $(LIBS) $(OBJS)

clean:
	rm -f org.schaffenburg.photobooth.gschema.valid
	rm -f gschemas.compiled
	rm -f $(BUILT_SRC)
	rm -f $(OBJS)
	rm -f photobooth
