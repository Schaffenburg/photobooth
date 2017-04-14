/*
 * photobooth.c
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
#include <gst/video/videooverlay.h>
#include <gst/video/gstvideosink.h>
#include <gst/app/app.h>
#include <curl/curl.h>
#include <X11/Xlib.h>
// #ifdef HAVE_LIBCANBERRA
#include <canberra-gtk.h>
// #endif
#include "photobooth.h"
#include "photoboothwin.h"
#include "photoboothled.h"

#define G_SETTINGS_ENABLE_BACKEND
#include <gio/gsettingsbackend.h>

#define photo_booth_parent_class parent_class

typedef struct _PhotoBoothPrivate PhotoBoothPrivate;

struct _PhotoBoothPrivate
{
	PhotoboothState    state;
	PhotoBoothWindow  *win;
	GstVideoRectangle  video_size;

	GThread           *capture_thread;
	gulong             video_block_id, photo_block_id, sink_block_id;
	gint               state_change_watchdog_timeout_id;

	guint32            countdown;
	gchar             *overlay_image;

	gchar             *save_path_template;
	guint              photos_taken, photos_printed;
	guint              save_filename_count;

	gchar             *printer_backend;
	gint               print_copies_min, print_copies_default, print_copies_max, print_copies;
	gint               print_dpi, print_width, print_height;
	gdouble            print_x_offset, print_y_offset;
	gchar             *print_icc_profile;
	gint               prints_remaining;
	GstBuffer         *print_buffer;
	GtkPrintSettings  *printer_settings;
	GMutex             processing_mutex;
	
	gint               preview_fps, preview_width, preview_height;
	gboolean           cam_reeinit_before_snapshot, cam_reeinit_after_snapshot;
	gchar             *cam_icc_profile;

	GstElement        *audio_pipeline;
	GstElement        *audio_playbin;

	GstElement        *screensaver_playbin;
	gboolean          paused_callback_id;

	gchar             *countdown_audio_uri;
	gchar             *error_sound;
	gchar             *ack_sound;

	gchar             *screensaver_uri;
	gint               screensaver_timeout;
	guint              screensaver_timeout_id;
	GstClockTime       last_play_pos;

	gint               upload_timeout;
	gchar             *facebook_put_uri;
	gchar             *imgur_album_id;
	gchar             *imgur_access_token;
	gchar             *imgur_description;
	GThread           *upload_thread;
	GMutex             upload_mutex;
	gboolean           do_twitter;

	PhotoBoothLed     *led;
};

#define MOVIEPIPE "moviepipe.mjpg"
#define DEFAULT_CONFIG "default.ini"
#define PREVIEW_FPS 24
#define DEFAULT_COUNTDOWN 5
#define DEFAULT_SAVE_PATH_TEMPLATE "./snapshot%03d.jpg"
#define DEFAULT_SCREENSAVER_TIMEOUT -1
#define PRINT_DPI 346
#define PRINT_WIDTH 2076
#define PRINT_HEIGHT 1384
#define PREVIEW_WIDTH 640
#define PREVIEW_HEIGHT 424
#define PT_PER_IN 72
#define IMGUR_UPLOAD_URI "https://api.imgur.com/3/upload"

typedef enum { NONE, ACK_SOUND, ERROR_SOUND } sound_t;

G_DEFINE_TYPE_WITH_PRIVATE (PhotoBooth, photo_booth, GTK_TYPE_APPLICATION);

GST_DEBUG_CATEGORY_STATIC (photo_booth_debug);
#define GST_CAT_DEFAULT photo_booth_debug

/* GObject / GApplication */
static void photo_booth_activate (GApplication *app);
static void photo_booth_open (GApplication  *app, GFile **files, gint n_files, const gchar *hint);
static void photo_booth_dispose (GObject *object);
static void photo_booth_finalize (GObject *object);
PhotoBooth *photo_booth_new (void);
void photo_booth_background_clicked (GtkWidget *widget, GdkEventButton *event, PhotoBoothWindow *win);
void photo_booth_button_cancel_clicked (GtkButton *button, PhotoBoothWindow *win);
void photo_booth_cancel (PhotoBooth *pb);

/* general private functions */
const gchar* photo_booth_state_get_name (PhotoboothState state);
static void photo_booth_change_state (PhotoBooth *pb, PhotoboothState state);
static void photo_booth_quit_signal (PhotoBooth *pb);
static void photo_booth_window_destroyed_signal (PhotoBoothWindow *win, PhotoBooth *pb);
static void photo_booth_setup_window (PhotoBooth *pb);
static void photo_booth_video_widget_ready (PhotoBooth *pb);
static gboolean photo_booth_preview (PhotoBooth *pb);
static gboolean photo_booth_preview_ready (PhotoBooth *pb);
static void photo_booth_snapshot_start (PhotoBooth *pb);
static gboolean photo_booth_snapshot_prepare (PhotoBooth *pb);
static gboolean photo_booth_snapshot_trigger (PhotoBooth *pb);
static gboolean photo_booth_snapshot_taken (PhotoBooth *pb);
static gboolean photo_booth_screensaver (PhotoBooth *pb);
static gboolean photo_booth_screensaver_stop (PhotoBooth *pb);
static gboolean photo_booth_watchdog_timedout (PhotoBooth *pb);
static gboolean photo_booth_capture_paused_cb (PhotoBooth *pb);

/* libgphoto2 */
static gboolean photo_booth_cam_init (CameraInfo **cam_info);
static gboolean photo_booth_cam_close (CameraInfo **cam_info);
static gboolean photo_booth_focus (CameraInfo *cam_info);
static gboolean photo_booth_take_photo (CameraInfo *cam_info);
static void photo_booth_flush_pipe (int fd);
static void photo_booth_capture_thread_func (PhotoBooth *pb);
static void _gphoto_err(GPLogLevel level, const char *domain, const char *str, void *data);

/* gstreamer functions */
static GstElement *build_video_bin (PhotoBooth *pb);
static GstElement *build_photo_bin (PhotoBooth *pb);
static gboolean photo_booth_setup_gstreamer (PhotoBooth *pb);
static gboolean photo_booth_bus_callback (GstBus *bus, GstMessage *message, PhotoBooth *pb);
static GstPadProbeReturn photo_booth_catch_photo_buffer (GstPad * pad, GstPadProbeInfo * info, gpointer user_data);
static gboolean photo_booth_process_photo_plug_elements (PhotoBooth *pb);
static GstFlowReturn photo_booth_catch_print_buffer (GstElement * appsink, gpointer user_data);
static gboolean photo_booth_process_photo_remove_elements (PhotoBooth *pb);
static void photo_booth_free_print_buffer (PhotoBooth *pb);
static GstPadProbeReturn photo_booth_screensaver_plug_continue (GstPad * pad, GstPadProbeInfo * info, gpointer user_data);
static GstPadProbeReturn photo_booth_screensaver_unplug_continue (GstPad * pad, GstPadProbeInfo * info, gpointer user_data);

/* printing functions */
static gboolean photo_booth_get_printer_status (PhotoBooth *pb);
void photo_booth_button_print_clicked (GtkButton *button, PhotoBoothWindow *win);
static void photo_booth_print (PhotoBooth *pb);
static void photo_booth_begin_print (GtkPrintOperation *operation, GtkPrintContext *context, gpointer user_data);
static void photo_booth_draw_page (GtkPrintOperation *operation, GtkPrintContext *context, int page_nr, gpointer user_data);
static void photo_booth_print_done (GtkPrintOperation *operation, GtkPrintOperationResult result, gpointer user_data);
static void photo_booth_printing_error_dialog (PhotoBoothWindow *window, GError *print_error);

/* upload functions */
void photo_booth_button_upload_clicked (GtkButton *button, PhotoBoothWindow *win);
static void photo_booth_post_thread_func (PhotoBooth *pb);
static gboolean photo_booth_upload_timedout (PhotoBooth *pb);

static void photo_booth_class_init (PhotoBoothClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
	GApplicationClass *gapplication_class = G_APPLICATION_CLASS (klass);

	GST_DEBUG_CATEGORY_INIT (photo_booth_debug, "photobooth", GST_DEBUG_BOLD | GST_DEBUG_FG_YELLOW | GST_DEBUG_BG_BLUE, "PhotoBooth");
	GST_DEBUG ("photo_booth_class_init");
	gp_log_add_func(GP_LOG_ERROR, _gphoto_err, NULL);

	gobject_class->finalize      = photo_booth_finalize;
	gobject_class->dispose       = photo_booth_dispose;
	gapplication_class->activate = photo_booth_activate;
	gapplication_class->open     = photo_booth_open;
}

static void photo_booth_init (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv;
	priv = photo_booth_get_instance_private (pb);

	GST_DEBUG_OBJECT (pb, "photo_booth_init init object!");

	int control_sock[2];
	if (socketpair (PF_UNIX, SOCK_STREAM, 0, control_sock) < 0)
	{
		GST_ERROR_OBJECT (pb, "cannot create control sockets: %s (%i)", strerror(errno), errno);
		g_application_quit (G_APPLICATION (pb));
	}
	READ_SOCKET (pb) = control_sock[0];
	WRITE_SOCKET (pb) = control_sock[1];
	fcntl (READ_SOCKET (pb), F_SETFL, O_NONBLOCK);
	fcntl (WRITE_SOCKET (pb), F_SETFL, O_NONBLOCK);

	pb->cam_info = NULL;

	pb->pipeline = NULL;
	priv->state = PB_STATE_NONE;
	priv->video_block_id = 0;
	priv->photo_block_id = 0;
	priv->sink_block_id = 0;

	if (mkfifo(MOVIEPIPE, 0666) == -1 && errno != EEXIST)
	{
		GST_ERROR_OBJECT (pb, "cannot create moviepipe file %s: %s (%i)", MOVIEPIPE, strerror(errno), errno);
		g_application_quit (G_APPLICATION (pb));
	}

	pb->video_fd = open(MOVIEPIPE, O_RDWR);
	if (pb->video_fd == -1)
	{
		GST_ERROR_OBJECT (pb, "cannot open moviepipe file %s: %s (%i)", MOVIEPIPE, strerror(errno), errno);
		g_application_quit (G_APPLICATION (pb));
	}

	priv->capture_thread = NULL;
	priv->countdown = DEFAULT_COUNTDOWN;
	priv->preview_fps = PREVIEW_FPS;
	priv->preview_width = PREVIEW_WIDTH;
	priv->preview_height = PREVIEW_HEIGHT;
	priv->print_copies_min = priv->print_copies_max = priv->print_copies_default = 1;
	priv->print_copies = 1;
	priv->print_dpi = PRINT_DPI;
	priv->print_width = PRINT_WIDTH;
	priv->print_height = PRINT_HEIGHT;
	priv->print_x_offset = priv->print_y_offset = 0;
	priv->print_buffer = NULL;
	priv->print_icc_profile = NULL;
	priv->cam_icc_profile = NULL;
	priv->printer_backend = NULL;
	priv->printer_settings = NULL;
	priv->overlay_image = NULL;
	priv->countdown_audio_uri = NULL;
	priv->ack_sound = NULL;
	priv->error_sound = NULL;
	priv->screensaver_uri = NULL;
	priv->screensaver_timeout = DEFAULT_SCREENSAVER_TIMEOUT;
	priv->screensaver_timeout_id = 0;
	priv->paused_callback_id = 0;
	priv->last_play_pos = GST_CLOCK_TIME_NONE;
	priv->save_path_template = g_strdup (DEFAULT_SAVE_PATH_TEMPLATE);
	priv->photos_taken = priv->photos_printed = 0;
	priv->save_filename_count = 0;
	priv->upload_timeout = 0;
	priv->facebook_put_uri = NULL;
	priv->imgur_album_id = NULL;
	priv->imgur_access_token = NULL;
	priv->imgur_description = NULL;
	priv->upload_thread = NULL;
	priv->do_twitter = TRUE;
	priv->state_change_watchdog_timeout_id = 0;

	priv->led = photo_booth_led_new ();

	G_stylesheet_filename = NULL;
	G_template_filename = NULL;

	G_strings_table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
	g_mutex_init (&priv->processing_mutex);
	g_mutex_init (&priv->upload_mutex);
}

