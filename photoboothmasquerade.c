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

#include "photobooth.h"
#include "photoboothmasquerade.h"


#define TYPE_PHOTO_BOOTH_MASK (photo_booth_mask_get_type())
#define PHOTO_BOOTH_MASK(obj) \
(G_TYPE_CHECK_INSTANCE_CAST((obj),TYPE_PHOTO_BOOTH_MASK,\
PhotoBoothMask))
#define IS_PHOTO_BOOTH_MASK(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj),TYPE_PHOTO_BOOTH_MASK))

typedef struct _PhotoBoothMask PhotoBoothMask;
typedef struct _PhotoBoothMaskClass PhotoBoothMaskClass;

static GType photo_booth_mask_get_type (void);

struct _PhotoBoothMask
{
	GtkFixed *fixed;
	GdkPixbuf *pixbuf;
	GtkWidget *imagew, *eventw;
	gint screen_offset_x, screen_offset_y;
	gint offset_x, offset_y;
	gboolean dragging;
	gint dragstartoffsetx, dragstartoffsety;
};

struct _PhotoBoothMaskClass
{
	GstObjectClass object_class;
};

G_DEFINE_TYPE (PhotoBoothMask, photo_booth_mask, GST_TYPE_OBJECT);

gboolean photo_booth_masquerade_press   (GtkWidget *widget, GdkEventButton *event, gpointer user_data);
gboolean photo_booth_masquerade_release (GtkWidget *widget, GdkEventButton *event, gpointer user_data);
gboolean photo_booth_masquerade_motion  (GtkWidget *widget, GdkEventMotion *event, gpointer user_data);

static void
photo_booth_mask_finalize (GObject *object)
{
	PhotoBoothMask *mask;
	mask = PHOTO_BOOTH_MASK (object);
	GST_DEBUG_OBJECT (mask, "finalize");
	g_object_unref (mask->pixbuf);
	gtk_container_remove (GTK_CONTAINER (mask->fixed), mask->imagew);
	gtk_container_remove (GTK_CONTAINER (mask->fixed), mask->eventw);
	g_object_unref (mask->eventw);
	g_object_unref (mask->imagew);
	G_OBJECT_CLASS (photo_booth_mask_parent_class)->finalize (object);
}

static void
photo_booth_mask_class_init (PhotoBoothMaskClass *klass)
{
	G_OBJECT_CLASS (klass)->finalize = photo_booth_mask_finalize;
}

static void
photo_booth_mask_init (PhotoBoothMask *mask)
{
	GST_INFO_OBJECT (mask, "mask init. fixed widget: %" GST_PTR_FORMAT, mask->fixed);
	mask->eventw = gtk_event_box_new ();
	mask->imagew = gtk_image_new ();
	gtk_widget_set_can_focus (mask->eventw, FALSE);
	gtk_container_add (GTK_CONTAINER (mask->fixed), mask->imagew);
}

static void
photo_booth_mask_connect_events (PhotoBoothMask *mask, gpointer press, gpointer release, gpointer motion)
{
	GST_DEBUG_OBJECT (mask, "connect events");
	gtk_widget_add_events (mask->eventw, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_BUTTON1_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK);
	g_signal_connect (mask->eventw, "button-press-event", G_CALLBACK (press), mask);
	g_signal_connect (mask->eventw, "button-release-event", G_CALLBACK (release), mask);
	g_signal_connect (mask->eventw, "motion-notify-event", G_CALLBACK (motion), mask);
}

static void
photo_booth_mask_show (PhotoBoothMask *mask, const GValue *face)
{
	const GstStructure *face_struct = gst_value_get_structure (face);
	GdkPixbuf *scaled_mask_pixbuf;
	guint x, y, width, height;
	gst_structure_get_uint (face_struct, "x", &x);
	gst_structure_get_uint (face_struct, "y", &y);
	gst_structure_get_uint (face_struct, "width", &width);
	gst_structure_get_uint (face_struct, "height", &height);
	gdouble scaling_factor;
	scaling_factor = (gdouble) width / (gdouble) gdk_pixbuf_get_width (mask->pixbuf);
	GST_LOG_OBJECT (mask, "mask size: (%dx%d) (scaling factor=%.2f) position: (%d,%d)", width, height, scaling_factor, x, y);
	height = (gdouble) gdk_pixbuf_get_height (mask->pixbuf) * scaling_factor;
	x += mask->screen_offset_x + (gdouble) mask->offset_x * scaling_factor;
	y += mask->screen_offset_y + (gdouble) mask->offset_y * scaling_factor;
	gtk_fixed_move (mask->fixed, mask->eventw, x, y);
	scaled_mask_pixbuf = gdk_pixbuf_scale_simple (mask->pixbuf, width, height, GDK_INTERP_BILINEAR);
	gtk_image_set_from_pixbuf (GTK_IMAGE (mask->imagew), scaled_mask_pixbuf);
	gtk_widget_show (mask->eventw);
	gtk_widget_show (mask->imagew);
}

