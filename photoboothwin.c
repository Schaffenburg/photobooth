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

#include <gtk/gtk.h>
#include <time.h>
#include <glib/gstdio.h>
#include "photobooth.h"
#include "photoboothwin.h"

typedef struct _PhotoBoothWindowPrivate PhotoBoothWindowPrivate;

struct _PhotoBoothWindowPrivate
{
	GtkWidget *overlay;
	GtkWidget *spinner, *statusbar;
	GtkLabel *countdown_label;
	GtkScale *copies;
	gint countdown;
	GtkFixed *fixed;
	GList *masks;
	gboolean dragging;
	gint startoffsetx, startoffsety;
};

typedef struct {
	GdkPixbuf *pixbuf;
	GtkWidget *widget;
	gint x_offset, y_offset;
} PhotoBoothMask;

G_DEFINE_TYPE_WITH_PRIVATE (PhotoBoothWindow, photo_booth_window, GTK_TYPE_APPLICATION_WINDOW);

GST_DEBUG_CATEGORY_STATIC (photo_booth_windows_debug);
#define GST_CAT_DEFAULT photo_booth_windows_debug

gboolean _pbw_tick_countdown (PhotoBoothWindow *win);
gboolean _pbw_clock_tick     (GtkLabel *status_clock);
static void photo_booth_window_dispose (GObject *object);

static void photo_booth_window_class_init (PhotoBoothWindowClass *klass)
{
	GST_DEBUG_CATEGORY_INIT (photo_booth_windows_debug, "photoboothwin", GST_DEBUG_BOLD | GST_DEBUG_FG_WHITE | GST_DEBUG_BG_BLUE, "PhotoBoothWindow");
	G_OBJECT_CLASS (klass)->dispose = photo_booth_window_dispose;
	GError *error = NULL;
	if (G_template_filename)
	{
		GST_DEBUG ("open template from file '%s'", G_template_filename);
		GMappedFile *templatef = g_mapped_file_new (G_template_filename, FALSE, &error);
		gtk_widget_class_set_template (GTK_WIDGET_CLASS (klass), g_mapped_file_get_bytes (templatef));
		g_mapped_file_unref (templatef);
	}
	if (error)
	{
		GST_INFO ( "can't use template from file '%s':  %s. Falling back to default resource!", G_template_filename, error->message);
		g_free (G_template_filename);
		G_template_filename = NULL;
		g_error_free (error);
	}
	if (G_template_filename == NULL)
		gtk_widget_class_set_template_from_resource (GTK_WIDGET_CLASS (klass), "/org/schaffenburg/photobooth/photobooth.ui");

	GST_DEBUG ("done!");
	gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (klass), PhotoBoothWindow, overlay);
	gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (klass), PhotoBoothWindow, spinner);
	gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (klass), PhotoBoothWindow, countdown_label);
	gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (klass), PhotoBoothWindow, copies);
	gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (klass), PhotoBoothWindow, fixed);
	gtk_widget_class_bind_template_child (GTK_WIDGET_CLASS (klass), PhotoBoothWindow, image);
	gtk_widget_class_bind_template_child (GTK_WIDGET_CLASS (klass), PhotoBoothWindow, button_cancel);
	gtk_widget_class_bind_template_child (GTK_WIDGET_CLASS (klass), PhotoBoothWindow, button_print);
	gtk_widget_class_bind_template_child (GTK_WIDGET_CLASS (klass), PhotoBoothWindow, button_upload);
	gtk_widget_class_bind_template_child (GTK_WIDGET_CLASS (klass), PhotoBoothWindow, switch_flip);
	gtk_widget_class_bind_template_child (GTK_WIDGET_CLASS (klass), PhotoBoothWindow, status_clock);
	gtk_widget_class_bind_template_child (GTK_WIDGET_CLASS (klass), PhotoBoothWindow, status);
	gtk_widget_class_bind_template_child (GTK_WIDGET_CLASS (klass), PhotoBoothWindow, status_printer);
}