static void photo_booth_change_state (PhotoBooth *pb, PhotoboothState newstate)
{
	PhotoBoothPrivate *priv;
	priv = photo_booth_get_instance_private (pb);
	GST_DEBUG_OBJECT (pb, "change state %s -> %s", photo_booth_state_get_name (priv->state), photo_booth_state_get_name (newstate));
	gchar *dot_filename = g_strdup_printf ("state_change_%s_to_%s", photo_booth_state_get_name (priv->state), photo_booth_state_get_name (newstate));
	GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pb->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, dot_filename);
	g_free (dot_filename);
	priv->state = newstate;
	if (priv->state_change_watchdog_timeout_id)
	{
		GST_LOG_OBJECT (pb, "removed watchdog timeout");
		g_source_remove (priv->state_change_watchdog_timeout_id);
		priv->state_change_watchdog_timeout_id = 0;
	}
}

static void photo_booth_setup_window (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv;
	priv = photo_booth_get_instance_private (pb);
	priv->win = photo_booth_window_new (pb);
	gtk_window_present (GTK_WINDOW (priv->win));
	g_signal_connect (G_OBJECT (priv->win), "destroy", G_CALLBACK (photo_booth_window_destroyed_signal), pb);
	priv->capture_thread = g_thread_try_new ("gphoto-capture", (GThreadFunc) photo_booth_capture_thread_func, pb, NULL);
	photo_booth_setup_gstreamer (pb);
	photo_booth_get_printer_status (pb);
}

static void photo_booth_activate (GApplication *app)
{
	GST_DEBUG_OBJECT (app, "photo_booth_activate");
	photo_booth_setup_window (PHOTO_BOOTH (app));
}

static void photo_booth_open (GApplication *app, GFile **files, gint n_files, const gchar *hint)
{
	GST_DEBUG_OBJECT (app, "photo_booth_open");
	photo_booth_setup_window (PHOTO_BOOTH (app));
}

static void photo_booth_finalize (GObject *object)
{
	PhotoBooth *pb = PHOTO_BOOTH (object);
	PhotoBoothPrivate *priv = photo_booth_get_instance_private (pb);

	GST_INFO_OBJECT (pb, "finalize");
	SEND_COMMAND (pb, CONTROL_QUIT);
	photo_booth_flush_pipe (pb->video_fd);
	g_thread_join (priv->capture_thread);
	if (pb->cam_info)
		photo_booth_cam_close (&pb->cam_info);
	if (pb->video_fd)
	{
		close (pb->video_fd);
		unlink (MOVIEPIPE);
	}
	if (priv->upload_thread)
		g_thread_join (priv->upload_thread);
	g_object_unref (priv->led);
}

static void photo_booth_dispose (GObject *object)
{
	PhotoBoothPrivate *priv;
	priv = photo_booth_get_instance_private (PHOTO_BOOTH (object));
	g_free (priv->printer_backend);
	if (priv->printer_settings != NULL)
		g_object_unref (priv->printer_settings);
	g_free (priv->countdown_audio_uri);
	g_free (priv->ack_sound);
	g_free (priv->error_sound);
	g_free (priv->screensaver_uri);
	g_free (priv->print_icc_profile);
	g_free (priv->cam_icc_profile);
	g_free (priv->overlay_image);
	g_free (priv->save_path_template);
	g_free (priv->facebook_put_uri);
	g_free (priv->imgur_album_id);
	g_free (priv->imgur_access_token);
	g_free (priv->imgur_description);
	g_hash_table_destroy (G_strings_table);
	G_strings_table = NULL;
	g_mutex_clear (&priv->processing_mutex);
	g_mutex_clear (&priv->upload_mutex);
	G_OBJECT_CLASS (photo_booth_parent_class)->dispose (object);
	g_free (G_stylesheet_filename);
	g_free (G_template_filename);
}

void photo_booth_load_settings (PhotoBooth *pb, const gchar *filename)
{
	GKeyFile* gkf;
	GError *error = NULL;
	guint keyidx;
	gsize num_keys;
	gchar **keys, *val;
	gchar *key;
	gchar *value;
	PhotoBoothPrivate *priv = photo_booth_get_instance_private (pb);
	
	GST_DEBUG_OBJECT (pb, "loading settings from file %s", filename);

	gkf = g_key_file_new();

	if (g_key_file_load_from_file (gkf, filename, G_KEY_FILE_NONE, &error))
	{
		if (g_key_file_has_group (gkf, "strings"))
		{
			keys = g_key_file_get_keys (gkf, "strings", &num_keys, &error);
			for (keyidx = 0; keyidx < num_keys; keyidx++)
			{
				val = g_key_file_get_value(gkf, "strings", keys[keyidx], &error);
				key = g_strdup(keys[keyidx]);
				value = g_strdup(val);
				g_hash_table_insert (G_strings_table, key, value);
				GST_TRACE ("key %u/%u:\t'%s' => '%s'", keyidx, num_keys-1, key, value);
				g_free (val);
			}
			if (error)
			{
				GST_INFO ( "can't read string: %s", error->message);
				g_error_free (error);
			}
			g_strfreev (keys);
		}
		if (g_key_file_has_group (gkf, "general"))
		{
			G_template_filename = g_key_file_get_string (gkf, "general", "template", NULL);
			G_stylesheet_filename = g_key_file_get_string (gkf, "general", "stylesheet", NULL);
			priv->countdown = g_key_file_get_integer (gkf, "general", "countdown", NULL);
			priv->overlay_image = g_key_file_get_string (gkf, "general", "overlay_image", NULL);
			priv->screensaver_timeout = g_key_file_get_integer (gkf, "general", "screensaver_timeout", NULL);
			gchar *screensaverfile = g_key_file_get_string (gkf, "general", "screensaver_file", NULL);
			if (screensaverfile)
			{
				gchar *screensaverabsfilename;
				if (!g_path_is_absolute (screensaverfile))
				{
					gchar *cdir = g_get_current_dir ();
					screensaverabsfilename = g_strdup_printf ("%s/%s", cdir, screensaverfile);
					g_free (cdir);
				}
				else
					screensaverabsfilename = g_strdup (screensaverfile);
				priv->screensaver_uri = g_filename_to_uri (screensaverabsfilename, NULL, NULL);
				g_free (screensaverfile);
				g_free (screensaverabsfilename);
			}
			gchar *save_path_template = g_key_file_get_string (gkf, "general", "save_path_template", NULL);
			if (save_path_template)
			{
				gchar *cdir;
				if (!g_path_is_absolute (save_path_template))
				{
					cdir = g_get_current_dir ();
					priv->save_path_template = g_strdup_printf ("%s/%s", cdir, save_path_template);
					g_free (cdir);
				}
				else
				{
					cdir = g_path_get_dirname (save_path_template);
					priv->save_path_template = g_strdup (save_path_template);
				}
				g_free (save_path_template);
			}
		}
		if (g_key_file_has_group (gkf, "sounds"))
		{
			gchar *countdownaudiofile = g_key_file_get_string (gkf, "sounds", "countdown_audio_file", NULL);
			if (countdownaudiofile)
			{
				gchar *audioabsfilename;
				if (!g_path_is_absolute (countdownaudiofile))
				{
					gchar *cdir = g_get_current_dir ();
					audioabsfilename = g_strdup_printf ("%s/%s", cdir, countdownaudiofile);
					g_free (cdir);
				}
				else
					audioabsfilename = g_strdup (countdownaudiofile);
				priv->countdown_audio_uri = g_filename_to_uri (audioabsfilename, NULL, NULL);
				g_free (countdownaudiofile);
				g_free (audioabsfilename);
			}
			priv->ack_sound = g_key_file_get_string (gkf, "sounds", "ack_sound", NULL);
			priv->error_sound = g_key_file_get_string (gkf, "sounds", "error_sound", NULL);
		}
		if (g_key_file_has_group (gkf, "printer"))
		{
			priv->printer_backend = g_key_file_get_string (gkf, "printer", "backend", NULL);
			priv->print_copies_min = g_key_file_get_integer (gkf, "printer", "copies_min", NULL);
			priv->print_copies_max = g_key_file_get_integer (gkf, "printer", "copies_max", NULL);
			priv->print_copies_default = g_key_file_get_integer (gkf, "printer", "copies_default", NULL);
			priv->print_dpi = g_key_file_get_integer (gkf, "printer", "dpi", NULL);
			priv->print_width = g_key_file_get_integer (gkf, "printer", "width", NULL);
			priv->print_height = g_key_file_get_integer (gkf, "printer", "height", NULL);
			priv->print_icc_profile = g_key_file_get_string (gkf, "printer", "icc_profile", NULL);
			priv->print_x_offset = g_key_file_get_double (gkf, "printer", "offset_x", NULL);
			priv->print_y_offset = g_key_file_get_double (gkf, "printer", "offset_y", NULL);
		}
		if (g_key_file_has_group (gkf, "camera"))
		{
			priv->preview_fps = g_key_file_get_integer (gkf, "camera", "fps", NULL);
			priv->preview_width = g_key_file_get_integer (gkf, "camera", "preview_width", NULL);
			priv->preview_height = g_key_file_get_integer (gkf, "camera", "preview_height", NULL);
			priv->cam_reeinit_before_snapshot = g_key_file_get_boolean (gkf, "camera", "cam_reeinit_before_snapshot", NULL);
			priv->cam_reeinit_after_snapshot = g_key_file_get_boolean (gkf, "camera", "cam_reeinit_after_snapshot", NULL);
			priv->cam_icc_profile = g_key_file_get_string (gkf, "camera", "icc_profile", NULL);
		}
		if (g_key_file_has_group (gkf, "upload"))
		{
			priv->facebook_put_uri = g_key_file_get_string (gkf, "upload", "facebook_put_uri", NULL);
			priv->imgur_album_id = g_key_file_get_string (gkf, "upload", "imgur_album_id", NULL);
			priv->imgur_access_token = g_key_file_get_string (gkf, "upload", "imgur_access_token", NULL);
			priv->imgur_description = g_key_file_get_string (gkf, "upload", "imgur_description", NULL);
			priv->upload_timeout = g_key_file_get_integer (gkf, "upload", "upload_timeout", NULL);
		}
	}

	gchar *save_path_basename = g_path_get_basename (priv->save_path_template);
	gchar *pos = g_strstr_len ((const gchar*) save_path_basename, strlen (save_path_basename), "%");
	if (pos)
	{
		gchar *filenameprefix = g_strndup (save_path_basename, pos-save_path_basename);
		GDir *save_dir;
		GError *error = NULL;
		gchar *cdir = g_path_get_dirname (priv->save_path_template);
		save_dir = g_dir_open ((const gchar*)cdir, 0, &error);
		if (error) {
			GST_WARNING ("couldn't open save directory '%s': %s", priv->save_path_template, error->message);
		}
		else if (save_dir)
		{
			const gchar *filename;
			GMatchInfo *match_info;
			GRegex *regex;
			gchar *pattern = g_strdup_printf("(?<filename>%s)(?<number>\\d+)", filenameprefix);
			GST_TRACE ("save_path_basename regex pattern = '%s'", pattern);
			regex = g_regex_new (pattern, 0, 0, &error);
			if (error) {
				g_critical ("%s\n", error->message);
			}
			while ((filename = g_dir_read_name (save_dir)))
			{
				if (g_regex_match (regex, filename, 0, &match_info))
				{
					gint count = atoi(g_match_info_fetch_named (match_info, "number"));
					gchar *name = g_match_info_fetch_named (match_info, "filename");
					if (count > priv->save_filename_count)
						priv->save_filename_count = count;
					GST_TRACE ("save_path_template found matching file %s (prefix %s, count %d, highest %i)", filename, name, count, priv->save_filename_count);
					g_free (name);
				}
				else
					GST_TRACE ("save_path_template unmatched file %s", filename);
			}
			g_dir_close (save_dir);
		}
	}
	g_free (save_path_basename);

	g_key_file_free (gkf);
	if (error)
	{
		GST_INFO ( "can't open settings file %s: %s", filename, error->message);
		g_error_free (error);
	}
}