static void
photo_booth_mask_hide (PhotoBoothMask *mask)
{
	GST_LOG_OBJECT (mask, "mask hide!");
	gtk_widget_hide (mask->eventw);
	gtk_widget_hide (mask->imagew);
}

static PhotoBoothMask *
photo_booth_mask_new (GtkFixed *fixed, gchar *filename, gint offset_x, gint offset_y)
{
	PhotoBoothMask *mask = g_object_new (TYPE_PHOTO_BOOTH_MASK, NULL);
	GError *error;
	GST_DEBUG_OBJECT (mask, "new mask from filename %s with offsets (%d,%d) and fixed widget %" GST_PTR_FORMAT, filename, offset_x, offset_y, fixed);
	mask->fixed = fixed;
	mask->pixbuf = gdk_pixbuf_new_from_file_at_scale (filename, -1, -1, FALSE, &error);
	mask->offset_x = offset_x;
	mask->offset_y = offset_y;
	mask->dragstartoffsetx = mask->dragstartoffsety = 0;
	mask->dragging = FALSE;
	mask->eventw = gtk_event_box_new ();
	gtk_fixed_put (mask->fixed, mask->eventw, 0, 0);
	mask->screen_offset_x = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (fixed), "screen-offset-x"));
	mask->screen_offset_y = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (fixed), "screen-offset-y"));

	return mask;
}

G_DEFINE_TYPE (PhotoBoothMasquerade, photo_booth_masquerade, G_TYPE_OBJECT);

GST_DEBUG_CATEGORY_STATIC (photo_booth_masquerade_debug);
#define GST_CAT_DEFAULT photo_booth_masquerade_debug

static void photo_booth_masquerade_finalize (GObject *object)
{
	PhotoBoothMasquerade *masq = PHOTO_BOOTH_MASQUERADE (object);
	g_list_free_full (masq->masks, g_object_unref);
	masq->masks = NULL;
	GST_DEBUG_OBJECT (masq, "finalize masquerade");
	G_OBJECT_CLASS (photo_booth_masquerade_parent_class)->finalize (object);
}

static void photo_booth_masquerade_class_init (PhotoBoothMasqueradeClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

	GST_DEBUG_CATEGORY_INIT (photo_booth_masquerade_debug, "photoboothmasquerade", GST_DEBUG_BOLD | GST_DEBUG_FG_WHITE | GST_DEBUG_BG_BLUE, "PhotoBoothMasquerade");

	gobject_class->finalize = photo_booth_masquerade_finalize;
}

static void photo_booth_masquerade_init (PhotoBoothMasquerade *masq)
{
	PhotoBoothMask *mask;
	GST_DEBUG_OBJECT (masq, "init masks!");
	mask = photo_booth_mask_new (masq->fixed, "overlays/mask_nasenbrille.png", 0, 40);
	photo_booth_mask_connect_events (mask, photo_booth_masquerade_press, photo_booth_masquerade_release, photo_booth_masquerade_motion);
	masq->masks = g_list_append (masq->masks, mask);

	mask = photo_booth_mask_new (masq->fixed, "overlays/mask_fuchsohren.png", 10, -120);
	photo_booth_mask_connect_events (mask, photo_booth_masquerade_press, photo_booth_masquerade_release, photo_booth_masquerade_motion);
	masq->masks = g_list_append (masq->masks, mask);
}

// void photo_booth_masquerade_set_fixed (PhotoBoothMasquerade *masq, GtkFixed *fixed)
// {
// 	PhotoBoothMask *mask;
// 	GST_INFO_OBJECT (masq, "set fixed %" GST_PTR_FORMAT, fixed);
// 	// TODO this needs to be specified in a config file or image metadata!
//
// 	masq->fixed = fixed;
// }

