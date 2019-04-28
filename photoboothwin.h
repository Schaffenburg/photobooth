/*
 * GStreamer photobooth.c
 * Copyright 2016 Andreas Frisch <fraxinas@opendreambox.org>
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

#ifndef __PHOTO_BOOTH_WIN_H__
#define __PHOTO_BOOTH_WIN_H__

#include <gtk/gtk.h>
#include "photobooth.h"

G_BEGIN_DECLS

#define PHOTO_BOOTH_WINDOW_TYPE                (photo_booth_window_get_type ())
#define PHOTO_BOOTH_WINDOW(obj)                (G_TYPE_CHECK_INSTANCE_CAST ((obj), PHOTO_BOOTH_WINDOW_TYPE, PhotoBoothWindow))
#define PHOTO_BOOTH_WINDOW_CLASS(klass)        (G_TYPE_CHECK_CLASS_CAST ((klass),  PHOTO_BOOTH_WINDOW_TYPE, PhotoBoothWindowClass))
#define IS_PHOTO_BOOTH_WINDOW(obj)             (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PHOTO_BOOTH_WINDOW_TYPE))
#define IS_PHOTO_BOOTH_WINDOW_CLASS(klass)     (G_TYPE_CHECK_CLASS_TYPE ((klass),  PHOTO_BOOTH_WINDOW_TYPE))

typedef struct _PhotoBoothWindow               PhotoBoothWindow;
typedef struct _PhotoBoothWindowClass          PhotoBoothWindowClass;

struct _PhotoBoothWindow
{
	GtkApplicationWindow parent;
	GtkWidget *gtkgstwidget;
	GtkFixed *fixed;
	GtkImage *image;
	GtkButton *button_cancel, *button_print, *button_publish, *button_upload;
	GtkSwitch *switch_flip, *switch_facedetect;
	GtkLabel *status_clock, *status, *status_printer;
	GtkComboBox *combo_masquerade;
};

struct _PhotoBoothWindowClass
{
	GtkApplicationWindowClass parent_class;
};

GType                   photo_booth_window_get_type         (void);
PhotoBoothWindow       *photo_booth_window_new              (PhotoBooth *pb);
void                    photo_booth_window_add_gtkgstwidget (PhotoBoothWindow *win, GtkWidget *gtkgstwidget);
void                    photo_booth_window_set_spinner      (PhotoBoothWindow *win, gboolean active);
void                    photo_booth_window_start_countdown  (PhotoBoothWindow *win, gint count);
void                    photo_booth_window_hide_cursor      (PhotoBoothWindow *win);
void                    photo_booth_window_show_cursor      (PhotoBoothWindow *win);
void                    photo_booth_window_set_copies_show  (PhotoBoothWindow *win, gint min, gint max, gint def);
gint                    photo_booth_window_get_copies_hide  (PhotoBoothWindow *win);
void                    photo_booth_window_init_masq_combobox (PhotoBoothWindow *win, GtkListStore *store);

G_END_DECLS

#endif /* __PHOTO_BOOTH_WIN_H__ */