static void _gphoto_err(GPLogLevel level, const char *domain, const char *str, void *data)
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

void _play_event_sound (PhotoBoothPrivate *priv, sound_t sound)
{
	gchar *soundfile = NULL;
	switch (sound) {
		case ACK_SOUND:
			soundfile = priv->ack_sound;
			break;
		case ERROR_SOUND:
			soundfile = priv->error_sound;
			break;
		default:
			break;
	}
	if (soundfile)
		ca_context_play (ca_gtk_context_get(), 0, CA_PROP_MEDIA_FILENAME, soundfile, NULL);
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
	(*cam_info)->size = 0;
	(*cam_info)->data = NULL;
	(*cam_info)->context = gp_context_new();
	gp_camera_new(&(*cam_info)->camera);
	retval = gp_camera_init((*cam_info)->camera, (*cam_info)->context);
	GST_DEBUG ("gp_camera_init returned %d cam_info@%p camera@%p", retval, *cam_info, (*cam_info)->camera);
	g_mutex_unlock (&(*cam_info)->mutex);
	if (retval == GP_ERROR_IO_USB_CLAIM)
	{
		g_usleep (G_USEC_PER_SEC);
	}
	if (retval != GP_OK) {
		photo_booth_cam_close (&(*cam_info));
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
	gp_camera_free ((*cam_info)->camera);
	gp_context_unref ((*cam_info)->context);
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
	const int flags = fcntl(fd, F_GETFL, 0);
	fcntl (fd, F_SETFL, flags | O_NONBLOCK);
	while (rlen != -1)
	{
		rlen = read (fd, buf, sizeof(buf));
	}
	fcntl (fd, F_SETFL, flags ^ O_NONBLOCK);
}

static void photo_booth_quit_signal (PhotoBooth *pb)
{
	GST_INFO_OBJECT (pb, "caught SIGINT! exit...");
	g_application_quit (G_APPLICATION (pb));
}

static void photo_booth_window_destroyed_signal (PhotoBoothWindow *win, PhotoBooth *pb)
{
	GST_INFO_OBJECT (pb, "main window closed! exit...");
	g_application_quit (G_APPLICATION (pb));
}

static void photo_booth_capture_thread_func (PhotoBooth *pb)
{
	PhotoboothCaptureThreadState state = CAPTURE_INIT;
	PhotoBoothPrivate *priv = photo_booth_get_instance_private (pb);
	CameraFile *gp_file = NULL;
	int gpret, captured_frames = 0;

	GST_DEBUG_OBJECT (pb, "enter capture thread fd = %d", pb->video_fd);

	if (gp_file_new_from_fd (&gp_file, pb->video_fd) != GP_OK)
	{
		GST_ERROR_OBJECT (pb, "couldn't start capture thread because gp_file_new_from_fd (%d) failed!", pb->video_fd);
		goto quit_thread;
	}

	while (TRUE) {
		if (state == CAPTURE_QUIT)
			goto quit_thread;

		struct pollfd rfd[2];
		int timeout = 0;
		rfd[0].fd = READ_SOCKET (pb);
		rfd[0].events = POLLIN | POLLERR | POLLHUP | POLLPRI;

		if (state == CAPTURE_INIT || state == CAPTURE_FAILED && !pb->cam_info)
		{
			if (photo_booth_cam_init (&pb->cam_info))
			{
				GST_INFO_OBJECT (pb, "photo_booth_cam_inited @ %p", pb->cam_info);
				if (state == CAPTURE_FAILED)
				{
					photo_booth_window_set_spinner (priv->win, FALSE);
				}
				//photo_booth_focus (pb->cam_info);
				state = CAPTURE_VIDEO;
				g_main_context_invoke (NULL, (GSourceFunc) photo_booth_preview, pb);
			}
			else {
				gtk_label_set_text (priv->win->status, _("No camera connected!"));
				GST_INFO_OBJECT (pb, "no camera info.");
			}
			timeout = 5000;
		}
		else if (state == CAPTURE_PAUSED)
			timeout = 1000;
		else
			timeout = 1000 / PREVIEW_FPS;

		int ret = poll(rfd, 1, timeout);

		GST_TRACE_OBJECT (pb, "poll ret=%i, state=%i, cam_info@%p", ret, state, pb->cam_info);

		if (G_UNLIKELY (ret == -1))
		{
			GST_ERROR_OBJECT (pb, "SELECT ERROR!");
			goto quit_thread;
		}
		else if (ret == 0 && state == CAPTURE_VIDEO)
		{
			const char *mime;
			if (pb->cam_info)
			{
				g_mutex_lock (&pb->cam_info->mutex);
				gpret = gp_camera_capture_preview (pb->cam_info->camera, gp_file, pb->cam_info->context);
				g_mutex_unlock (&pb->cam_info->mutex);
				if (gpret < 0) {
					GST_ERROR_OBJECT (pb, "Movie capture error %d", gpret);
					if (gpret == -7)
					{
						state = CAPTURE_FAILED;
						photo_booth_change_state (pb, PB_STATE_NONE);
						photo_booth_cam_close (&pb->cam_info);
					}
					continue;
				}
				else {
					gp_file_get_mime_type (gp_file, &mime);
					if (strcmp (mime, GP_MIME_JPEG)) {
						GST_ERROR_OBJECT ("Movie capture error... Unhandled MIME type '%s'.", mime);
						continue;
					}
					captured_frames++;
					GST_LOG_OBJECT (pb, "captured frame (%d frames total)", captured_frames);
				}
			}
		}
		else if (ret == 0 && state == CAPTURE_PRETRIGGER)
		{
			gtk_label_set_text (priv->win->status, _("Focussing..."));
// 			photo_booth_focus (pb->cam_info);
			if (priv->cam_reeinit_before_snapshot)
			{
				photo_booth_cam_close (&pb->cam_info);
				photo_booth_cam_init (&pb->cam_info);
			}
		}
		else if (ret == 0 && state == CAPTURE_PHOTO)
		{
			if (pb->cam_info)
			{
				gtk_label_set_text (priv->win->status, _("Taking photo..."));
				photo_booth_led_flash (priv->led);
				ret = photo_booth_take_photo (pb->cam_info);
				photo_booth_led_black (priv->led);
				if (ret && pb->cam_info->size)
				{
					g_main_context_invoke (NULL, (GSourceFunc) photo_booth_snapshot_taken, pb);
					state = CAPTURE_PAUSED;
				}
				else {
					gtk_label_set_text (priv->win->status, _("Taking photo failed!"));
					_play_event_sound (priv, ERROR_SOUND);
					GST_ERROR_OBJECT (pb, "Taking photo failed!");
					photo_booth_cam_close (&pb->cam_info);
					photo_booth_change_state (pb, PB_STATE_NONE);
					state = CAPTURE_FAILED;
				}
			}
		}
		else if (rfd[0].revents)
		{
			char command;
			READ_COMMAND (pb, command, ret);
			switch (command) {
				case CONTROL_PAUSE:
					GST_DEBUG_OBJECT (pb, "CONTROL_PAUSE!");
					state = CAPTURE_PAUSED;
					break;
				case CONTROL_UNPAUSE:
					GST_DEBUG_OBJECT (pb, "CONTROL_UNPAUSE!");
					state = CAPTURE_INIT;
					break;
				case CONTROL_VIDEO:
					GST_DEBUG_OBJECT (pb, "CONTROL_VIDEO");
					state = CAPTURE_VIDEO;
					break;
				case CONTROL_PRETRIGGER:
					GST_DEBUG_OBJECT (pb, "CONTROL_PRETRIGGER");
					state = CAPTURE_PRETRIGGER;
					break;
				case CONTROL_PHOTO:
					GST_DEBUG_OBJECT (pb, "CONTROL_PHOTO");
					state = CAPTURE_PHOTO;
					break;
				case CONTROL_QUIT:
					GST_DEBUG_OBJECT (pb, "CONTROL_QUIT!");
					state = CAPTURE_QUIT;
					break;
				case CONTROL_REINIT:
				{
					GST_DEBUG_OBJECT (pb, "CONTROL_REINIT!");
					photo_booth_cam_close (&pb->cam_info);
					photo_booth_cam_init (&pb->cam_info);
					break;
				}
				default:
					GST_ERROR_OBJECT (pb, "illegal control socket command %c received!", command);
			}
			continue;
		}
		else if (state == CAPTURE_PAUSED)
		{
			if (pb->cam_info)
			{
				GST_LOG_OBJECT (pb, "captured thread paused... close camera! %s", photo_booth_state_get_name (priv->state));
				photo_booth_cam_close (&pb->cam_info);
// 				photo_booth_flush_pipe (pb->video_fd);
			}
			else
				GST_LOG_OBJECT (pb, "captured thread paused... timeout. %s", photo_booth_state_get_name (priv->state));
			if (priv->paused_callback_id)
			{
				priv->paused_callback_id = 0;
				g_main_context_invoke (NULL, (GSourceFunc) photo_booth_capture_paused_cb, pb);
			}
		}
	}

	g_assert_not_reached ();
	return;

	quit_thread:
	{
		if (gp_file)
			gp_file_unref (gp_file);
		GST_DEBUG ("stop running, exit thread, %d frames captured", captured_frames);
		return;
	}
}

static GstElement *build_video_bin (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv;
	GstElement *video_bin;
	GstElement *mjpeg_source, *mjpeg_decoder, *mjpeg_filter, *video_filter, *video_scale, *video_convert, *video_overlay;
	GstCaps *caps;
	GstPad *ghost, *pad;

	priv = photo_booth_get_instance_private (pb);

	video_bin = gst_element_factory_make ("bin", "video-bin");
	mjpeg_source = gst_element_factory_make ("fdsrc", "mjpeg-fdsrc");
	g_object_set (mjpeg_source, "fd", pb->video_fd, NULL);
	g_object_set (mjpeg_source, "do-timestamp", TRUE, NULL);
	g_object_set (mjpeg_source, "blocksize", 65536, NULL);

	mjpeg_filter = gst_element_factory_make ("capsfilter", "mjpeg-capsfilter");
	caps = gst_caps_new_simple ("image/jpeg", "width", G_TYPE_INT, priv->preview_width, "height", G_TYPE_INT, priv->preview_height, "framerate", GST_TYPE_FRACTION, PREVIEW_FPS, 1, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, NULL);
	g_object_set (G_OBJECT (mjpeg_filter), "caps", caps, NULL);
	gst_caps_unref (caps);

	mjpeg_decoder = gst_element_factory_make ("jpegdec", "mjpeg-decoder");
	video_scale = gst_element_factory_make ("videoscale", "mjpeg-videoscale");
	video_convert = gst_element_factory_make ("videoconvert", "mjpeg-videoconvert");
	video_filter = gst_element_factory_make ("capsfilter", "video-capsfilter");
	caps = gst_caps_new_simple ("video/x-raw", "width", G_TYPE_INT, priv->preview_width, "height", G_TYPE_INT, priv->preview_height, NULL);
	g_object_set (G_OBJECT (video_filter), "caps", caps, NULL);
	gst_caps_unref (caps);

	video_overlay = gst_element_factory_make ("gdkpixbufoverlay", "video-overlay");
	if (priv->overlay_image)
		g_object_set (video_overlay, "location", priv->overlay_image, NULL);

	if (!(mjpeg_source && mjpeg_filter && mjpeg_decoder && video_scale && video_convert && video_filter && video_overlay))
	{
		GST_ERROR_OBJECT (video_bin, "Failed to make videobin pipeline element(s):%s%s%s%s%s%s%s", mjpeg_source?"":" fdsrc", mjpeg_filter?"":" capsfilter", mjpeg_decoder?"":" jpegdec",
			video_scale?"":" videoscale", video_convert?"":" videoconvert", video_filter?"":" capsfilter", video_overlay?"":" gdkpixbufoverlay");
		return FALSE;
	}

	gst_bin_add_many (GST_BIN (video_bin), mjpeg_source, mjpeg_filter, mjpeg_decoder, video_scale, video_convert, video_filter, video_overlay, NULL);

	if (!gst_element_link_many (mjpeg_source, mjpeg_filter, mjpeg_decoder, video_scale, video_convert, video_filter, video_overlay, NULL))
	{
		GST_ERROR_OBJECT (video_bin, "couldn't link videobin elements!");
		return FALSE;
	}

	pad = gst_element_get_static_pad (video_overlay, "src");
	ghost = gst_ghost_pad_new ("src", pad);
	gst_object_unref (pad);
	gst_pad_set_active (ghost, TRUE);
	gst_element_add_pad (video_bin, ghost);
	return video_bin;
}

static GstElement *build_photo_bin (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv;
	GstElement *photo_bin;
	GstElement *photo_source, *photo_decoder, *photo_freeze, *photo_scale, *photo_filter, *photo_overlay, *photo_convert, *photo_gamma, *photo_tee;
	GstCaps *caps;
	GstPad *ghost, *pad;

	priv = photo_booth_get_instance_private (pb);

	photo_bin = gst_element_factory_make ("bin", "photo-bin");
	photo_source = gst_element_factory_make ("appsrc", "photo-appsrc");
	photo_decoder = gst_element_factory_make ("jpegdec", "photo-decoder");
	photo_freeze = gst_element_factory_make ("imagefreeze", "photo-freeze");
	photo_scale = gst_element_factory_make ("videoscale", "photo-scale");

	photo_filter = gst_element_factory_make ("capsfilter", "photo-capsfilter");
	caps = gst_caps_new_simple ("video/x-raw", "width", G_TYPE_INT, priv->print_width, "height", G_TYPE_INT, priv->print_height, "framerate", GST_TYPE_FRACTION, 10, 1, NULL);
	g_object_set (G_OBJECT (photo_filter), "caps", caps, NULL);
	gst_caps_unref (caps);

	photo_overlay = gst_element_factory_make ("gdkpixbufoverlay", "photo-overlay");
	if (priv->overlay_image)		
		g_object_set (photo_overlay, "location", priv->overlay_image, NULL);
	g_object_set (photo_overlay, "overlay-width", priv->print_width, NULL);
	g_object_set (photo_overlay, "overlay-height", priv->print_height, NULL);

	photo_convert = gst_element_factory_make ("videoconvert", "photo-convert");
	photo_gamma = gst_element_factory_make ("gamma", "photo-gamma");
	g_object_set (photo_gamma, "gamma", 1.0, NULL);
	photo_tee = gst_element_factory_make ("tee", "photo-tee");

	if (!(photo_bin && photo_source && photo_decoder && photo_freeze && photo_scale && photo_filter, photo_overlay && photo_convert && photo_tee))
	{
		GST_ERROR_OBJECT (photo_bin, "Failed to make photobin pipeline element(s)");
		return FALSE;
	}

	gst_bin_add_many (GST_BIN (photo_bin), photo_source, photo_decoder, photo_freeze, photo_scale, photo_filter, photo_overlay, photo_convert, photo_gamma, photo_tee, NULL);

	if (!gst_element_link_many (photo_source, photo_decoder, photo_freeze, photo_scale, photo_filter, photo_overlay, photo_convert, photo_gamma, photo_tee, NULL))
	{
		GST_ERROR_OBJECT (photo_bin, "couldn't link photobin elements!");
		return FALSE;
	}

	pad = gst_element_get_request_pad (photo_tee, "src_%u");
	ghost = gst_ghost_pad_new ("src", pad);
	gst_object_unref (pad);
	gst_pad_set_active (ghost, TRUE);
	gst_element_add_pad (photo_bin, ghost);
	return photo_bin;
}

static gboolean photo_booth_setup_gstreamer (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv;
	GstBus *bus;
	GtkWidget *gtkgstwidget;
	GstPad *ghost, *pad;

	priv = photo_booth_get_instance_private (pb);

	pb->video_bin  = build_video_bin (pb);
	pb->photo_bin  = build_photo_bin (pb);

	pb->pipeline = gst_pipeline_new ("photobooth-pipeline");

	pb->video_sink = gst_element_factory_make ("gtksink", "video-sink");
// 	g_object_set (pb->video_sink, "sync", FALSE, NULL);

	if (!(pb->video_sink))
	{
		GST_ERROR_OBJECT (pb, "Failed to create gtksink");
		return FALSE;
	}

	g_object_get (pb->video_sink, "widget", &gtkgstwidget, NULL);
	photo_booth_window_add_gtkgstwidget (priv->win, gtkgstwidget);
	g_object_unref (gtkgstwidget);

	gst_element_set_state (pb->pipeline, GST_STATE_PLAYING);
	gst_element_set_state (pb->video_sink, GST_STATE_PLAYING);

	gst_bin_add_many (GST_BIN (pb->pipeline), pb->video_bin, pb->photo_bin, pb->video_sink, NULL);

	/* add watch for messages */
	bus = gst_pipeline_get_bus (GST_PIPELINE (pb->pipeline));
	gst_bus_add_watch (bus, (GstBusFunc) photo_booth_bus_callback, pb);
	gst_object_unref (GST_OBJECT (bus));

	priv->audio_pipeline = gst_pipeline_new ("audio-pipeline");
	priv->audio_playbin = gst_element_factory_make ("playbin", "audio-playbin");
	gst_bin_add (GST_BIN (priv->audio_pipeline), priv->audio_playbin);

	return TRUE;
}

static gboolean photo_booth_bus_callback (GstBus *bus, GstMessage *message, PhotoBooth *pb)
{
	GstObject *src = GST_MESSAGE_SRC (message);
	PhotoBoothPrivate *priv;
	priv = photo_booth_get_instance_private (pb);
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
			if (src == GST_OBJECT (priv->screensaver_playbin))
			{
				GST_DEBUG ("screensaver EOS, replay");
				gst_element_seek (priv->screensaver_playbin, 1.0, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, GST_SEEK_TYPE_SET, 0, GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
			}
			break;
		}
		case GST_MESSAGE_STATE_CHANGED:
		{
			GstState old_state, new_state;
			gst_message_parse_state_changed (message, &old_state, &new_state, NULL);
			GstStateChange transition = (GstStateChange)GST_STATE_TRANSITION (old_state, new_state);
			GST_LOG ("gst %" GST_PTR_FORMAT " state transition %s -> %s. %s", src, gst_element_state_get_name(GST_STATE_TRANSITION_CURRENT(transition)), gst_element_state_get_name(GST_STATE_TRANSITION_NEXT(transition)), photo_booth_state_get_name (priv->state));
			if (src == GST_OBJECT (pb->video_sink) && transition == GST_STATE_CHANGE_READY_TO_PAUSED)
			{
				photo_booth_video_widget_ready (pb);
			}
			if (src == GST_OBJECT (pb->video_sink) && transition == GST_STATE_CHANGE_PAUSED_TO_PLAYING)
			{
				GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pb->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "photo_booth_video_start");
				GST_DEBUG ("video_bin GST_STATE_CHANGE_PAUSED_TO_PLAYING -> hide spinner!");
				photo_booth_window_hide_cursor (priv->win);
				photo_booth_window_set_spinner (priv->win, FALSE);
			}
			if (src == GST_OBJECT (priv->screensaver_playbin) && transition == GST_STATE_CHANGE_READY_TO_PAUSED)
			{
				GST_DEBUG ("screensaver_playbin GST_STATE_CHANGE_READY_TO_PAUSED last_play_pos=%" GST_TIME_FORMAT "", GST_TIME_ARGS (priv->last_play_pos));
				if (priv->last_play_pos != GST_CLOCK_TIME_NONE)
					gst_element_seek (priv->screensaver_playbin, 1.0, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, GST_SEEK_TYPE_SET, priv->last_play_pos, GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
			}
			break;
		}
		case GST_MESSAGE_STREAM_START:
		{
			GST_DEBUG ("GST_MESSAGE_STREAM_START! state=%s", photo_booth_state_get_name (priv->state));
		}
		default:
		{
// 			GST_DEBUG ("gst_message from %" GST_PTR_FORMAT ": %" GST_PTR_FORMAT "", GST_MESSAGE_SRC(message), message);
		}
	}
	return TRUE;
}

