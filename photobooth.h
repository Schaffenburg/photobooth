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

#define CONTROL_VIDEO          'V'     /* start movie capture */
#define CONTROL_PHOTO          'T'     /* photo capture */
#define CONTROL_PAUSE          'P'     /* pause capture */
#define CONTROL_STOP           'S'     /* stop capture thread */
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

#define PREVIEW_FPS 24

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
	CAPTURE_NONE = 0,
	CAPTURE_VIDEO,
	CAPTURE_PHOTO,
	CAPTURE_PAUSED,
	CAPTURE_STOP,
} PhotoboothCaptureThreadState;

typedef enum
{
	PB_STATE_NONE = 0,
	PB_STATE_PREVIEW,
	PB_STATE_TAKING_PHOTO,
	PB_STATE_ASKING,
	PB_STATE_PRINTING
} PhotoboothState;

#define PHOTO_BOOTH_TYPE                (photo_booth_get_type ())
#define PHOTO_BOOTH(obj)                (G_TYPE_CHECK_INSTANCE_CAST ((obj),PHOTO_BOOTH_TYPE,PhotoBooth))
#define PHOTO_BOOTH_CLASS(klass)        (G_TYPE_CHECK_CLASS_CAST ((klass), PHOTO_BOOTH_TYPE,PhotoBoothClass))
#define IS_PHOTO_BOOTH(obj)             (G_TYPE_CHECK_INSTANCE_TYPE ((obj),PHOTO_BOOTH_TYPE))
#define IS_PHOTO_BOOTH_CLASS(klass)     (G_TYPE_CHECK_CLASS_TYPE ((klass), PHOTO_BOOTH_TYPE))

typedef struct _PhotoBooth              PhotoBooth;
typedef struct _PhotoBoothClass         PhotoBoothClass;

typedef struct _CameraInfo              CameraInfo;

struct _PhotoBooth
{
	GtkApplication parent;

	GstElement *pipeline;
	GstElement *video_bin;
	GstElement *photo_bin;
	GstElement *pixoverlay, *video_sink;

	int video_fd;
	gint timeout_id;
	CameraInfo *cam_info;

	gulong video_block_id;
	gulong photo_block_id;

	int control_sock[2];
	GThread *capture_thread;
	PhotoboothState state;
};

struct _PhotoBoothClass
{
	GtkApplicationClass parent_class;
};

GType   photo_booth_get_type    (void);

G_END_DECLS

#endif /* __PHOTO_BOOTH_H__ */