void photo_booth_masquerade_faces_detected (PhotoBoothMasquerade *masq, const GValue *faces)
{
	guint i, n_masks, n_faces = 0;
	gchar *contents;
	GList *masks;

	n_masks = g_list_length (masq->masks);

	if (faces)
	{
		contents = g_strdup_value_contents (faces);
		n_faces = gst_value_list_get_size (faces);
		GST_TRACE ("Detected objects: %s face=%i masks=%i", *(&contents), n_faces, n_masks);
		g_free (contents);
	}

	for (i = 0; i < n_masks; i++)
	{
		PhotoBoothMask *mask;
		masks = g_list_nth (masq->masks, i);
		mask = masks->data;
		if (mask && i < n_faces)
		{
			const GValue *face = gst_value_list_get_value (faces, i);
			photo_booth_mask_show (mask, face);
		} else {
			photo_booth_mask_hide (mask);
		}
	}
}

void photo_booth_masquerade_facedetect_update (GstStructure *structure)
{
	GstElement *src;
	PhotoBoothMasquerade *masq;
	const GValue *faces;
	GST_DEBUG ("photo_booth_masquerade_facedetect_update");
	gst_structure_get (structure, "masq", G_TYPE_POINTER, &masq, NULL);
	gst_structure_get (structure, "element", G_TYPE_POINTER, &src, NULL);
	faces = gst_structure_get_value (structure, "faces");
	if (g_str_has_prefix (GST_ELEMENT_NAME (src), "video")) {
		photo_booth_masquerade_faces_detected (masq, faces);
	} /*else {
		photo_booth_mask_photo_face_detected (priv->win, faces);
	}*/
}

PhotoBoothMask * _get_mask_for_element (GList *masks, GtkWidget *widget)
{
	guint i, n_masks;
	n_masks = g_list_length (masks);

	for (i = 0; i < n_masks; i++)
	{
		GST_WARNING ("mask iterate %i", i);
		PhotoBoothMask *mask;
		masks = g_list_nth (masks, i);
		mask = masks->data;
		if (mask)
		{
			GST_WARNING_OBJECT (mask->eventw, "mask eventbox");
			if (mask->eventw == widget) {
				GST_WARNING ("FOUND MASK %" GST_PTR_FORMAT, mask->imagew);
				return mask;
			}
		}
	}
	GST_WARNING ("MASK NOT FOUND FOR WIDGET %" GST_PTR_FORMAT, widget);
	return NULL;
}

gboolean photo_booth_masquerade_press (GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
	PhotoBoothMask *mask = PHOTO_BOOTH_MASK (user_data);
	GtkWidget* p;
	gint widgetoffsetx, widgetoffsety, screenoffsetx, screenoffsety;

	GST_INFO_OBJECT (widget, "MASK PRESS");

// 	mask = _get_mask_for_element (priv->masks, widget);

	mask->dragging = TRUE;
	p = gtk_widget_get_parent (widget);
	gdk_window_get_position (gtk_widget_get_parent_window (p), &widgetoffsetx, &widgetoffsety);

	p = gtk_widget_get_parent (p);
	gdk_window_get_position (gtk_widget_get_parent_window (p), &screenoffsetx, &screenoffsety);

	mask->dragstartoffsetx = (int)event->x + widgetoffsetx + mask->screen_offset_x;
	mask->dragstartoffsety = (int)event->y + widgetoffsety + mask->screen_offset_y;

	return TRUE;
}

gboolean photo_booth_masquerade_release (GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
	PhotoBoothMask *mask = PHOTO_BOOTH_MASK (user_data);

	if (mask)
		mask->dragging = FALSE;

	return TRUE;
}

gboolean photo_booth_masquerade_motion (GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
	PhotoBoothMask *mask = PHOTO_BOOTH_MASK (user_data);

	GST_TRACE_OBJECT (mask, "event (%.0f,%.0f) root (%d,%d) off", event->x, event->y, (int)event->x_root, (int)event->y_root);

	if (!mask)
		return TRUE;

	GST_TRACE_OBJECT (mask, "startoffset (%d,%d)", mask->dragstartoffsetx, mask->dragstartoffsety);

	if (mask->dragging)
	{
		int x = (int)event->x_root - mask->dragstartoffsetx;
		int y = (int)event->y_root - mask->dragstartoffsety;
		gtk_fixed_move (GTK_FIXED (mask->fixed), GTK_WIDGET (widget), x, y);
	}
	return TRUE;
}

PhotoBoothMasquerade *photo_booth_masquerade_new (GtkFixed *fixed)
{
	PhotoBoothMasquerade *masq = g_object_new (PHOTO_BOOTH_MASQUERADE_TYPE, NULL);
	masq->fixed = fixed;
	return masq;
}