static void photo_booth_video_widget_ready (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv;
	GtkRequisition size;
	GtkAllocation size2;
	GstVideoRectangle s1, s2, rect;
	GstElement *element;
	GstCaps *caps;

	priv = photo_booth_get_instance_private (pb);
	gtk_widget_get_preferred_size (priv->win->gtkgstwidget, NULL, &size);
	gtk_widget_get_allocated_size (priv->win->gtkgstwidget, &size2, NULL);
	s1.w = size.width;
	s1.h = size.height;
	s2.w = size2.width;
	s2.h = size2.height;
	gst_video_sink_center_rect (s1, s2, &rect, TRUE);

	GST_DEBUG_OBJECT (pb, "gtksink widget is ready. preferred dimensions: %dx%d allocated %dx%d", size.width, size.height, size2.width, size2.height);

	element = gst_bin_get_by_name (GST_BIN (pb->video_bin), "video-capsfilter");
	caps = gst_caps_new_simple ("video/x-raw", "width", G_TYPE_INT, rect.w, "height", G_TYPE_INT, rect.h, NULL);
	g_object_set (G_OBJECT (element), "caps", caps, NULL);
	gst_caps_unref (caps);
	gst_object_unref (element);

	element = gst_bin_get_by_name (GST_BIN (pb->video_bin), "video-overlay");
	g_object_set (element, "overlay-width", rect.w, NULL);
	g_object_set (element, "overlay-height", rect.h, NULL);
	gst_object_unref (element);

	GST_DEBUG_OBJECT (pb, "gtksink widget is ready. output dimensions: %dx%d", rect.w, rect.h);
	priv->video_size = rect;
}

