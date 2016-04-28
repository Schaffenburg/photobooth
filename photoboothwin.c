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
#include "photobooth.h"
#include "photoboothwin.h"

typedef struct _PhotoBoothWindowPrivate PhotoBoothWindowPrivate;

struct _PhotoBoothWindowPrivate
{
	GtkWidget *overlay;
	GtkWidget *spinner, *statusbar;
	GtkLabel *countdown_label;
	gint countdown;
};

G_DEFINE_TYPE_WITH_PRIVATE (PhotoBoothWindow, photo_booth_window, GTK_TYPE_APPLICATION_WINDOW);

GST_DEBUG_CATEGORY_STATIC (photo_booth_windows_debug);
#define GST_CAT_DEFAULT photo_booth_windows_debug

gboolean _pbw_tick_countdown (PhotoBoothWindow *win);
gboolean _pbw_clock_tick (GtkLabel *status_clock);

static void photo_booth_window_class_init (PhotoBoothWindowClass *klass)
{
	GST_DEBUG_CATEGORY_INIT (photo_booth_windows_debug, "photoboothwin", GST_DEBUG_BOLD | GST_DEBUG_FG_WHITE | GST_DEBUG_BG_BLUE, "PhotoBoothWindow");
	gtk_widget_class_set_template_from_resource (GTK_WIDGET_CLASS (klass), "/org/schaffenburg/photobooth/photobooth.ui");
	gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (klass), PhotoBoothWindow, overlay);
	gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (klass), PhotoBoothWindow, spinner);
	gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (klass), PhotoBoothWindow, countdown_label);
	gtk_widget_class_bind_template_child (GTK_WIDGET_CLASS (klass), PhotoBoothWindow, button_yes);
	gtk_widget_class_bind_template_child (GTK_WIDGET_CLASS (klass), PhotoBoothWindow, status_clock);
	gtk_widget_class_bind_template_child (GTK_WIDGET_CLASS (klass), PhotoBoothWindow, status);
	gtk_widget_class_bind_template_child (GTK_WIDGET_CLASS (klass), PhotoBoothWindow, status_printer);
}

static void photo_booth_window_init (PhotoBoothWindow *win)
{
	gtk_widget_init_template (GTK_WIDGET (win));
	PhotoBoothWindowPrivate *priv;
	priv = photo_booth_window_get_instance_private (win);
	GdkScreen *screen = gdk_screen_get_default ();
	gtk_window_fullscreen_on_monitor (GTK_WINDOW (win), screen, 0);
	GFile *cssfile = g_file_new_for_path ("photobooth.css");
	if (cssfile)
	{
		GtkCssProvider *cssprovider = gtk_css_provider_new ();
		gtk_css_provider_load_from_file (cssprovider, cssfile, NULL);
		gtk_style_context_add_provider_for_screen (screen, (GtkStyleProvider *)cssprovider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
		g_object_unref (cssfile);
	}
	g_timeout_add (1000, (GSourceFunc) _pbw_clock_tick, win->status_clock);
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
		gchar *status_str = g_strdup_printf ("Taking photo in %d seconds...", priv->countdown);
		gtk_label_set_text (win->status, status_str);
		str = g_strdup_printf ("%d...", priv->countdown);
		gtk_label_set_text (priv->countdown_label, str);
		g_free (str);
	}
	else if (priv->countdown == 0)
	{
		gtk_label_set_text (priv->countdown_label, "SAY CHEESE!");
	}
	else if (priv->countdown == -1)
	{
		gtk_widget_hide (GTK_WIDGET (priv->countdown_label));
		return FALSE;
	}
	return TRUE;
}

void photo_booth_window_start_countdown (PhotoBoothWindow *win, gint count)
{
	PhotoBoothWindowPrivate *priv;
	GST_DEBUG ("photo_booth_window_start_countdown %i", count);
	priv = photo_booth_window_get_instance_private (win);
	priv->countdown = count+1;
	_pbw_tick_countdown(win);
	gtk_widget_show (GTK_WIDGET (priv->countdown_label));
	g_timeout_add (1000, (GSourceFunc) _pbw_tick_countdown, win);
}

PhotoBoothWindow * photo_booth_window_new (PhotoBooth *pb)
{
	return g_object_new (PHOTO_BOOTH_WINDOW_TYPE, "application", pb, NULL);
}
