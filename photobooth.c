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

// gcc -Wall -g `pkg-config gstreamer-1.0 gstreamer-video-1.0 libgphoto2 gtk+-3.0 gtk+-x11-3.0 --cflags --libs` photobooth.c focus.c -o photobooth

#include <poll.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <gdk/gdkx.h>
#include <gst/video/videooverlay.h>
#include "photobooth.h"

#define photo_booth_parent_class parent_class
G_DEFINE_TYPE (PhotoBooth, photo_booth, GTK_TYPE_APPLICATION);

GST_DEBUG_CATEGORY_STATIC (photo_booth_debug);
#define GST_CAT_DEFAULT photo_booth_debug

/* GObject / GApplication */
static void photo_booth_activate (GApplication *app);
static void photo_booth_open (GApplication  *app, GFile **files, gint n_files, const gchar *hint);
static void photo_booth_finalize (GObject *object);
PhotoBooth *photo_booth_new (void);
static void photo_booth_clicked (GtkWidget *button, GdkEventButton *event, PhotoBooth *pb);

/* general private functions */
static void photo_booth_quit_signal (GMainLoop *loop);
static void photo_booth_window_destroyed_signal (PhotoBoothWindow *win, GMainLoop *loop);
static void photo_booth_setup_window (PhotoBooth *pb);
static void photo_booth_preview (PhotoBooth *pb);
static void photo_booth_snapshot (PhotoBooth *pb);

/* libgphoto2 */
static gboolean photo_booth_cam_init (CameraInfo **cam_info);
static gboolean photo_booth_cam_close (CameraInfo **cam_info);
static void photo_booth_flush_pipe (int fd);
static void photo_booth_video_capture_thread_func (PhotoBooth *pb);

/* gstreamer functions */
static gboolean photo_booth_setup_gstreamer (PhotoBooth *pb);
static GstBusSyncReply photo_booth_bus_sync_callback (GstBus *bus, GstMessage *message, GtkWidget *drawing_area);
static gboolean photo_booth_bus_callback (GstBus *bus, GstMessage *message, PhotoBooth *pb);

static void photo_booth_class_init (PhotoBoothClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
	GApplicationClass *gapplication_class = G_APPLICATION_CLASS (klass);

	GST_DEBUG_CATEGORY_INIT (photo_booth_debug, "photobooth", GST_DEBUG_BOLD | GST_DEBUG_FG_YELLOW | GST_DEBUG_BG_BLUE, "PhotoBooth");
	GST_DEBUG ("photo_booth_class_init");
	gobject_class->finalize = photo_booth_finalize;
	gapplication_class->activate = photo_booth_activate;
	gapplication_class->open = photo_booth_open;
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

	if (!photo_booth_cam_init (&pb->cam_info))
		GST_ERROR_OBJECT (pb, "can't init cam!");

	pb->state = PB_STATE_NONE;
	pb->video_block_id = 0;
	pb->photo_block_id = 0;

	pb->video_capture_thread = NULL;
	pb->video_capture_thread = g_thread_try_new ("video-capture", (GThreadFunc) photo_booth_video_capture_thread_func, pb, NULL);
}

static void photo_booth_setup_window (PhotoBooth *pb)
{
	PhotoBoothWindow *win = photo_booth_window_new (pb);
	gtk_window_present (GTK_WINDOW (win));
	GdkScreen *screen = gdk_screen_get_default ();
	gtk_window_fullscreen_on_monitor (GTK_WINDOW (win), screen, 0);
	GdkWindow *w = gdk_screen_get_active_window (screen);
	gint m = gdk_screen_get_monitor_at_window (screen, w);
	gdk_screen_get_monitor_geometry (screen, m, &pb->monitor_geo);
	GST_INFO_OBJECT (pb, "monitor geometry %dx%d", pb->monitor_geo.width, pb->monitor_geo.height);
	g_signal_connect (G_OBJECT (win), "destroy", G_CALLBACK (photo_booth_window_destroyed_signal), pb->loop);
	pb->overlay = gtk_overlay_new ();
	gtk_container_add (GTK_CONTAINER (win), pb->overlay);
	pb->drawing_area = gtk_drawing_area_new ();
	gtk_container_add (GTK_CONTAINER (pb->overlay), pb->drawing_area);
	g_signal_connect (G_OBJECT (pb->drawing_area), "button-press-event", G_CALLBACK (photo_booth_clicked), pb);
	gtk_widget_add_events (pb->drawing_area, GDK_BUTTON_PRESS_MASK);
	gtk_widget_show_all (pb->overlay);
	gtk_widget_show_all (GTK_WIDGET (win));
	photo_booth_setup_gstreamer (pb);
}

