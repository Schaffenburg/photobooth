/*
 * GStreamer photoboothmasquerade.h
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

#ifndef __PHOTO_BOOTH_MASQUERADE_H__
#define __PHOTO_BOOTH_MASQUERADE_H__

#include <gtk/gtk.h>
#include "photobooth.h"

G_BEGIN_DECLS

#define TYPE_PHOTO_BOOTH_MASQUERADE                (photo_booth_masquerade_get_type ())
#define PHOTO_BOOTH_MASQUERADE(obj)                (G_TYPE_CHECK_INSTANCE_CAST ((obj),TYPE_PHOTO_BOOTH_MASQUERADE,PhotoBoothMasquerade))
#define PHOTO_BOOTH_MASQUERADE_CLASS(klass)        (G_TYPE_CHECK_CLASS_CAST ((klass), TYPE_PHOTO_BOOTH_MASQUERADE,PhotoBoothMasqueradeClass))
#define IS_PHOTO_BOOTH_MASQUERADE(obj)             (G_TYPE_CHECK_INSTANCE_TYPE ((obj),TYPE_PHOTO_BOOTH_MASQUERADE))
#define IS_PHOTO_BOOTH_MASQUERADE_CLASS(klass)     (G_TYPE_CHECK_CLASS_TYPE ((klass), TYPE_PHOTO_BOOTH_MASQUERADE))

typedef struct _PhotoBoothMasquerade              PhotoBoothMasquerade;
typedef struct _PhotoBoothMasqueradeClass         PhotoBoothMasqueradeClass;

struct _PhotoBoothMasquerade
{
	GObject parent;
	GList *masks;
};

struct _PhotoBoothMasqueradeClass
{
	GObjectClass parent_class;
};

GType                 photo_booth_masquerade_get_type          (void);
PhotoBoothMasquerade *photo_booth_masquerade_new               (void);
void                  photo_booth_masquerade_init_masks        (PhotoBoothMasquerade *masq, GtkFixed *fixed, const gchar *dir, gchar *list_json, gdouble print_scaling_factor);
void                  photo_booth_masquerade_facedetect_update (PhotoBoothMasquerade *masq, GstStructure *structure);

G_END_DECLS

#endif /* __PHOTO_BOOTH_MASQUERADE_H__ */