static void photo_booth_window_init (PhotoBoothWindow *win)
{
	GError *error = NULL;
	gtk_widget_init_template (GTK_WIDGET (win));
	PhotoBoothWindowPrivate *priv;
	priv = photo_booth_window_get_instance_private (win);
	GST_TRACE_OBJECT (priv->countdown_label, "countdown_label");
	GST_TRACE_OBJECT (priv->copies, "copies");
	GdkScreen *screen = gdk_screen_get_default ();
	gtk_window_fullscreen_on_monitor (GTK_WINDOW (win), screen, 0);
	if (G_stylesheet_filename)
	{
		GFile *cssfile = g_file_new_for_path (G_stylesheet_filename);
		if (cssfile)
		{
			GST_DEBUG ("open stylesheet from file '%s'", G_stylesheet_filename);
			GtkCssProvider *cssprovider = gtk_css_provider_new ();
			gtk_css_provider_load_from_file (cssprovider, cssfile, NULL);
			gtk_style_context_add_provider_for_screen (screen, (GtkStyleProvider *)cssprovider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
			g_object_unref (cssfile);
		}
	}
	gtk_button_set_label (win->button_cancel, _("Cancel"));
	gtk_button_set_label (win->button_print, _("Print photo"));
	gtk_button_set_label (win->button_upload, _("Upload photo"));
	g_timeout_add (1000, (GSourceFunc) _pbw_clock_tick, win->status_clock);

	gtk_widget_set_has_window (GTK_WIDGET (priv->fixed), TRUE);
	priv->dragging = FALSE;
	priv->startoffsetx = 0, priv->startoffsety = 0;

	// TODO this needs to be specified in a config file or image metadata!
	PhotoBoothMask *mask = g_slice_new0 (PhotoBoothMask);
	mask->pixbuf = gdk_pixbuf_new_from_file_at_scale ("overlays/mask_nasenbrille.png", -1, -1, FALSE, &error);
	mask->widget = gtk_image_new ();
	mask->x_offset = 150;
	mask->y_offset = 50;
	gtk_fixed_put (priv->fixed, mask->widget, 0, 0);
	priv->masks = g_list_append (priv->masks, mask);
	
	mask = g_slice_new0 (PhotoBoothMask);
	mask->pixbuf = gdk_pixbuf_new_from_file_at_scale ("overlays/mask_fuchsohren.png", -1, -1, FALSE, &error);
	mask->widget = gtk_image_new ();
	mask->x_offset = 180;
	mask->y_offset = -100;
	gtk_fixed_put (priv->fixed, mask->widget, 0, 0);
	priv->masks = g_list_append (priv->masks, mask);
}

static void photo_booth_window_dispose (GObject *object)
{
	PhotoBoothWindowPrivate *priv;
	priv = photo_booth_window_get_instance_private (PHOTO_BOOTH_WINDOW (object));
	g_list_free (priv->masks);
}

void photo_booth_window_add_gtkgstwidget (PhotoBoothWindow *win, GtkWidget *gtkgstwidget)
{
	PhotoBoothWindowPrivate *priv;
	priv = photo_booth_window_get_instance_private (win);
	gtk_container_add (GTK_CONTAINER (priv->overlay), gtkgstwidget);
	gtk_widget_add_events (gtkgstwidget, GDK_BUTTON_PRESS_MASK);
	gtk_widget_realize (gtkgstwidget);
	gtk_widget_show (gtkgstwidget);
	gtk_widget_show (priv->overlay);
	win->gtkgstwidget = gtkgstwidget;
}

gboolean _pbw_clock_tick (GtkLabel *status_clock)
{
	gchar clockstr[200];
	time_t now;
	struct tm *now_tm;
	time (&now);
	now_tm = localtime (&now);
	if (!now_tm)
		return TRUE;
	if (strftime(clockstr, sizeof(clockstr), "%A, %d. %B %Y\t%T", now_tm) == 0)
		return TRUE;
	gtk_label_set_text (status_clock, clockstr);
	return TRUE;
}

void photo_booth_window_set_spinner (PhotoBoothWindow *win, gboolean active)
{
	PhotoBoothWindowPrivate *priv;
	priv = photo_booth_window_get_instance_private (win);
	if (active)
	{
		gtk_spinner_start (GTK_SPINNER (priv->spinner));
		gtk_widget_show (priv->spinner);
	}
	else
	{
		gtk_spinner_stop (GTK_SPINNER (priv->spinner));
		gtk_widget_hide (priv->spinner);
	}
}

gboolean _pbw_tick_countdown (PhotoBoothWindow *win)
{
	PhotoBoothWindowPrivate *priv;
	gchar *str;
	priv = photo_booth_window_get_instance_private (win);
	priv->countdown--;
	GST_DEBUG ("_pbw_tick_countdown %i", priv->countdown);
	if (priv->countdown > 0)
	{
		gchar *status_str = g_strdup_printf (_("Taking photo in %d seconds..."), priv->countdown);
		gtk_label_set_text (win->status, status_str);
		str = g_strdup_printf ("%d...", priv->countdown);
		gtk_label_set_text (priv->countdown_label, str);
		g_free (str);
	}
	else if (priv->countdown == 0)
	{
		gtk_label_set_text (priv->countdown_label, _("SAY CHEESE!"));
	}
	else if (priv->countdown == -1)
	{
		gtk_widget_hide (GTK_WIDGET (priv->countdown_label));
	}
	return FALSE;
}

void photo_booth_window_hide_cursor (PhotoBoothWindow *win)
{
	GdkDisplay *display = gdk_display_get_default ();
	GdkCursor *cursor_blank = gdk_cursor_new_for_display (display, GDK_BLANK_CURSOR);
	gdk_window_set_cursor (gtk_widget_get_window (GTK_WIDGET (win)), cursor_blank);
}

void photo_booth_window_show_cursor (PhotoBoothWindow *win)
{
	GdkDisplay *display = gdk_display_get_default ();
	GdkCursor* cursor_arrow = gdk_cursor_new_for_display (display, GDK_ARROW);
	gdk_window_set_cursor (gtk_widget_get_window (GTK_WIDGET (win)), cursor_arrow);
}

void photo_booth_window_start_countdown (PhotoBoothWindow *win, gint count)
{
	PhotoBoothWindowPrivate *priv;
	int i;
	GST_DEBUG ("photo_booth_window_start_countdown %i", count);
	priv = photo_booth_window_get_instance_private (win);
	priv->countdown = count+1;
	_pbw_tick_countdown(win);
	gtk_widget_show (GTK_WIDGET (priv->countdown_label));
	for (i = 1; i <= count+2; i++)
	{
		g_timeout_add (1000*i, (GSourceFunc) _pbw_tick_countdown, win);
		GST_INFO ("added timeout callback at %i", 1000*i);
	}
}

void photo_booth_window_set_copies_show (PhotoBoothWindow *win, gint min, gint max, gint def)
{
	PhotoBoothWindowPrivate *priv;
	int x;
	priv = photo_booth_window_get_instance_private (win);
	GST_DEBUG ("photo_booth_window_set_copies_limit [%i-%i]", min, max);
	GtkAdjustment *adj = gtk_range_get_adjustment (GTK_RANGE (priv->copies));
	gtk_adjustment_set_lower (adj, (gdouble) min);
	gtk_adjustment_set_upper (adj, (gdouble) max);
	priv = photo_booth_window_get_instance_private (win);
	gtk_range_set_value (GTK_RANGE (priv->copies), (gdouble) def);
	for (x = min; x <= max; x++)
	{
		gtk_scale_add_mark (priv->copies, (gdouble) x, GTK_POS_BOTTOM, NULL);
	}
	gtk_widget_show (GTK_WIDGET (priv->copies));
}

gint photo_booth_window_get_copies_hide (PhotoBoothWindow *win)
{
	PhotoBoothWindowPrivate *priv;
	gint copies = 0;
	priv = photo_booth_window_get_instance_private (win);
	copies = (gint) gtk_range_get_value (GTK_RANGE (priv->copies));
	gtk_widget_hide (GTK_WIDGET (priv->copies));
	GST_DEBUG ("photo_booth_window_get_copies_hide %i", copies);
	return copies;
}

gchar* photo_booth_window_format_copies_value (GtkScale *scale, gdouble value, gpointer user_data)
{
	int intval = (int) value;
	if (intval == 1)
		return g_strdup (_("1 print"));
	return g_strdup_printf (_("%d prints"), intval);
}

void photo_booth_window_face_detected (PhotoBoothWindow *win, const GValue *faces)
{
	PhotoBoothWindowPrivate *priv;
	guint i, n_masks, n_faces;
	gchar *contents;
	GList *masks;

	priv = photo_booth_window_get_instance_private (win);
	contents = g_strdup_value_contents (faces);
	
	n_faces = gst_value_list_get_size (faces);
	n_masks = g_list_length (priv->masks);
	
	GST_TRACE ("Detected objects: %s face=%i masks=%i", *(&contents), n_faces, n_masks);
	g_free (contents);

	for (i = 0; i < n_masks; i++)
	{
		PhotoBoothMask *mask;
		masks = g_list_nth (priv->masks, i);
		mask = masks->data;
		if (i < n_faces)
		{
			const GValue *face = gst_value_list_get_value (faces, i);
			const GstStructure *face_struct = gst_value_get_structure (face);
			GdkPixbuf *scaled_mask_pixbuf;
			guint x, y, width, height;
			gst_structure_get_uint (face_struct, "x", &x);
			gst_structure_get_uint (face_struct, "y", &y);
			gst_structure_get_uint (face_struct, "width", &width);
			gst_structure_get_uint (face_struct, "height", &height);
			GST_LOG ("mask[%i] size: (%dx%d) position: (%d,%d)", i, width, height, x, y);
			gdouble aspect;
			aspect = (gdouble) gdk_pixbuf_get_width (mask->pixbuf) / (gdouble) gdk_pixbuf_get_height (mask->pixbuf);
			height = width / aspect;
			x += mask->x_offset;
			y += mask->y_offset;
			gtk_fixed_move (priv->fixed, mask->widget, x, y);
			scaled_mask_pixbuf = gdk_pixbuf_scale_simple (mask->pixbuf, width, height, GDK_INTERP_BILINEAR);
			gtk_image_set_from_pixbuf (GTK_IMAGE (mask->widget), scaled_mask_pixbuf);
			gtk_widget_show (mask->widget);
		} else {
			GST_LOG ("mask[%i] hide!", i);
			gtk_widget_hide (mask->widget);
		}
	}
}

PhotoBoothWindow * photo_booth_window_new (PhotoBooth *pb)
{
	return g_object_new (PHOTO_BOOTH_WINDOW_TYPE, "application", pb, NULL);
}