static gboolean photo_booth_preview (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv = photo_booth_get_instance_private (pb);
	GstPad *pad;
	if (!priv->photo_block_id)
	{
		gst_element_set_state (pb->photo_bin, GST_STATE_READY);
		pad = gst_element_get_static_pad (pb->photo_bin, "src");
		GST_DEBUG_OBJECT (pad, "photo_booth_preview! halt photo_bin...");
		priv->photo_block_id = gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_DATA_DOWNSTREAM, _gst_photo_probecb, pb, NULL);
		gst_object_unref (pad);
		gst_element_unlink (pb->photo_bin, pb->video_sink);
	}
	if (priv->video_block_id)
	{
		pad = gst_element_get_static_pad (pb->video_bin, "src");
		GST_DEBUG_OBJECT (pad, "photo_booth_preview! unblock video_bin pad@%p...", pad);
		gst_pad_remove_probe (pad, priv->video_block_id);
		priv->video_block_id = 0;
		gst_object_unref (pad);
	}
	if (priv->sink_block_id)
	{
		pad = gst_element_get_static_pad (pb->video_sink, "sink");
		GST_DEBUG_OBJECT (pad, "photo_booth_preview! unblock video_sink pad@%p...", pad);
		gst_pad_remove_probe (pad, priv->sink_block_id);
		priv->sink_block_id = 0;
		gst_object_unref (pad);
		gst_element_set_state (pb->video_sink, GST_STATE_PLAYING);
	}
	int ret = gst_element_link (pb->video_bin, pb->video_sink);
	GST_LOG_OBJECT (pb, "linked video-bin ! video-sink ret=%i", ret);
	gst_element_set_state (pb->video_bin, GST_STATE_PLAYING);
	int cooldown_delay = 2000;
	if (priv->state == PB_STATE_NONE)
		cooldown_delay = 10;
	if (priv->state != PB_STATE_UPLOADING)
	{
		photo_booth_change_state (pb, PB_STATE_PREVIEW_COOLDOWN);
		gtk_label_set_text (priv->win->status, _("Please wait..."));
	}
	g_timeout_add (cooldown_delay, (GSourceFunc) photo_booth_preview_ready, pb);
	GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pb->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "photo_booth_preview");
	SEND_COMMAND (pb, CONTROL_VIDEO);
	return FALSE;
}

static gboolean photo_booth_preview_ready (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv = photo_booth_get_instance_private (pb);
	if (priv->state == PB_STATE_UPLOADING)
	{
		GST_DEBUG_OBJECT (pb, "still uploading, wait another bit");
		return TRUE;
	}
	if (!pb->cam_info)
	{
		GST_DEBUG_OBJECT (pb, "camera not ready, waiting");
		return TRUE;
	}
	if (priv->state != PB_STATE_PREVIEW_COOLDOWN)
	{
		GST_DEBUG_OBJECT (pb, "wrong state");
		return FALSE;
	}
	photo_booth_change_state (pb, PB_STATE_PREVIEW);
	gtk_label_set_text (priv->win->status, _("Touch screen to take a photo!"));
	photo_booth_window_hide_cursor (priv->win);

	if (priv->screensaver_timeout > 0)
		priv->screensaver_timeout_id = g_timeout_add_seconds (priv->screensaver_timeout, (GSourceFunc) photo_booth_screensaver, pb);
	return FALSE;
}

static gboolean photo_booth_screensaver (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv = photo_booth_get_instance_private (pb);
	if (priv->state != PB_STATE_PREVIEW && priv->state != PB_STATE_NONE)
	{
		GST_DEBUG_OBJECT (pb, "wrong state %s", photo_booth_state_get_name (priv->state));
		return FALSE;
	}
	photo_booth_change_state (pb, PB_STATE_SCREENSAVER);
	SEND_COMMAND (pb, CONTROL_PAUSE);

	priv->screensaver_timeout_id = 0;
	priv->paused_callback_id = 1;

	return FALSE;
}

static gboolean photo_booth_screensaver_stop (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv = photo_booth_get_instance_private (pb);
	photo_booth_change_state (pb, PB_STATE_NONE);

	GstElement *src = gst_bin_get_by_name (GST_BIN (pb->video_bin), "mjpeg-fdsrc");
	GstPad *pad;
	pad = gst_element_get_static_pad (src, "src");
	gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_IDLE, photo_booth_screensaver_unplug_continue, pb, NULL);
	gst_object_unref (pad);
	gst_object_unref (src);
	return FALSE;
}

static GstPadProbeReturn photo_booth_screensaver_unplug_continue (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
	PhotoBooth *pb = PHOTO_BOOTH (user_data);
	PhotoBoothPrivate *priv;
	priv = photo_booth_get_instance_private (pb);

	g_mutex_lock (&priv->processing_mutex);

        GstPad *sinkpad;
        sinkpad = gst_element_get_static_pad (pb->video_sink, "sink");
        priv->sink_block_id = gst_pad_add_probe (sinkpad, GST_PAD_PROBE_TYPE_DATA_DOWNSTREAM, _gst_video_probecb, pb, NULL);

	gst_element_query_position (priv->screensaver_playbin, GST_FORMAT_TIME, &priv->last_play_pos);
	GST_DEBUG ("stop screensaver @ %" GST_TIME_FORMAT " %" GST_PTR_FORMAT " block_id=%lu", GST_TIME_ARGS (priv->last_play_pos), pad, priv->sink_block_id );

	gst_element_set_state (priv->screensaver_playbin, GST_STATE_NULL);
	gst_element_set_state (pb->pipeline, GST_STATE_READY);

	gst_bin_add (GST_BIN (pb->pipeline), pb->video_sink);
	gst_object_unref (pb->video_sink);
	GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pb->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "photo_booth_screensaver_stop");

	SEND_COMMAND (pb, CONTROL_UNPAUSE);

	g_mutex_unlock (&priv->processing_mutex);
	return GST_PAD_PROBE_REMOVE;
}

static gboolean photo_booth_capture_paused_cb (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv;
	priv = photo_booth_get_instance_private (pb);

	g_mutex_lock (&priv->processing_mutex);
	gst_element_unlink (pb->video_bin, pb->video_sink);
	GST_DEBUG_OBJECT (pb, "continue plugging screensaver pipeline");

	if (priv->sink_block_id)
	{
		GstPad *sinkpad;
		sinkpad = gst_element_get_static_pad (pb->video_sink, "sink");
		GST_DEBUG_OBJECT (sinkpad, "showing screensaver! unblock video_sink...");
		gst_pad_remove_probe (sinkpad, priv->sink_block_id);
		priv->sink_block_id = 0;
		gst_object_unref (sinkpad);
		gst_element_set_state (pb->video_sink, GST_STATE_PLAYING);
	}

	priv->screensaver_playbin = gst_element_factory_make ("playbin", "screensaver-playbin");
	gst_object_ref (pb->video_sink);
	gst_bin_remove (GST_BIN (pb->pipeline), pb->video_sink);
	g_object_set (priv->screensaver_playbin, "video-sink", pb->video_sink, NULL);

	if (priv->screensaver_uri)
		g_object_set (priv->screensaver_playbin, "uri", priv->screensaver_uri, NULL);
	gst_element_set_state (priv->screensaver_playbin, GST_STATE_PLAYING);

	GstBus *bus;
	bus = gst_pipeline_get_bus (GST_PIPELINE (priv->screensaver_playbin));
	gst_bus_add_watch (bus, (GstBusFunc) photo_booth_bus_callback, pb);
	gst_object_unref (GST_OBJECT (bus));

	gtk_label_set_text (priv->win->status, _("Touch screen to take a photo!"));

	g_mutex_unlock (&priv->processing_mutex);
	return FALSE;
}

void photo_booth_background_clicked (GtkWidget *widget, GdkEventButton *event, PhotoBoothWindow *win)
{
	PhotoBooth *pb = PHOTO_BOOTH_FROM_WINDOW (win);
	PhotoBoothPrivate *priv = photo_booth_get_instance_private (pb);
	priv = photo_booth_get_instance_private (pb);
	GST_INFO_OBJECT (pb, "background clicked in state %s", photo_booth_state_get_name (priv->state));

	if (priv->screensaver_timeout_id)
	{
		int ret = g_source_remove (priv->screensaver_timeout_id);
		GST_DEBUG_OBJECT (pb, "removing screensaver_timeout");
		priv->screensaver_timeout_id = 0;
	}
	switch (priv->state) {
		case PB_STATE_PREVIEW:
		{
			photo_booth_snapshot_start (pb);
			break;
		}
		case PB_STATE_COUNTDOWN:
		case PB_STATE_PREVIEW_COOLDOWN:
			GST_DEBUG_OBJECT (pb, "BUSY... ignore");
			break;
		case PB_STATE_TAKING_PHOTO:
		case PB_STATE_PROCESS_PHOTO:
		case PB_STATE_PRINTING:
		{
			GST_DEBUG_OBJECT (pb, "BUSY... install watchdog timeout");
			if (!priv->state_change_watchdog_timeout_id)
				priv->state_change_watchdog_timeout_id = g_timeout_add_seconds (5, (GSourceFunc) photo_booth_watchdog_timedout, pb);
			_play_event_sound (priv, ERROR_SOUND);
			break;
		}
		case PB_STATE_ASK_PRINT:
			g_timeout_add_seconds (15, (GSourceFunc) photo_booth_get_printer_status, pb);
		case PB_STATE_ASK_UPLOAD:
		{
// 			photo_booth_button_cancel_clicked (pb);
			_play_event_sound (priv, ERROR_SOUND);
			break;
		}
		case PB_STATE_SCREENSAVER:
		{
			photo_booth_screensaver_stop (pb);
			_play_event_sound (priv, ACK_SOUND);
			break;
		}
		case PB_STATE_NONE:
		{
			photo_booth_screensaver (pb);
			_play_event_sound (priv, ACK_SOUND);
			break;
		}
		default:
			_play_event_sound (priv, ERROR_SOUND);
			break;
	}

// 	if (priv->prints_remaining < 1)
// 		photo_booth_get_printer_status (pb);
}

static gboolean photo_booth_get_printer_status (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv = photo_booth_get_instance_private (pb);
	gchar *label_string;
	gchar *backend_environment = g_strdup_printf ("BACKEND=%s", priv->printer_backend);
	gchar *argv[] = { "/usr/lib/cups/backend/gutenprint52+usb", "-m", NULL };
	gchar *envp[] = { backend_environment, NULL };
	gchar *output = NULL;
	GError *error = NULL;
	gint remain = -1;
	gint ret = 0;

	if (!priv->printer_backend)
	{
		label_string = g_strdup_printf(_("No printer configured!"));
	}
	else if (g_spawn_sync (NULL, argv, envp, G_SPAWN_DEFAULT, NULL, NULL, NULL, &output, &ret, &error))
	{
		GMatchInfo *match_info;
		GRegex *regex;
		if (ret == 0)
		{
			regex = g_regex_new ("INFO: Media type\\s.*?: (?<code>\\d+) \\((?<size>.*?)\\)\nINFO: Media remaining\\s.*?: (?<remain>\\d{3})/(?<total>\\d{3})\n", G_REGEX_MULTILINE|G_REGEX_DOTALL, 0, &error);
			if (error) {
				g_critical ("%s\n", error->message);
				return FALSE;
			}
			if (g_regex_match (regex, output, 0, &match_info))
			{
				guint code = atoi(g_match_info_fetch_named(match_info, "code"));
				gchar *size = g_match_info_fetch_named(match_info, "size");
				remain = atoi(g_match_info_fetch_named(match_info, "remain"));
				guint total = atoi(g_match_info_fetch_named(match_info, "total"));
				label_string = g_strdup_printf(_("Printer %s online. %i prints (%s) remaining"), priv->printer_backend, remain, size);
				GST_INFO_OBJECT (pb, "printer %s status: media code %i (%s) prints remaining %i of %i", priv->printer_backend, code, size, remain, total);
			}
			else {
				label_string = g_strdup_printf(_("Can't parse printer backend output"));
				GST_ERROR_OBJECT (pb, "%s: '%s'", label_string, output);
			}
		}
		else
		{
			regex = g_regex_new ("ERROR: Printer open failure", G_REGEX_MULTILINE|G_REGEX_DOTALL, 0, &error);
			if (g_regex_match (regex, output, 0, &match_info))
			{
				label_string = g_strdup_printf(_("Printer %s off-line"), priv->printer_backend);
				GST_WARNING_OBJECT (pb, "%s", label_string);
			}
			else {
				label_string = g_strdup_printf(_("can't parse printer backend output"));
				GST_ERROR_OBJECT (pb, "%s: '%s'", label_string, output);
			}
		}
		g_free (output);
		g_match_info_free (match_info);
		g_regex_unref (regex);
	}
	else {
		label_string = g_strdup_printf(_("Can't spawn %s"), argv[0]);
		GST_ERROR_OBJECT (pb, "%s  %s %s (%s)", label_string, argv[1], envp[0], error->message);
		g_error_free (error);
	}
	priv->prints_remaining = remain;
	gtk_label_set_text (priv->win->status_printer, label_string);
	g_free (label_string);
	g_free (backend_environment);
	return FALSE;
}

