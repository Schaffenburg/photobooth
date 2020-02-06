/*
 * GStreamer photoboothoverlay.h
 * Copyright 2018 Andreas Frisch <fraxinas@schaffenburg.org>
 *
 * This program is licensed under the Creative Commons
 * Attribution-NonCommercial-ShareAlike 3.0 Unported
 * License. To view a copy of this license, visit
 * http://creativecommons.org/licenses/by-nc-sa/3.0/ or send a letter to
 * Creative Commons,559 Nathan Abbott Way,Stanford,California 94305,USA.
 *
 * This program is NOT free software. It is open source, you are allowed
 * to modify it (if you keep the license), but it may not be commercially
 * distributed other than under the conditions noted above.
 */

#ifndef __PHOTO_BOOTH_OVERLAY_H__
#define __PHOTO_BOOTH_OVERLAY_H__

#include <gtk/gtk.h>
#include "photobooth.h"
#include "photoboothwin.h"

G_BEGIN_DECLS

#define TYPE_PHOTO_BOOTH_OVERLAYS               (photo_booth_overlays_get_type ())
#define PHOTO_BOOTH_OVERLAYS(obj)               (G_TYPE_CHECK_INSTANCE_CAST ((obj),TYPE_PHOTO_BOOTH_OVERLAYS,PhotoBoothOverlays))
#define PHOTO_BOOTH_OVERLAYS_CLASS(klass)       (G_TYPE_CHECK_CLASS_CAST ((klass), TYPE_PHOTO_BOOTH_OVERLAYS,PhotoBoothOverlaysClass))
#define IS_PHOTO_BOOTH_OVERLAYS(obj)            (G_TYPE_CHECK_INSTANCE_TYPE ((obj),TYPE_PHOTO_BOOTH_OVERLAYS))
#define IS_PHOTO_BOOTH_OVERLAYS_CLASS(klass)    (G_TYPE_CHECK_CLASS_TYPE ((klass), TYPE_PHOTO_BOOTH_OVERLAYS))

typedef struct _PhotoBoothOverlays              PhotoBoothOverlays;
typedef struct _PhotoBoothOverlaysClass         PhotoBoothOverlaysClass;

struct _PhotoBoothOverlays
{
	GObject parent;
	GtkListStore *store;
};

struct _PhotoBoothOverlaysClass
{
	GObjectClass parent_class;
};

GType               photo_booth_overlays_get_type   (void);
PhotoBoothOverlays *photo_booth_overlays_new        (PhotoBoothWindow *win, const gchar *dir, gchar *list_json);
void                photo_booth_overlays_set_index  (PhotoBoothOverlays *overlays, guint index, GstElement *photo_bin);
guint               photo_booth_overlays_get_count  (PhotoBoothOverlays *overlays);

#endif /* __PHOTO_BOOTH_OVERLAY_H__ */