static void photo_booth_activate (GApplication *app)
{
	GST_DEBUG_OBJECT (app, "photo_booth_activate");
	photo_booth_setup_window (PHOTO_BOOTH (app));
}

static void photo_booth_open (GApplication  *app, GFile **files, gint n_files, const gchar *hint)
{
	GST_DEBUG_OBJECT (app, "photo_booth_open");
	photo_booth_setup_window (PHOTO_BOOTH (app));
}

static void photo_booth_finalize (GObject *object)
{
	PhotoBooth *pb = PHOTO_BOOTH (object);
	GST_INFO_OBJECT (pb, "finalize server");
	SEND_COMMAND (pb, CONTROL_STOP);
	photo_booth_flush_pipe (pb->video_fd);
	g_thread_join (pb->video_capture_thread);
	if (pb->cam_info)
		photo_booth_cam_close (&pb->cam_info);
}

static void _gphoto_gst_errordumper(GPLogLevel level, const char *domain, const char *str, void *data)
{
	GST_DEBUG ("GPhoto %d, %s:%s", (int) level, domain, str);
}

static GstPadProbeReturn _gst_photo_probecb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
	GST_DEBUG_OBJECT (pad, "drop photo");
	return GST_PAD_PROBE_DROP;
}
static GstPadProbeReturn _gst_video_probecb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
	GST_DEBUG_OBJECT (pad, "drop video");
	return GST_PAD_PROBE_DROP;
}

static gboolean photo_booth_cam_init (CameraInfo **cam_info)
{
	int retval;
	*cam_info = (CameraInfo*)malloc(sizeof(struct _CameraInfo));
	if (!cam_info)
		return FALSE;

	g_mutex_init (&(*cam_info)->mutex);
	g_mutex_lock (&(*cam_info)->mutex);
	(*cam_info)->preview_capture_count = 0;
	(*cam_info)->context = gp_context_new();
	gp_log_add_func(GP_LOG_ERROR, _gphoto_gst_errordumper, NULL);
	gp_camera_new(&(*cam_info)->camera);
	retval = gp_camera_init((*cam_info)->camera, (*cam_info)->context);
	GST_DEBUG ("gp_camera_init returned %d cam_info@%p camera@%p", retval, *cam_info, (*cam_info)->camera);
	g_mutex_unlock (&(*cam_info)->mutex);
	if (retval != GP_OK) {
		g_mutex_clear (&(*cam_info)->mutex);
		free (*cam_info);
		*cam_info = NULL;
		return FALSE;
	}
	return TRUE;
}

static gboolean photo_booth_cam_close (CameraInfo **cam_info)
{
	int retval;
	g_mutex_lock (&(*cam_info)->mutex);
	retval = gp_camera_exit((*cam_info)->camera, (*cam_info)->context);
	GST_DEBUG ("gp_camera_exit returned %i", retval);
	g_mutex_unlock (&(*cam_info)->mutex);
	g_mutex_clear (&(*cam_info)->mutex);
	free (*cam_info);
	*cam_info = NULL;
	return GP_OK ? TRUE : FALSE;
}

static void photo_booth_flush_pipe (int fd)
{
	int rlen = 0;
	unsigned char buf[1024];
	fcntl (fd, F_SETFL, O_NONBLOCK);
	while (rlen != -1)
		rlen = read (fd, buf, sizeof(buf));
}

