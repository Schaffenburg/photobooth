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

// gcc -Wall -g `pkg-config gstreamer-1.0 gstreamer-video-1.0 libgphoto2 --cflags --libs gtk+-3.0 gtk+-x11-3.0` photobooth.c -o photobooth

#include <poll.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gst/video/videooverlay.h>
#include "photobooth.h"

#define photo_booth_parent_class parent_class
G_DEFINE_TYPE (PhotoBooth, photo_booth, G_TYPE_OBJECT);

GST_DEBUG_CATEGORY_STATIC (photo_booth_debug);
#define GST_CAT_DEFAULT photo_booth_debug

gboolean _pb_cam_init (CameraInfo *cam_info);
gboolean _pb_cam_close (CameraInfo *cam_info);
void _pb_flush_pipe (int fd);
static void _pb_video_capture_thread_func (PhotoBooth *pb);

static void photo_booth_finalize (GObject * object);

static void photo_booth_class_init (PhotoBoothClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
	GST_DEBUG_CATEGORY_INIT (photo_booth_debug, "photobooth", GST_DEBUG_BOLD | GST_DEBUG_FG_YELLOW | GST_DEBUG_BG_BLUE, "PhotoBooth");
	GST_DEBUG ("photo_booth_class_init");
	gobject_class->finalize = photo_booth_finalize;
}

static void photo_booth_init (PhotoBooth *pb)
{
	GST_DEBUG_OBJECT (pb, "photo_booth_init init object!");
	
	int control_sock[2];
	if (socketpair (PF_UNIX, SOCK_STREAM, 0, control_sock) < 0)
	{
		GST_ERROR_OBJECT (pb, "cannot create control sockets: %s (%i)", strerror(errno), errno);
		return;
	}
	READ_SOCKET (pb) = control_sock[0];
	WRITE_SOCKET (pb) = control_sock[1];
	fcntl (READ_SOCKET (pb), F_SETFL, O_NONBLOCK);
	fcntl (WRITE_SOCKET (pb), F_SETFL, O_NONBLOCK);

	if (!_pb_cam_init (&pb->cam_info))
		g_error ("can't init cam!");

	pb->video_capture_thread = NULL;
	pb->video_capture_thread = g_thread_try_new ("video-capture", (GThreadFunc) _pb_video_capture_thread_func, pb, NULL);
}

static void photo_booth_finalize (GObject * object)
{
	PhotoBooth *pb = PHOTO_BOOTH (object);
	GST_INFO_OBJECT (pb, "finalize server");
	SEND_COMMAND (pb, CONTROL_STOP);
	_pb_flush_pipe (pb->video_fd);
	g_thread_join (pb->video_capture_thread);
	_pb_cam_close (&pb->cam_info);
}
static void _gphoto_gst_errordumper(GPLogLevel level, const char *domain, const char *str, void *data)
{
	GST_DEBUG ("GPhoto %d, %s:%s", (int) level, domain, str);
}

gboolean _pb_cam_init (CameraInfo *cam_info)
{
	int retval;

	cam_info->context = gp_context_new();
	gp_log_add_func(GP_LOG_ERROR, _gphoto_gst_errordumper, NULL);
	gp_camera_new(&cam_info->camera);
	retval = gp_camera_init(cam_info->camera, cam_info->context);
	GST_DEBUG ("gp_camera_init returned %d", retval);
	if (retval != GP_OK) {
		return FALSE;
	}
	return TRUE;
}

gboolean _pb_cam_close (CameraInfo *cam_info)
{
	int retval;
	retval = gp_camera_exit(cam_info->camera, cam_info->context);
	GST_DEBUG ("gp_camera_exit returned %i", retval);
	return GP_OK ? TRUE : FALSE;
}

void _pb_flush_pipe (int fd)
{
	int rlen = 0;
	unsigned char buf[1024];
	fcntl (fd, F_SETFL, O_NONBLOCK);
	while (rlen != -1)
		rlen = read (fd, buf, sizeof(buf));
}
void _pb_quit_signal (GMainLoop *loop)
{
	GST_INFO_OBJECT (loop, "caught SIGINT");
	g_main_loop_quit (loop);
}

static void _pb_video_capture_thread_func (PhotoBooth *pb)
{
	PhotoboothReadthreadState state = CAPTURETHREAD_NONE;
	
	mkfifo("moviepipe", 0666);
	pb->video_fd = open("moviepipe", O_RDWR);

	GST_DEBUG_OBJECT (pb, "enter video capture thread fd = %d", pb->video_fd);
		
	CameraFile *gp_file = NULL;
	int gp_r, captured_frames = 0;

	if (gp_file_new_from_fd (&gp_file, pb->video_fd) != GP_OK)
	{
		GST_ERROR_OBJECT (pb, "couldn't start video capture thread because gp_file_new_from_fd (%d) failed!", pb->video_fd);
		goto stop_running;
	}

	while (TRUE) {
		if (state == CAPTURETHREAD_STOP)
			goto stop_running;

		struct pollfd rfd[2];
			int timeout = 1000 / PREVIEW_FPS;
			rfd[0].fd = READ_SOCKET (pb);
			rfd[0].events = POLLIN | POLLERR | POLLHUP | POLLPRI;

		int ret = poll(rfd, 1, timeout);
		if (G_UNLIKELY (ret == -1))
		{
			GST_ERROR_OBJECT (pb, "SELECT ERROR!");
			goto stop_running;
		}
		else if ( ret == 0 )
		{
			const char *mime;
			gp_r = gp_camera_capture_preview (pb->cam_info.camera, gp_file, pb->cam_info.context);
			if (gp_r < 0) {
				GST_ERROR_OBJECT (pb, "Movie capture error.");
				state = CAPTURETHREAD_STOP;
				break;
			}
			gp_file_get_mime_type (gp_file, &mime);
			if (strcmp (mime, GP_MIME_JPEG)) {
				g_print ("Movie capture error... Unhandled MIME type '%s'.", mime);
				state = CAPTURETHREAD_STOP;
				break;
			}
			captured_frames++;
		}
		else if ( rfd[0].revents )
		{
			char command;
			READ_COMMAND (pb, command, ret);
			switch (command) {
				case CONTROL_STOP:
					GST_DEBUG_OBJECT (pb, "CONTROL_STOP!");
					state = CAPTURETHREAD_STOP;
					break;
				case CONTROL_RUN:
					GST_DEBUG_OBJECT (pb, "CONTROL_RUN");
					state = CAPTURETHREAD_RUN;
					break;
				default:
					GST_ERROR_OBJECT (pb, "illegal control socket command %c received!", command);
			}
			continue;
		}
	}
	
	g_assert_not_reached ();
	return;

	stop_running:
	{
		if (gp_file)
			gp_file_unref (gp_file);
		if (pb->video_fd)
			close (pb->video_fd);
		GST_DEBUG ("stop running, exit thread, %d frames captured", captured_frames);
		return;
	}

}

int main (int argc, char *argv[])
{
	GMainLoop *loop;
	PhotoBooth *pb;
	
	gst_init (&argc, &argv);
	gtk_init (0, NULL);
	
	loop = g_main_loop_new (NULL, FALSE);
	
	g_unix_signal_add (SIGINT, (GSourceFunc) _pb_quit_signal, loop);

	pb = g_object_new (PHOTO_BOOTH_TYPE, NULL);

	g_main_loop_run (loop);
	
	g_object_unref (pb);
	return 0;
}

