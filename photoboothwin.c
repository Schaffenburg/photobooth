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

#include "photobooth.h"
#include "photoboothwin.h"

typedef struct _PhotoBoothWindowPrivate PhotoBoothWindowPrivate;

struct _PhotoBoothWindowPrivate
{
	GtkWidget *overlay;
	GtkWidget *drawing_area, *spinner, *statusbar;
};

G_DEFINE_TYPE_WITH_PRIVATE (PhotoBoothWindow, photo_booth_window, GTK_TYPE_APPLICATION_WINDOW);

static void photo_booth_window_class_init (PhotoBoothWindowClass *klass)
{
	GST_DEBUG_OBJECT (klass, "photo_booth_window_class_init");
	gtk_widget_class_set_template_from_resource (GTK_WIDGET_CLASS (klass), "/org/schaffenburg/photobooth/photobooth.ui");
	gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (klass), PhotoBoothWindow, overlay);
	gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (klass), PhotoBoothWindow, drawing_area);
	gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (klass), PhotoBoothWindow, spinner);
// 	gtk_widget_class_bind_template_child_private (GTK_WIDGET_CLASS (klass), PhotoBoothWindow, statusbar);
}

static void photo_booth_window_init (PhotoBoothWindow *win)
{
	GST_DEBUG_OBJECT (win, "photo_booth_window_init");
	gtk_widget_init_template (GTK_WIDGET (win));
}

void photo_booth_window_setup (PhotoBoothWindow *win, GdkRectangle *monitor_geo)
{
	PhotoBoothWindowPrivate *priv;
	g_print ("photo_booth_window_setup\n");
	priv = photo_booth_window_get_instance_private (win);
	GdkScreen *screen = gdk_screen_get_default ();
	gtk_window_fullscreen_on_monitor (GTK_WINDOW (win), screen, 0);
	GdkWindow *w = gdk_screen_get_active_window (screen);
	gint m = gdk_screen_get_monitor_at_window (screen, w);
	gdk_screen_get_monitor_geometry (screen, m, monitor_geo);
	gtk_widget_add_events (priv->drawing_area, GDK_BUTTON_PRESS_MASK);
	GFile *cssfile = g_file_new_for_path ("photobooth.css");
	if (cssfile)
	{
		GtkCssProvider *cssprovider = gtk_css_provider_new ();
		gtk_css_provider_load_from_file (cssprovider, cssfile, NULL);
		gtk_style_context_add_provider_for_screen (screen, (GtkStyleProvider *)cssprovider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
		g_print ("added css style provider\n");
		g_object_unref (cssfile);
	}
	gtk_widget_show_all (priv->overlay);
	gtk_widget_show_all (GTK_WIDGET (win));
}

GtkWidget* photo_booth_window_get_drawing_area (PhotoBoothWindow *win)
{
	PhotoBoothWindowPrivate *priv;
	priv = photo_booth_window_get_instance_private (win);
	return priv->drawing_area;
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

PhotoBoothWindow * photo_booth_window_new (PhotoBooth *pb)
{
	g_print ("photo_booth_window_new\n");
	return g_object_new (PHOTO_BOOTH_WINDOW_TYPE, "photobooth", pb, NULL);
}