static void photo_booth_quit_signal (GMainLoop *loop)
{
	GST_INFO_OBJECT (loop, "caught SIGINT");
	g_main_loop_quit (loop);
}

static void photo_booth_window_destroyed_signal (PhotoBoothWindow *win, GMainLoop *loop)
{
	GST_INFO_OBJECT (win, "window destroyed");
	g_main_loop_quit (loop);
}

static void photo_booth_video_capture_thread_func (PhotoBooth *pb)
{
	PhotoboothReadthreadState state = CAPTURETHREAD_NONE;

	mkfifo("moviepipe", 0666);
	pb->video_fd = open("moviepipe", O_RDWR);

	GST_DEBUG_OBJECT (pb, "enter video capture thread fd = %d cam_info@%p", pb->video_fd, pb->cam_info);

	CameraFile *gp_file = NULL;
	int gpret, captured_frames = 0;

	if (gp_file_new_from_fd (&gp_file, pb->video_fd) != GP_OK)
	{
		GST_ERROR_OBJECT (pb, "couldn't start video capture thread because gp_file_new_from_fd (%d) failed!", pb->video_fd);
		goto stop_running;
	}

	while (TRUE) {
		if (state == CAPTURETHREAD_STOP)
			goto stop_running;

		struct pollfd rfd[2];
		int timeout = 0;
		rfd[0].fd = READ_SOCKET (pb);
		rfd[0].events = POLLIN | POLLERR | POLLHUP | POLLPRI;

		if (!pb->cam_info)
		{
			if (!photo_booth_cam_init (&pb->cam_info))
				GST_DEBUG_OBJECT (pb, "no camera info - can't capture!");
			timeout = 10000;
		}
		else if (pb->state == PB_STATE_NONE)
			pb->state = PB_STATE_PREVIEW;
		else if (state == CAPTURETHREAD_PAUSED)
			timeout = 1000;
		else
			timeout = 1000 / PREVIEW_FPS;

		int ret = poll(rfd, 1, timeout);
		if (G_UNLIKELY (ret == -1))
		{
			GST_ERROR_OBJECT (pb, "SELECT ERROR!");
			goto stop_running;
		}
		else if (ret == 0 && state >= CAPTURETHREAD_RUN)
		{
			const char *mime;
			if (pb->cam_info)
			{
				g_mutex_lock (&pb->cam_info->mutex);
				gpret = gp_camera_capture_preview (pb->cam_info->camera, gp_file, pb->cam_info->context);
				g_mutex_unlock (&pb->cam_info->mutex);
				if (gpret < 0) {
					GST_ERROR_OBJECT (pb, "Movie capture error %d", gpret);
					state = CAPTURETHREAD_STOP;
					continue;
				}
				else {
					g_mutex_lock (&pb->cam_info->mutex);
					gp_file_get_mime_type (gp_file, &mime);
					g_mutex_unlock (&pb->cam_info->mutex);
					if (strcmp (mime, GP_MIME_JPEG)) {
						GST_ERROR_OBJECT ("Movie capture error... Unhandled MIME type '%s'.", mime);
						state = CAPTURETHREAD_STOP;
						continue;
					}
					captured_frames++;
					GST_DEBUG_OBJECT (pb, "captured frame (%d frames total)", captured_frames);
				}
			}
		}
		else if (rfd[0].revents)
		{
			char command;
			READ_COMMAND (pb, command, ret);
			switch (command) {
				case CONTROL_STOP:
					GST_DEBUG_OBJECT (pb, "CONTROL_STOP!");
					state = CAPTURETHREAD_STOP;
					break;
				case CONTROL_PAUSE:
					GST_DEBUG_OBJECT (pb, "CONTROL_PAUSE!");
					state = CAPTURETHREAD_PAUSED;
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
		else if (state == CAPTURETHREAD_PAUSED)
		{
			GST_DEBUG_OBJECT (pb, "captured thread paused... timeout");
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

static GstElement *build_video_bin (PhotoBooth *pb)
{
	GstElement *video_bin;
	GstElement *mjpeg_source, *mjpeg_decoder, *mjpeg_filter, *video_filter, *video_scale, *video_convert;
	GstCaps *caps;
	GstPad *ghost, *pad;

	video_bin = gst_element_factory_make ("bin", "videobin");
	mjpeg_source = gst_element_factory_make ("fdsrc", "mjpeg-fdsrc");
	g_object_set (mjpeg_source, "fd", pb->video_fd, NULL);
	g_object_set (mjpeg_source, "do-timestamp", TRUE, NULL);
	g_object_set (mjpeg_source, "blocksize", 65536, NULL);

	mjpeg_filter = gst_element_factory_make ("capsfilter", "mjpeg-capsfilter");
	caps = gst_caps_new_simple ("image/jpeg", "width", G_TYPE_INT, 640, "height", G_TYPE_INT, 424, "framerate", GST_TYPE_FRACTION, PREVIEW_FPS, 1, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, NULL);
	g_object_set (G_OBJECT (mjpeg_filter), "caps", caps, NULL);
	gst_caps_unref (caps);

	mjpeg_decoder = gst_element_factory_make ("jpegdec", "mjpeg-decoder");
	video_scale = gst_element_factory_make ("videoscale", "mjpeg-videoscale");
	video_convert = gst_element_factory_make ("videoconvert", "mjpeg-videoconvert");
	video_filter = gst_element_factory_make ("capsfilter", "video-capsfilter");
	caps = gst_caps_new_simple ("video/x-raw", "width", G_TYPE_INT, pb->monitor_geo.width, "height", G_TYPE_INT, pb->monitor_geo.height, NULL);
	g_object_set (G_OBJECT (video_filter), "caps", caps, NULL);
	gst_caps_unref (caps);

	if (!(mjpeg_source && mjpeg_filter && mjpeg_decoder && video_scale && video_convert && video_filter))
	{
		GST_ERROR_OBJECT (video_bin, "Failed to videobin pipeline element(s):%s%s%s%s%s%s", mjpeg_source?"":" fdsrc", mjpeg_filter?"":" capsfilter", mjpeg_decoder?"":" jpegdec",
			video_scale?"":" videoscale", video_convert?"":" videoconvert", video_filter?"":" capsfilter");
		return FALSE;
	}

	gst_bin_add_many (GST_BIN (video_bin), mjpeg_source, mjpeg_filter, mjpeg_decoder, video_scale, video_convert, video_filter, NULL);

	if (!gst_element_link_many (mjpeg_source, mjpeg_filter, mjpeg_decoder, video_scale, video_convert, video_filter, NULL))
	{
		GST_ERROR_OBJECT(video_bin, "couldn't link videobin elements!");
		return FALSE;
	}

	pad = gst_element_get_static_pad (video_filter, "src");
	ghost = gst_ghost_pad_new ("src", pad);
	gst_object_unref (pad);
	gst_pad_set_active (ghost, TRUE);
	gst_element_add_pad (video_bin, ghost);
	return video_bin;
}

static GstElement *build_photo_bin (PhotoBooth *pb)
{
	GstElement *photo_bin;
	GstElement *photo_source, *photo_decoder, *photo_freeze, *photo_scale, *photo_filter;
	GstCaps *caps;
	GstPad *ghost, *pad;

	photo_bin = gst_element_factory_make ("bin", "photobin");
	photo_source = gst_element_factory_make ("appsrc", "photo-appsrc");
	photo_decoder = gst_element_factory_make ("jpegdec", "photo-decoder");
	photo_freeze = gst_element_factory_make ("imagefreeze", "photo-freeze");
	photo_scale = gst_element_factory_make ("videoscale", "photo-scale");

	photo_filter = gst_element_factory_make ("capsfilter", "photo-filter");
	caps = gst_caps_new_simple ("video/x-raw", "width", G_TYPE_INT, pb->monitor_geo.width, "height", G_TYPE_INT, pb->monitor_geo.height, "framerate", GST_TYPE_FRACTION, 1, 10, NULL);
	g_object_set (G_OBJECT (photo_filter), "caps", caps, NULL);
	gst_caps_unref (caps);

	if (!(photo_bin && photo_source && photo_decoder && photo_freeze && photo_scale && photo_filter))
	{
		GST_ERROR_OBJECT (photo_bin, "Failed to photobin pipeline element(s)");
		return FALSE;
	}

	gst_bin_add_many (GST_BIN (photo_bin), photo_source, photo_decoder, photo_freeze, photo_scale, photo_filter, NULL);

	if (!gst_element_link_many (photo_source, photo_decoder, photo_freeze, photo_scale, photo_filter, NULL))
	{
		GST_ERROR_OBJECT(photo_bin, "couldn't link photobin elements!");
		return FALSE;
	}

	pad = gst_element_get_static_pad (photo_filter, "src");
	ghost = gst_ghost_pad_new ("src", pad);
	gst_object_unref (pad);
	gst_pad_set_active (ghost, TRUE);
	gst_element_add_pad (photo_bin, ghost);
	return photo_bin;
}

static gboolean photo_booth_setup_gstreamer (PhotoBooth *pb)
{
	GstBus * bus;

	pb->video_bin = build_video_bin (pb);
	pb->photo_bin = build_photo_bin (pb);

	pb->pipeline = gst_pipeline_new ("photobooth-pipeline");

	pb->pixoverlay = gst_element_factory_make ("gdkpixbufoverlay", NULL);
	g_object_set (pb->pixoverlay, "location", "overlay_print.png", NULL);
	g_object_set (pb->pixoverlay, "overlay-width", pb->monitor_geo.width, NULL);
	g_object_set (pb->pixoverlay, "overlay-height", pb->monitor_geo.height, NULL);

	pb->video_sink = gst_element_factory_make ("xvimagesink", NULL);
// 	g_object_set (pb->video_sink, "sync", FALSE, NULL);

	if (!(pb->pixoverlay && pb->video_sink))
	{
		GST_ERROR_OBJECT (pb, "Failed to create pipeline element(s):%s%s", pb->pixoverlay?"":" gdkpixbufoverlay", pb->video_sink?"":" xvimagesink");
		return FALSE;
	}

	gst_bin_add_many (GST_BIN (pb->pipeline), pb->pixoverlay, pb->video_sink, NULL);

	if (!gst_element_link_many (pb->pixoverlay, pb->video_sink, NULL))
	{
		GST_ERROR_OBJECT(pb, "couldn't link elements!");
		return FALSE;
	}

	gst_element_set_state (pb->pipeline, GST_STATE_PLAYING);

	gst_bin_add_many (GST_BIN (pb->pipeline), pb->video_bin, pb->photo_bin, NULL);

	bus = gst_pipeline_get_bus (GST_PIPELINE (pb->pipeline));

	/* add watch for messages */
	gst_bus_add_watch (bus, (GstBusFunc) photo_booth_bus_callback, pb);
	gst_bus_set_sync_handler (bus, (GstBusSyncHandler) photo_booth_bus_sync_callback, pb->drawing_area, NULL);

	photo_booth_preview (pb);

	return TRUE;
}

static GstBusSyncReply photo_booth_bus_sync_callback (GstBus *bus, GstMessage *message, GtkWidget *drawing_area)
{
	if (GST_MESSAGE_TYPE (message) != GST_MESSAGE_ELEMENT)
		return GST_BUS_PASS;

	if (!gst_message_has_name (message, "prepare-window-handle"))
		return GST_BUS_PASS;

	/* FIXME: make sure to get XID in main thread */
	GST_DEBUG_OBJECT (drawing_area, "setting video overlay window handle");
	gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY (message->src), GDK_WINDOW_XID (gtk_widget_get_window (drawing_area)));

	gst_message_unref (message);
	return GST_BUS_DROP;
}

static gboolean photo_booth_bus_callback (GstBus *bus, GstMessage *message, PhotoBooth *pb)
{
	switch (GST_MESSAGE_TYPE (message))
	{
		case GST_MESSAGE_WARNING:
		{
			GError *err = NULL;
			gchar *debug = NULL;

			gst_message_parse_warning (message, &err, &debug);
			GST_WARNING ("Warning: %s\n", err->message);
			g_error_free (err);
			g_free (debug);
			break;
		}
		case GST_MESSAGE_ERROR:
		{
			GError *err = NULL;
			gchar *debug = NULL;

			gst_message_parse_error (message, &err, &debug);
			GST_ERROR ("Error: %s : %s", err->message, debug);
			g_error_free (err);
			g_free (debug);

			gtk_main_quit ();
			break;
		}
		case GST_MESSAGE_EOS:
		{
			GST_INFO ("EOS");
			gtk_main_quit ();
			break;
		}
		case GST_MESSAGE_STATE_CHANGED:
		{
			GstState old_state, new_state;
			gst_message_parse_state_changed (message, &old_state, &new_state, NULL);
			GstStateChange transition = (GstStateChange)GST_STATE_TRANSITION (old_state, new_state);
			GstElement *src = GST_ELEMENT (GST_MESSAGE_SRC (message));
			GST_DEBUG ("%" GST_PTR_FORMAT " state transition %s -> %s", src, gst_element_state_get_name(GST_STATE_TRANSITION_CURRENT(transition)), gst_element_state_get_name(GST_STATE_TRANSITION_NEXT(transition)));
			if (src == pb->video_bin && transition == GST_STATE_CHANGE_PAUSED_TO_PLAYING)
			{
				GST_DEBUG ("video_bin GST_STATE_CHANGE_READY_TO_PAUSED -> RUN CAPTURE!");
				SEND_COMMAND (pb, CONTROL_RUN);
			}
			if (src == pb->video_bin && transition == GST_STATE_CHANGE_PLAYING_TO_PAUSED)
			{
				GST_DEBUG ("video_bin GST_STATE_CHANGE_PLAYING_TO_PAUSED -> PAUSE CAPTURE!");
				SEND_COMMAND (pb, CONTROL_PAUSE);
			}
			break;
		}
		default:
		{
// 			GST_DEBUG ("gst_message from %" GST_PTR_FORMAT ": %" GST_PTR_FORMAT "", GST_MESSAGE_SRC(message), message);
		}
	}
	return TRUE;
}

extern int camera_auto_focus (Camera *list, GPContext *context, int onoff);

static gboolean photo_booth_focus (CameraInfo *cam_info)
{
	int gpret;
// 	CameraEventType evttype;
// 	void *evtdata;


// 	do {
// 		g_mutex_lock (&cam_info->mutex);
// 		gpret = gp_camera_wait_for_event (cam_info->camera, 10, &evttype, &evtdata, cam_info->context);
// 		g_mutex_unlock (&cam_info->mutex);
// 		GST_DEBUG ("gp_camera_wait_for_event gpret=%i", gpret);
// 	} while ((gpret == GP_OK) && (evttype != GP_EVENT_TIMEOUT));
// 
// 	g_mutex_lock (&cam_info->mutex);
// 	gpret = camera_auto_focus (cam_info->camera, cam_info->context, 1);
// 	g_mutex_unlock (&cam_info->mutex);
// 	if (gpret != GP_OK) {
// 		GST_WARNING ("gphoto error: %s\n", gp_result_as_string(gpret));
// 		return FALSE;
// 	}
// 
// 	do {
// 		GST_DEBUG ("gp_camera_wait_for_event gpret=%i", gpret);
// 		g_mutex_lock (&cam_info->mutex);
// 		gpret = gp_camera_wait_for_event (cam_info->camera, 10, &evttype, &evtdata, cam_info->context);
// 		g_mutex_unlock (&cam_info->mutex);
// 	} while ((gpret == GP_OK) && (evttype != GP_EVENT_TIMEOUT));
// 
	g_mutex_lock (&cam_info->mutex);
	gpret = camera_auto_focus (cam_info->camera, cam_info->context, 0);
	g_mutex_unlock (&cam_info->mutex);
	if (gpret != GP_OK) {
		GST_WARNING ("gphoto error: %s\n", gp_result_as_string(gpret));
	}
	return TRUE;
}

static gboolean photo_booth_take_photo (CameraInfo *cam_info, const char **ptr, unsigned long int *size)
{
	int gpret;
	CameraFile *file;
	CameraFilePath camera_file_path;

	g_mutex_lock (&cam_info->mutex);
	strcpy (camera_file_path.folder, "/");
	strcpy (camera_file_path.name, "foo.jpg");
// 	snprintf (camera_file_path.name, 128, "pb_capt_%04d", cam_info->preview_capture_count);
	gpret = gp_camera_capture (cam_info->camera, GP_CAPTURE_IMAGE, &camera_file_path, cam_info->context);
	GST_DEBUG ("gp_camera_capture gpret=%i Pathname on the camera: %s/%s", gpret, camera_file_path.folder, camera_file_path.name);
	if (gpret < 0)
		return FALSE;

	gpret = gp_file_new (&file);
	GST_DEBUG ("gp_file_new gpret=%i", gpret);

	gpret = gp_camera_file_get (cam_info->camera, camera_file_path.folder, camera_file_path.name, GP_FILE_TYPE_NORMAL, file, cam_info->context);
	GST_DEBUG ("gp_camera_file_get gpret=%i", gpret);
	if (gpret < 0)
		return FALSE;

	gp_file_get_data_and_size (file, ptr, size);
	if (gpret < 0)
		return FALSE;

	gpret = gp_camera_file_delete (cam_info->camera, camera_file_path.folder, camera_file_path.name, cam_info->context);
	GST_DEBUG ("gp_camera_file_delete gpret=%i", gpret);
// 	gp_file_free(file);
	g_mutex_unlock (&cam_info->mutex);

	return TRUE;
}

static void photo_booth_clicked (GtkWidget *button, GdkEventButton *event, PhotoBooth *pb)
{
	GST_DEBUG_OBJECT (pb, "photo_booth_clicked state=%d", pb->state);
	switch (pb->state) {
		case PB_STATE_PREVIEW:
			photo_booth_snapshot (pb);
			break;
		case PB_STATE_TAKING_PHOTO:
			GST_WARNING_OBJECT (pb, "BUSY TAKING A PHOTO, IGNORE CLICK");
			break;
		case PB_STATE_ASKING:
			photo_booth_preview (pb);
			break;
		default:
			break;
	}
}

static void photo_booth_preview (PhotoBooth *pb)
{
	GstPad *pad;
	if (pb->video_block_id)
	{
		GST_DEBUG_OBJECT (pb, "photo_booth_preview! halt photo_bin...");
		gst_element_set_state (pb->photo_bin, GST_STATE_PAUSED);
		pad = gst_element_get_static_pad (pb->photo_bin, "src");
		pb->photo_block_id = gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_DATA_DOWNSTREAM, _gst_photo_probecb, pb, NULL);
		gst_object_unref (pad);
		gst_element_unlink (pb->photo_bin, pb->pixoverlay);

		GST_DEBUG_OBJECT (pb, "photo_booth_preview! unblock video_bin...");
		pad = gst_element_get_static_pad (pb->video_bin, "src");
		gst_pad_remove_probe (pad, pb->video_block_id);
		gst_object_unref (pad);
	}
	int ret = gst_element_link (pb->video_bin, pb->pixoverlay);
	GST_DEBUG_OBJECT (pb, "linking %" GST_PTR_FORMAT " ! %" GST_PTR_FORMAT " ret=%i", pb->video_bin, pb->pixoverlay, ret);
	gst_element_set_state (pb->video_bin, GST_STATE_PLAYING);
// 	SEND_COMMAND (pb, CONTROL_RUN);
	GST_DEBUG_OBJECT (pb, "photo_booth_preview done");
	pb->state = PB_STATE_PREVIEW;
}

static void photo_booth_snapshot (PhotoBooth *pb)
{
	GstPad *pad;
	gboolean ret;

	pb->state = PB_STATE_TAKING_PHOTO;

	GST_INFO_OBJECT (pb, "SNAPSHOT!");

	if (!photo_booth_focus (pb->cam_info))
	{
		GST_WARNING_OBJECT (pb, "OUT OF FOCUS!");
		pb->state = PB_STATE_PREVIEW;
		return;
	}

	gst_element_set_state (pb->video_bin, GST_STATE_READY);
	GST_DEBUG_OBJECT (pb, "photo_booth_preview! halt video_bin...");
	pad = gst_element_get_static_pad (pb->video_bin, "src");
	pb->video_block_id = gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_DATA_DOWNSTREAM, _gst_video_probecb, pb, NULL);
	gst_object_unref (pad);
	gst_element_unlink (pb->video_bin, pb->pixoverlay);

	if (pb->photo_block_id)
	{
		GST_DEBUG_OBJECT (pb, "photo_booth_preview! unblock photo_bin...");
		pad = gst_element_get_static_pad (pb->photo_bin, "src");
		gst_pad_remove_probe (pad, pb->photo_block_id);
		gst_object_unref (pad);
	}

	ret = gst_element_link (pb->photo_bin, pb->pixoverlay);
	GST_DEBUG_OBJECT (pb, "linking %" GST_PTR_FORMAT " ! %" GST_PTR_FORMAT " ret=%i", pb->photo_bin, pb->pixoverlay, ret);
	gst_element_set_state (pb->photo_bin, GST_STATE_PLAYING);

	char	*data;
	unsigned long size;
	ret = photo_booth_take_photo (pb->cam_info, (const char**)&data, &size);
	if (ret)
	{
		GstElement *appsrc = gst_bin_get_by_name (GST_BIN (pb->photo_bin), "photo-appsrc");
		GstBuffer *buffer;
		GstFlowReturn flowret;

		buffer = gst_buffer_new_wrapped (data, size);
		g_signal_emit_by_name (appsrc, "push-buffer", buffer, &flowret);

		if (flowret != GST_FLOW_OK)
			GST_ERROR_OBJECT(appsrc, "couldn't push %" GST_PTR_FORMAT " to appsrc", buffer);
		gst_object_unref (appsrc);
		GST_INFO_OBJECT (pb, "photo_booth_snapshot now waiting for user input... PB_STATE_ASKING");
		pb->state = PB_STATE_ASKING;
		return;
	}
	GST_INFO_OBJECT (pb, "photo_booth_take_photo_push_to_appsrc failed!");
	gst_element_set_state (pb->pipeline, GST_STATE_NULL);
	g_main_loop_quit (pb->loop);
// 	photo_booth_preview (pb);
}

PhotoBooth *photo_booth_new (void)
{
	return g_object_new (PHOTO_BOOTH_TYPE,
			"application-id", "org.schaffenburg.photobooth",
			"flags", G_APPLICATION_HANDLES_OPEN,
			NULL);
}

G_DEFINE_TYPE (PhotoBoothWindow, photo_booth_window, GTK_TYPE_APPLICATION_WINDOW);

static void photo_booth_window_init (PhotoBoothWindow *win)
{
	gtk_window_set_title (GTK_WINDOW (win), "Live Video Preview");
}

static void photo_booth_window_class_init (PhotoBoothWindowClass *class)
{
}

PhotoBoothWindow * photo_booth_window_new (PhotoBooth *pb)
{
	g_print ("photo_booth_window_new\n");
	return g_object_new (PHOTO_BOOTH_WINDOW_TYPE, "photobooth", pb, NULL);
}

void photo_booth_window_open (PhotoBoothWindow *win, GFile *file)
{
	g_print ("photo_booth_window_open\n");
}

int main (int argc, char *argv[])
{
	PhotoBooth *pb;

	gst_init (&argc, &argv);

	pb = photo_booth_new ();
	pb->loop = g_main_loop_new (NULL, FALSE);

	g_unix_signal_add (SIGINT, (GSourceFunc) photo_booth_quit_signal, pb->loop);
	g_application_run (G_APPLICATION (pb), argc, argv);
	g_main_loop_run (pb->loop);

	g_object_unref (pb);
	return 0;

}

