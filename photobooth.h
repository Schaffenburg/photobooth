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

#ifndef __PHOTO_BOOTH_H__
#define __PHOTO_BOOTH_H__

#include <glib-unix.h>
#include <glib-object.h>
#include <gtk/gtk.h>
#include <gst/gst.h>
#include <gphoto2/gphoto2.h>
#include <gphoto2/gphoto2-camera.h>

#define CONTROL_VIDEO          '1'     /* start movie capture */
#define CONTROL_PRETRIGGER     '2'     /* pretrigger */
#define CONTROL_PHOTO          '3'     /* photo capture */
#define CONTROL_PAUSE          '4'     /* pause capture */
#define CONTROL_UNPAUSE        '5'     /* unpause capture */
#define CONTROL_QUIT           '0'     /* quit capture thread */
#define CONTROL_SOCKETS(src)   src->control_sock
#define WRITE_SOCKET(src)      src->control_sock[1]
#define READ_SOCKET(src)       src->control_sock[0]

#define CLEAR_COMMAND(src)                  \
G_STMT_START {                              \
  char c;                                   \
  read(READ_SOCKET(src), &c, 1);            \
} G_STMT_END

#define SEND_COMMAND(src, command)          \
G_STMT_START {                              \
  int G_GNUC_UNUSED _res; unsigned char c; c = command;   \
  _res = write (WRITE_SOCKET(src), &c, 1);  \
} G_STMT_END

#define READ_COMMAND(src, command, res)        \
G_STMT_START {                                 \
  res = read(READ_SOCKET(src), &command, 1);   \
} G_STMT_END

G_BEGIN_DECLS

struct _CameraInfo {
	Camera *camera;
	GPContext *context;
	GMutex mutex;
	int preview_capture_count;
	char *data;
	unsigned long size;
};

typedef enum
{
	CAPTURE_INIT = 0,
	CAPTURE_VIDEO,
	CAPTURE_PRETRIGGER,
	CAPTURE_PHOTO,
	CAPTURE_PAUSED,
	CAPTURE_UNPAUSE,
	CAPTURE_FAILED,
	CAPTURE_QUIT,
} PhotoboothCaptureThreadState;

typedef enum
{
	PB_STATE_NONE = 0,
	PB_STATE_PREVIEW,
	PB_STATE_PREVIEW_COOLDOWN,
	PB_STATE_COUNTDOWN,
	PB_STATE_TAKING_PHOTO,
	PB_STATE_PROCESS_PHOTO,
	PB_STATE_WAITING_FOR_ANSWER,
	PB_STATE_PRINTING,
	PB_STATE_SCREENSAVER
} PhotoboothState;

gchar *G_template_filename;
gchar *G_stylesheet_filename;
GHashTable *G_strings_table;
#define _(key) (g_hash_table_contains (G_strings_table, key) ? g_hash_table_lookup (G_strings_table, key) : key)

#define PHOTO_BOOTH_TYPE                (photo_booth_get_type ())
#define PHOTO_BOOTH(obj)                (G_TYPE_CHECK_INSTANCE_CAST ((obj),PHOTO_BOOTH_TYPE,PhotoBooth))
#define PHOTO_BOOTH_CLASS(klass)        (G_TYPE_CHECK_CLASS_CAST ((klass), PHOTO_BOOTH_TYPE,PhotoBoothClass))
#define IS_PHOTO_BOOTH(obj)             (G_TYPE_CHECK_INSTANCE_TYPE ((obj),PHOTO_BOOTH_TYPE))
#define IS_PHOTO_BOOTH_CLASS(klass)     (G_TYPE_CHECK_CLASS_TYPE ((klass), PHOTO_BOOTH_TYPE))
#define PHOTO_BOOTH_FROM_WINDOW(win)    (PHOTO_BOOTH (gtk_window_get_application (GTK_WINDOW (win))))

typedef struct _PhotoBooth              PhotoBooth;
typedef struct _PhotoBoothClass         PhotoBoothClass;

typedef struct _CameraInfo              CameraInfo;

struct _PhotoBooth
{
	GtkApplication parent;

	GstElement *pipeline;
	GstElement *video_bin;
	GstElement *photo_bin;
	GstElement *video_sink;

	int video_fd;
	gint timeout_id;
	CameraInfo *cam_info;

	int control_sock[2];
};

struct _PhotoBoothClass
{
	GtkApplicationClass parent_class;
};

GType   photo_booth_get_type    (void);
PhotoBooth *photo_booth_new (void);
void    photo_booth_load_settings (PhotoBooth *pb, const gchar *filename);

G_END_DECLS

#endif /* __PHOTO_BOOTH_H__ */