static void photo_booth_snapshot_start (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv;
	guint pretrigger_delay = 1;
	guint snapshot_delay   = 2;

	priv = photo_booth_get_instance_private (pb);
	photo_booth_change_state (pb, PB_STATE_COUNTDOWN);
	photo_booth_window_start_countdown (priv->win, priv->countdown);
	if (priv->countdown > 1)
	{
		pretrigger_delay = (priv->countdown*1000)-1500;
		snapshot_delay = (priv->countdown*1000)-100;
	}
	GST_DEBUG_OBJECT (pb, "started countdown of %d seconds, pretrigger in %d ms, snapshot in %d ms", priv->countdown, pretrigger_delay, snapshot_delay);
	g_timeout_add (pretrigger_delay, (GSourceFunc) photo_booth_snapshot_prepare, pb);
	g_timeout_add (snapshot_delay,   (GSourceFunc) photo_booth_snapshot_trigger, pb);

	if (priv->countdown_audio_uri)
	{
		g_object_set (priv->audio_playbin, "uri", priv->countdown_audio_uri, NULL);
		gst_element_set_state (priv->audio_pipeline, GST_STATE_PLAYING);
	}
	photo_booth_led_countdown (priv->led, priv->countdown);
}

static gboolean photo_booth_snapshot_prepare (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv;
	GstPad *pad;
	gboolean ret;

	GST_DEBUG_OBJECT (pb, "photo_booth_snapshot_prepare!");
	GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pb->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "photo_booth_pre_snapshot");

	photo_booth_change_state (pb, PB_STATE_TAKING_PHOTO);

	priv = photo_booth_get_instance_private (pb);
	photo_booth_window_set_spinner (priv->win, TRUE);

	SEND_COMMAND (pb, CONTROL_PRETRIGGER);

	return FALSE;
}

static gboolean photo_booth_snapshot_trigger (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv;
	GstPad *pad;

	GST_DEBUG_OBJECT (pb, "photo_booth_snapshot_trigger");
	GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pb->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "photo_booth_snapshot_trigger");

	priv = photo_booth_get_instance_private (pb);

	gst_element_set_state ((priv->audio_pipeline), GST_STATE_READY);

	SEND_COMMAND (pb, CONTROL_PHOTO);

	GST_DEBUG_OBJECT (pb, "preparing for snapshot...");

	return FALSE;
}

extern int camera_auto_focus (Camera *list, GPContext *context, int onoff);

static gboolean photo_booth_focus (CameraInfo *cam_info)
{
	int gpret;
	CameraEventType evttype;
	void *evtdata;


	do {
		g_mutex_lock (&cam_info->mutex);
		gpret = gp_camera_wait_for_event (cam_info->camera, 10, &evttype, &evtdata, cam_info->context);
		g_mutex_unlock (&cam_info->mutex);
		GST_DEBUG ("gp_camera_wait_for_event gpret=%i", gpret);
	} while ((gpret == GP_OK) && (evttype != GP_EVENT_TIMEOUT));

	g_mutex_lock (&cam_info->mutex);
	gpret = camera_auto_focus (cam_info->camera, cam_info->context, 1);
	g_mutex_unlock (&cam_info->mutex);
	if (gpret != GP_OK) {
		GST_WARNING ("gphoto error: %s\n", gp_result_as_string(gpret));
		return FALSE;
	}

	do {
		GST_DEBUG ("gp_camera_wait_for_event gpret=%i", gpret);
		g_mutex_lock (&cam_info->mutex);
		gpret = gp_camera_wait_for_event (cam_info->camera, 10, &evttype, &evtdata, cam_info->context);
		g_mutex_unlock (&cam_info->mutex);
	} while ((gpret == GP_OK) && (evttype != GP_EVENT_TIMEOUT));

	g_mutex_lock (&cam_info->mutex);
	gpret = camera_auto_focus (cam_info->camera, cam_info->context, 0);
	g_mutex_unlock (&cam_info->mutex);
	if (gpret != GP_OK) {
		GST_WARNING ("gphoto error: %s\n", gp_result_as_string(gpret));
	}
	return TRUE;
}

static gboolean photo_booth_take_photo (CameraInfo *cam_info)
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
		goto fail;

	gpret = gp_file_new (&file);
	GST_DEBUG ("gp_file_new gpret=%i", gpret);

	gpret = gp_camera_file_get (cam_info->camera, camera_file_path.folder, camera_file_path.name, GP_FILE_TYPE_NORMAL, file, cam_info->context);
	GST_DEBUG ("gp_camera_file_get gpret=%i", gpret);
	if (gpret < 0)
		goto fail;
	gp_file_get_data_and_size (file, (const char**)&(cam_info->data), &(cam_info->size));
	if (gpret < 0)
		goto fail;

	gpret = gp_camera_file_delete (cam_info->camera, camera_file_path.folder, camera_file_path.name, cam_info->context);
	GST_DEBUG ("gp_camera_file_delete gpret=%i", gpret);
// 	gp_file_free(file);

	if (cam_info->size <= 0)
		goto fail;

	g_mutex_unlock (&cam_info->mutex);
	return TRUE;

fail:
	g_mutex_unlock (&cam_info->mutex);
	return FALSE;
}

static gboolean photo_booth_snapshot_taken (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv = photo_booth_get_instance_private (pb);
	GstElement *appsrc;
	GstBuffer *buffer;
	GstFlowReturn flowret;
	GstPad *pad;
	gboolean ret;

	gst_element_set_state (pb->video_bin, GST_STATE_READY);
	pad = gst_element_get_static_pad (pb->video_bin, "src");
	priv->video_block_id = gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_DATA_DOWNSTREAM, _gst_video_probecb, pb, NULL);
	gst_object_unref (pad);
	gst_element_unlink (pb->video_bin, pb->video_sink);

	if (priv->photo_block_id)
	{
		GST_DEBUG_OBJECT (pb, "unblock photo_bin...");
		pad = gst_element_get_static_pad (pb->photo_bin, "src");
		gst_pad_remove_probe (pad, priv->photo_block_id);
		gst_object_unref (pad);
	}

	ret = gst_element_link (pb->photo_bin, pb->video_sink);
	gst_element_set_state (pb->photo_bin, GST_STATE_PLAYING);

	priv->photos_taken++;
	GST_DEBUG_OBJECT (pb, "photo_booth_snapshot_taken size=%lu photos_taken=%i", pb->cam_info->size, priv->photos_taken);
	gtk_label_set_text (priv->win->status, _("Processing photo..."));

	appsrc = gst_bin_get_by_name (GST_BIN (pb->photo_bin), "photo-appsrc");
	buffer = gst_buffer_new_wrapped (pb->cam_info->data, pb->cam_info->size);
	g_signal_emit_by_name (appsrc, "push-buffer", buffer, &flowret);

	if (flowret != GST_FLOW_OK)
		GST_ERROR_OBJECT (appsrc, "couldn't push %" GST_PTR_FORMAT " to appsrc", buffer);
	gst_object_unref (appsrc);

	gst_element_set_state (pb->photo_bin, GST_STATE_PLAYING);
	pad = gst_element_get_static_pad (pb->photo_bin, "src");
	priv->photo_block_id = gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, photo_booth_catch_photo_buffer, pb, NULL);

	return FALSE;
}

static GstPadProbeReturn photo_booth_catch_photo_buffer (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
	PhotoBooth *pb = PHOTO_BOOTH (user_data);
	PhotoBoothPrivate *priv;
	GstPadProbeReturn ret = GST_PAD_PROBE_PASS;
	priv = photo_booth_get_instance_private (pb);

	GST_LOG_OBJECT (pb, "probe function in state %s... locking", photo_booth_state_get_name (priv->state));
	g_mutex_lock (&priv->processing_mutex);
	switch (priv->state) {
		case PB_STATE_TAKING_PHOTO:
		{
			if (priv->cam_reeinit_after_snapshot)
				SEND_COMMAND (pb, CONTROL_REINIT);
			photo_booth_change_state (pb, PB_STATE_PROCESS_PHOTO);
			GST_DEBUG_OBJECT (pb, "first buffer caught -> display in sink, invoke processing");
			gtk_widget_show (GTK_WIDGET (priv->win->button_print));
			gtk_widget_show (GTK_WIDGET (priv->win->button_cancel));
			photo_booth_window_show_cursor (priv->win);
			g_main_context_invoke (NULL, (GSourceFunc) photo_booth_process_photo_plug_elements, pb);
			break;
		}
		case PB_STATE_PROCESS_PHOTO:
		{
			photo_booth_change_state (pb, PB_STATE_ASK_PRINT);
			GST_DEBUG_OBJECT (pb, "second buffer caught -> will be caught for printing. waiting for answer, hide spinner");
			if (priv->print_copies_min != priv->print_copies_max)
				photo_booth_window_set_copies_show (priv->win, priv->print_copies_min, priv->print_copies_max, priv->print_copies_default);
			photo_booth_window_set_spinner (priv->win, FALSE);
			break;
		}
		case PB_STATE_ASK_PRINT:
		{
			GST_DEBUG_OBJECT (pb, "third buffer caught -> okay this is enough, remove processing elements and probe");
			g_main_context_invoke (NULL, (GSourceFunc) photo_booth_process_photo_remove_elements, pb);
			ret = GST_PAD_PROBE_REMOVE;
			break;
		}
		default:
			break;
	}
	g_mutex_unlock (&priv->processing_mutex);
	GST_LOG_OBJECT (pb, "probe function in state %s... unlocked", photo_booth_state_get_name (priv->state));
	return ret;
}

static gboolean photo_booth_process_photo_plug_elements (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv;
	GstElement *tee, *encoder, *filesink, *lcms, *appsink;
	priv = photo_booth_get_instance_private (pb);

	GST_DEBUG_OBJECT (pb, "plugging photo processing elements. locking...");
	g_mutex_lock (&priv->processing_mutex);
	encoder = gst_bin_get_by_name (GST_BIN (pb->photo_bin), "photo-encoder");
	tee = gst_bin_get_by_name (GST_BIN (pb->photo_bin), "photo-tee");

	encoder = gst_element_factory_make ("jpegenc", "photo-encoder");
	filesink = gst_element_factory_make ("filesink", "photo-filesink");
	if (!encoder || !filesink)
		GST_ERROR_OBJECT (pb->photo_bin, "Failed to make photo encoder");
	priv->save_filename_count++;
	gchar *filename = g_strdup_printf (priv->save_path_template, priv->save_filename_count);
	GST_INFO_OBJECT (pb->photo_bin, "saving photo to '%s'", filename);
	g_object_set (filesink, "location", filename, NULL);
	g_free (filename);

	gst_bin_add_many (GST_BIN (pb->photo_bin), encoder, filesink, NULL);
	tee = gst_bin_get_by_name (GST_BIN (pb->photo_bin), "photo-tee");
	if (!gst_element_link_many (tee, encoder, filesink, NULL))
		GST_ERROR_OBJECT (pb->photo_bin, "couldn't link photobin filewrite elements!");

	lcms = gst_bin_get_by_name (GST_BIN (pb->photo_bin), "print-lcms");
	if (!lcms)
	{
		lcms = gst_element_factory_make ("lcms", "print-lcms");
		if (lcms)
		{
			g_object_set (G_OBJECT (lcms), "intent", 0, NULL);
			g_object_set (G_OBJECT (lcms), "lookup", 2, NULL);
			if (priv->cam_icc_profile)
				g_object_set (G_OBJECT (lcms), "input-profile", priv->cam_icc_profile, NULL);
			if (priv->print_icc_profile)
				g_object_set (G_OBJECT (lcms), "dest-profile", priv->print_icc_profile, NULL);
			g_object_set (G_OBJECT (lcms), "preserve-black", TRUE, NULL);
			gst_bin_add (GST_BIN (pb->photo_bin), lcms);
		}
		else
			GST_WARNING_OBJECT (pb->photo_bin, "no lcms pluing found, ICC color correction unavailable!");
	}

	appsink = gst_element_factory_make ("appsink", "print-appsink");
	if (!appsink )
		GST_ERROR_OBJECT (pb->photo_bin, "Failed to make photo print processing element(s): %s", appsink?"":" appsink");

	gst_bin_add (GST_BIN (pb->photo_bin), appsink);
	if (lcms)
	{
		if (!gst_element_link_many (tee, lcms, appsink, NULL))
			GST_ERROR_OBJECT (pb->photo_bin, "couldn't link tee ! lcms ! appsink!");
	}
	else
	{
		if (!gst_element_link (tee, appsink))
			GST_ERROR_OBJECT (pb->photo_bin, "couldn't link tee ! appsink!");
	}
	g_object_set (G_OBJECT (appsink), "emit-signals", TRUE, NULL);
	g_object_set (G_OBJECT (appsink), "enable-last-sample", FALSE, NULL);
	g_signal_connect (appsink, "new-sample", G_CALLBACK (photo_booth_catch_print_buffer), pb);

	gst_object_unref (tee);
	gst_element_set_state (pb->photo_bin, GST_STATE_PLAYING);
	GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pb->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "photo_booth_process_photo_plug_elements");

	g_mutex_unlock (&priv->processing_mutex);
	GST_DEBUG_OBJECT (pb, "plugged photo processing elements and unlocked.");
	return FALSE;
}

static GstFlowReturn photo_booth_catch_print_buffer (GstElement * appsink, gpointer user_data)
{
	PhotoBooth *pb;
	PhotoBoothPrivate *priv;
	GstSample *sample;
	GstMapInfo map;
	GstPad *pad;

	pb = PHOTO_BOOTH (user_data);
	priv = photo_booth_get_instance_private (pb);
	g_mutex_lock (&priv->processing_mutex);
	sample = gst_app_sink_pull_sample (GST_APP_SINK (appsink));
	priv->print_buffer = gst_buffer_ref( gst_sample_get_buffer (sample));

	pad = gst_element_get_static_pad (appsink, "sink");
	GstCaps *caps = gst_pad_get_current_caps (pad);
	GST_DEBUG_OBJECT (pb, "got photo for printer: %" GST_PTR_FORMAT ". caps = %" GST_PTR_FORMAT "", priv->print_buffer, caps);
	gst_caps_unref (caps);
	gst_sample_unref (sample);
	g_mutex_unlock (&priv->processing_mutex);
	return GST_FLOW_OK;
}

static gboolean photo_booth_process_photo_remove_elements (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv;
	GstElement *tee, *encoder, *filesink, *appsink, *lcms;
	priv = photo_booth_get_instance_private (pb);

	GST_DEBUG_OBJECT (pb, "remove output file encoder and writer elements and pause. locking...");
	g_mutex_lock (&priv->processing_mutex);

	gst_element_set_state (pb->photo_bin, GST_STATE_READY);
	tee = gst_bin_get_by_name (GST_BIN (pb->photo_bin), "photo-tee");
	encoder = gst_bin_get_by_name (GST_BIN (pb->photo_bin), "photo-encoder");
	filesink = gst_bin_get_by_name (GST_BIN (pb->photo_bin), "photo-filesink");
	gst_element_unlink_many (tee, encoder, filesink, NULL);

	appsink = gst_bin_get_by_name (GST_BIN (pb->photo_bin), "print-appsink");
	lcms = gst_bin_get_by_name (GST_BIN (pb->photo_bin), "print-lcms");
	if (lcms)
	{
		gst_element_unlink_many (tee, lcms, appsink, NULL);
		gst_element_set_state (lcms, GST_STATE_READY);
	}
	else
		gst_element_unlink (tee, appsink);

	gst_bin_remove_many (GST_BIN (pb->photo_bin), encoder, filesink, appsink, NULL);
	gst_element_set_state (filesink, GST_STATE_NULL);
	gst_element_set_state (encoder, GST_STATE_NULL);
	gst_element_set_state (appsink, GST_STATE_NULL);
	gst_object_unref (tee);
	gst_object_unref (encoder);
	gst_object_unref (filesink);
	priv->photo_block_id = 0;

	g_mutex_unlock (&priv->processing_mutex);
	GST_DEBUG_OBJECT (pb, "removed output file encoder and writer elements and paused and unlocked.");
	return FALSE;
}

static void photo_booth_free_print_buffer (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv;
	GstElement *appsink;
	priv = photo_booth_get_instance_private (pb);
	GST_DEBUG_OBJECT (pb, "freeing buffer");
	if (GST_IS_BUFFER (priv->print_buffer))
		gst_buffer_unref (priv->print_buffer);
	appsink = gst_bin_get_by_name (GST_BIN (pb->photo_bin), "print-appsink");
	if (GST_IS_ELEMENT (appsink))
		gst_object_unref (appsink);
}

void photo_booth_button_print_clicked (GtkButton *button, PhotoBoothWindow *win)
{
	PhotoBooth *pb = PHOTO_BOOTH_FROM_WINDOW (win);
	PhotoBoothPrivate *priv;
	priv = photo_booth_get_instance_private (pb);
	GST_DEBUG_OBJECT (pb, "photo_booth_button_print_clicked");
	if (priv->state == PB_STATE_ASK_PRINT)
	{
		_play_event_sound (priv, ACK_SOUND);
		photo_booth_print (pb);
	}
}

void photo_booth_button_upload_clicked (GtkButton *button, PhotoBoothWindow *win)
{
	PhotoBooth *pb = PHOTO_BOOTH_FROM_WINDOW (win);
	PhotoBoothPrivate *priv;
	priv = photo_booth_get_instance_private (pb);
	GST_DEBUG_OBJECT (pb, "photo_booth_button_upload_clicked");
	if (priv->state == PB_STATE_ASK_UPLOAD)
	{
		_play_event_sound (priv, ACK_SOUND);
		photo_booth_window_set_spinner (priv->win, TRUE);
		gtk_label_set_text (priv->win->status, _("Uploading..."));
		priv->upload_thread = g_thread_try_new ("upload", (GThreadFunc) photo_booth_post_thread_func, pb, NULL);
		photo_booth_cancel (pb);
	}
}

void photo_booth_button_cancel_clicked (GtkButton *button, PhotoBoothWindow *win)
{
	PhotoBooth *pb = PHOTO_BOOTH_FROM_WINDOW (win);
	GST_DEBUG_OBJECT (button, "photo_booth_button_cancel_clicked");
	_play_event_sound (photo_booth_get_instance_private (pb), ACK_SOUND);
	photo_booth_cancel (pb);
}

void photo_booth_cancel (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv;
	priv = photo_booth_get_instance_private (pb);
	switch (priv->state) {
		case PB_STATE_PROCESS_PHOTO:
			photo_booth_process_photo_remove_elements (pb);
		case PB_STATE_TAKING_PHOTO:
		case PB_STATE_PRINTING:
			break;
		case PB_STATE_ASK_PRINT:
		{
			gtk_widget_hide (GTK_WIDGET (priv->win->button_print));
			photo_booth_window_get_copies_hide (priv->win);
			break;
		}
		case PB_STATE_ASK_UPLOAD:
			gtk_widget_hide (GTK_WIDGET (priv->win->button_upload));
			break;
		default: return;
	}
	gtk_widget_hide (GTK_WIDGET (priv->win->button_cancel));
// 	photo_booth_change_state (pb, PB_STATE_NONE);
	SEND_COMMAND (pb, CONTROL_UNPAUSE);
}

void photo_booth_copies_value_changed (GtkRange *range, PhotoBoothWindow *win)
{
	PhotoBooth *pb = PHOTO_BOOTH_FROM_WINDOW (win);
	PhotoBoothPrivate *priv;
	priv = photo_booth_get_instance_private (pb);
	priv->print_copies = (int) gtk_range_get_value (range);
	GST_DEBUG_OBJECT (range, "\n\nphoto_booth_copies_value_changed value=%d", priv->print_copies);
}

#define ALWAYS_PRINT_DIALOG 1

static void photo_booth_print (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv;
	priv = photo_booth_get_instance_private (pb);
	GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pb->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "photo_booth_photo_print");
	photo_booth_get_printer_status (pb);
	GST_INFO_OBJECT (pb, "PRINT! prints_remaining=%i", priv->prints_remaining);
	priv->print_copies = photo_booth_window_get_copies_hide (priv->win);
	gtk_widget_hide (GTK_WIDGET (priv->win->button_print));

#ifdef ALWAYS_PRINT_DIALOG
	if (1)
#else
	if (priv->prints_remaining > priv->print_copies)
#endif
	{
		gtk_label_set_text (priv->win->status, _("Printing..."));
		photo_booth_change_state (pb, PB_STATE_PRINTING);
		PhotoBoothPrivate *priv;
		GtkPrintOperation *printop;
		GtkPrintOperationResult res;
		GtkPageSetup *page_setup;
		GtkPaperSize *paper_size;
		GError *print_error;
		GtkPrintOperationAction action;

		priv = photo_booth_get_instance_private (pb);
		printop = gtk_print_operation_new ();

		if (priv->printer_settings != NULL)
			action = GTK_PRINT_OPERATION_ACTION_PRINT;
		else
		{
			priv->printer_settings = gtk_print_settings_new ();
			action = GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG;
		}

		gtk_print_operation_set_print_settings (printop, priv->printer_settings);
		g_signal_connect (printop, "begin_print", G_CALLBACK (photo_booth_begin_print), pb);
		g_signal_connect (printop, "draw_page", G_CALLBACK (photo_booth_draw_page), pb);
		g_signal_connect (printop, "done", G_CALLBACK (photo_booth_print_done), pb);

		page_setup = gtk_page_setup_new();
		paper_size = gtk_paper_size_new_custom("custom", "custom", PT_PER_IN*4.0, PT_PER_IN*6.0, GTK_UNIT_POINTS);
		gtk_page_setup_set_orientation (page_setup, GTK_PAGE_ORIENTATION_LANDSCAPE);

		gtk_page_setup_set_paper_size (page_setup, paper_size);
		gtk_print_operation_set_default_page_setup (printop, page_setup);
		gtk_print_operation_set_use_full_page (printop, TRUE);
		gtk_print_operation_set_unit (printop, GTK_UNIT_POINTS);

		res = gtk_print_operation_run (printop, action, GTK_WINDOW (priv->win), &print_error);
		if (res == GTK_PRINT_OPERATION_RESULT_ERROR)
		{
			photo_booth_printing_error_dialog (priv->win, print_error);
			g_error_free (print_error);
		}
		else if (res == GTK_PRINT_OPERATION_RESULT_CANCEL)
		{
			gtk_label_set_text (priv->win->status, _("Printing cancelled"));
			g_object_unref (priv->printer_settings);
			priv->printer_settings = NULL;
			GST_INFO_OBJECT (pb, "print cancelled");
		}
		else if (res == GTK_PRINT_OPERATION_RESULT_APPLY)
		{
			g_object_unref (priv->printer_settings);
			priv->printer_settings = g_object_ref (gtk_print_operation_get_print_settings (printop));
		}
		g_object_unref (printop);
	}
	else if (priv->prints_remaining == -1) {
		gtk_label_set_text (priv->win->status, _("Can't print, no printer connected!"));
	}
	else
		gtk_label_set_text (priv->win->status, _("Can't print, out of paper!"));
}

static void photo_booth_begin_print (GtkPrintOperation *operation, GtkPrintContext *context, gpointer user_data)
{
	PhotoBooth *pb;
	PhotoBoothPrivate *priv;

	pb = PHOTO_BOOTH (user_data);

	priv = photo_booth_get_instance_private (pb);

	GST_INFO_OBJECT (pb, "photo_booth_begin_print %i copies", priv->print_copies);
	gtk_print_operation_set_n_pages (operation, priv->print_copies);
}

static void photo_booth_draw_page (GtkPrintOperation *operation, GtkPrintContext *context, int page_nr, gpointer user_data)
{
	PhotoBooth *pb;
	PhotoBoothPrivate *priv;
	GstMapInfo map;

	pb = PHOTO_BOOTH (user_data);
	priv = photo_booth_get_instance_private (pb);

	if (!GST_IS_BUFFER (priv->print_buffer))
	{
		GST_ERROR_OBJECT (context, "can't draw because we have no photo buffer!");
		return;
	}
	GST_DEBUG_OBJECT (context, "draw_page no. %i . %" GST_PTR_FORMAT " size %dx%d, %i dpi, offsets (%.2f, %.2f)", page_nr, priv->print_buffer, priv->print_width, priv->print_height, priv->print_dpi, priv->print_x_offset, priv->print_y_offset);

	gst_buffer_map(priv->print_buffer, &map, GST_MAP_READ);
	guint8 *h = map.data;
	guint l = map.size;

	int stride = cairo_format_stride_for_width (CAIRO_FORMAT_RGB24, priv->print_width);
	cairo_surface_t *cairosurface = cairo_image_surface_create_for_data (map.data, CAIRO_FORMAT_RGB24, priv->print_width, priv->print_height, stride);
	cairo_t *cr = gtk_print_context_get_cairo_context (context);
	cairo_matrix_t m;
	cairo_get_matrix(cr, &m);

	float scale = (float) PT_PER_IN / (float) priv->print_dpi;
	cairo_scale(cr, scale, scale);
	cairo_set_source_surface(cr, cairosurface, priv->print_x_offset, priv->print_y_offset);
	cairo_paint(cr);
	cairo_set_matrix(cr, &m);
	
	gst_buffer_unmap (priv->print_buffer, &map);
}

static void photo_booth_printing_error_dialog (PhotoBoothWindow *window, GError *print_error)
{
	GtkWidget *error_dialog;
	gchar *error_string;
	error_string = g_strdup_printf(_("Failed to print! Error message: %s"), print_error->message);
	GST_ERROR_OBJECT (window, error_string);
	error_dialog = gtk_message_dialog_new (GTK_WINDOW (window), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, error_string);
	g_signal_connect (error_dialog, "response", G_CALLBACK(gtk_widget_destroy), NULL);
	gtk_widget_show (error_dialog);
	g_free (error_string);
}

static void photo_booth_print_done (GtkPrintOperation *operation, GtkPrintOperationResult result, gpointer user_data)
{
	PhotoBooth *pb;
	PhotoBoothPrivate *priv;
	GstMapInfo map;

	pb = PHOTO_BOOTH (user_data);
	priv = photo_booth_get_instance_private (pb);

	GError *print_error;
	if (result == GTK_PRINT_OPERATION_RESULT_ERROR)
	{
		gtk_print_operation_get_error (operation, &print_error);
		photo_booth_printing_error_dialog (priv->win, print_error);
		g_error_free (print_error);
	}
	else if (result == GTK_PRINT_OPERATION_RESULT_APPLY)
	{
		gint copies = gtk_print_operation_get_n_pages_to_print (operation);
		priv->photos_printed += copies;
		GST_INFO_OBJECT (user_data, "print_done photos_printed copies=%i total=%i", copies, priv->photos_printed);
		photo_booth_led_printer (priv->led, copies);
	}
	else
		GST_INFO_OBJECT (user_data, "print_done photos_printed unhandled result %i", result);

	g_timeout_add_seconds (15, (GSourceFunc) photo_booth_get_printer_status, pb);

	if ((priv->imgur_album_id && priv->imgur_access_token) || priv->facebook_put_uri)
	{
		gtk_widget_show (GTK_WIDGET (priv->win->button_upload));
		g_timeout_add_seconds (priv->upload_timeout, (GSourceFunc) photo_booth_upload_timedout, pb);
		photo_booth_change_state (pb, PB_STATE_ASK_UPLOAD);
	}
	else
		photo_booth_cancel (pb);

	return;
}

size_t _curl_write_func (void *ptr, size_t size, size_t nmemb, void *buf)
{
	int i;
	for (i = 0; i < size*nmemb; i++)
		g_string_append_c((GString *)buf, ((gchar *)ptr)[i]);
	return i;
}

void photo_booth_post_thread_func (PhotoBooth* pb)
{
	PhotoBoothPrivate *priv;
	CURLcode res;
	CURL *curl;
	priv = photo_booth_get_instance_private (pb);
	GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pb->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "photo_booth_facebook_post");
	g_mutex_lock (&priv->upload_mutex);
	curl = curl_easy_init();
	if (curl)
	{
		photo_booth_change_state (pb, PB_STATE_UPLOADING);
		struct curl_httppost* post = NULL;
		struct curl_httppost* last = NULL;
		GString *buf = g_string_new("");
		gchar *filename = g_strdup_printf (priv->save_path_template, priv->save_filename_count);
		curl_formadd (&post, &last, CURLFORM_COPYNAME, "image", CURLFORM_FILE, filename, CURLFORM_CONTENTTYPE, "image/jpeg", CURLFORM_END);
		curl_easy_setopt (curl, CURLOPT_USERAGENT, "Schaffenburg Photobooth");
		if (priv->imgur_access_token && priv->imgur_album_id)
		{
			gchar *auth_header;
			struct curl_slist *headerlist = NULL;
			auth_header = g_strdup_printf ("Authorization: Bearer %s", priv->imgur_access_token);
			headerlist = curl_slist_append (headerlist, auth_header);
			curl_formadd (&post, &last, CURLFORM_COPYNAME, "album", CURLFORM_COPYCONTENTS, priv->imgur_album_id, CURLFORM_END);
			if (priv->imgur_description)
				curl_formadd (&post, &last, CURLFORM_COPYNAME, "description", CURLFORM_COPYCONTENTS, priv->imgur_description, CURLFORM_END);
			curl_easy_setopt (curl, CURLOPT_HTTPHEADER, headerlist);
			curl_easy_setopt (curl, CURLOPT_URL, IMGUR_UPLOAD_URI);
			GST_INFO_OBJECT (pb, "imgur posting '%s' to album http://imgur.com/a/%s'...", filename, priv->imgur_album_id);
			g_free (auth_header);
		}
		else if (priv->facebook_put_uri)
		{
			curl_easy_setopt (curl, CURLOPT_URL, priv->facebook_put_uri);
			GST_INFO_OBJECT (pb, "facebook posting '%s' to '%s'...", filename, priv->facebook_put_uri);
		}
		curl_easy_setopt (curl, CURLOPT_POST, 1L);
		curl_easy_setopt (curl, CURLOPT_HTTPPOST, post);
		curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, _curl_write_func);
		curl_easy_setopt (curl, CURLOPT_WRITEDATA, buf);
		res = curl_easy_perform (curl);
		if (res != CURLE_OK)
		{
			GST_WARNING ("curl_easy_perform() failed %s", curl_easy_strerror(res));
		}
		curl_easy_cleanup (curl);
		curl_formfree (post);
		g_free (filename);
		GST_DEBUG ("curl_easy_perform() finished. response='%s'", buf->str);

		if (priv->do_twitter)
		{
			JsonParser *parser;
			JsonNode *root;
			JsonReader *reader;
			GError *error;
			const char *link_url;

			parser = json_parser_new ();

			error = NULL;
			json_parser_load_from_data (parser, buf->str, buf->len, &error);
			if (error)
				{
					GST_WARNING ("Unable to parse '%s': %s", buf->str, error->message);
					g_error_free (error);
					g_object_unref (parser);
					return;
				}

			root = json_parser_get_root (parser);
			reader = json_reader_new (root);
			json_reader_read_member (reader, "link");
			link_url = json_reader_get_string_value (reader);
			GST_INFO ("imgur uploaded photo url: %s", link_url);


			/* manipulate the object tree and then exit */

			g_object_unref (parser);

		}
		g_string_free (buf, TRUE);
	}
	g_mutex_unlock (&priv->upload_mutex);
	photo_booth_change_state (pb, PB_STATE_PREVIEW_COOLDOWN);
	photo_booth_window_set_spinner (priv->win, FALSE);
	return;
}

static gboolean photo_booth_upload_timedout (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv = photo_booth_get_instance_private (pb);
	if (priv->state == PB_STATE_ASK_UPLOAD)
	{
		GST_DEBUG_OBJECT (pb, "ask upload timed out");
		photo_booth_cancel (pb);
	}
	return FALSE;
}

static gboolean photo_booth_watchdog_timedout (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv = photo_booth_get_instance_private (pb);
	GST_ERROR_OBJECT (pb, "watchdog timed out in state %s", photo_booth_state_get_name(priv->state));
	photo_booth_cancel (pb);
	return FALSE;
}

const gchar* photo_booth_state_get_name (PhotoboothState state)
{
	switch (state) {
		case PB_STATE_NONE: return "PB_STATE_NONE";break;
		case PB_STATE_PREVIEW: return "PB_STATE_PREVIEW";break;
		case PB_STATE_PREVIEW_COOLDOWN: return "PB_STATE_PREVIEW_COOLDOWN";break;
		case PB_STATE_COUNTDOWN: return "PB_STATE_COUNTDOWN";break;
		case PB_STATE_TAKING_PHOTO: return "PB_STATE_TAKING_PHOTO";break;
		case PB_STATE_PROCESS_PHOTO: return "PB_STATE_PROCESS_PHOTO";break;
		case PB_STATE_ASK_PRINT: return "PB_STATE_ASK_PRINT";break;
		case PB_STATE_PRINTING: return "PB_STATE_PRINTING";break;
		case PB_STATE_ASK_UPLOAD: return "PB_STATE_ASK_UPLOAD";break;
		case PB_STATE_UPLOADING: return "PB_STATE_UPLOADING";break;
		case PB_STATE_SCREENSAVER: return "PB_STATE_SCREENSAVER";break;
		default: return "STATE UNKOWN!";break;
	}
	return "";
}

PhotoBooth *photo_booth_new (void)
{
	return g_object_new (PHOTO_BOOTH_TYPE,
			"application-id", "org.schaffenburg.photobooth",
			"flags", G_APPLICATION_HANDLES_OPEN,
			NULL);
}

int main (int argc, char *argv[])
{
	PhotoBooth *pb;
	int ret;

	XInitThreads();
	gst_init (0, NULL);

	pb = photo_booth_new ();
	
	if (argc == 2)
		photo_booth_load_settings (pb, argv[1]);
	else
		photo_booth_load_settings (pb, DEFAULT_CONFIG);

	g_unix_signal_add (SIGINT, (GSourceFunc) photo_booth_quit_signal, pb);
	ret = g_application_run (G_APPLICATION (pb), argc, argv);

	g_object_unref (pb);
	return ret;
}
