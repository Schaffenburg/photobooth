/*
 * GStreamer photoboothmasquerade.c
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

#include <gst/video/gstvideosink.h>
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
	GstObject parent;
	guint index;
	gboolean active;
	const gchar *filename;
	GtkFixed *fixed;
	GdkPixbuf *pixbuf, *pixbuf_icon, *pixbuf_copy;
	GtkWidget *imagew, *eventw;
	gint screen_offset_x, screen_offset_y;
	gint offset_x, offset_y;
	gdouble print_scaling_factor;
	gboolean dragging;
	gint dragstartoffsetx, dragstartoffsety;
	GstVideoRectangle print_rectangle;
};

struct _PhotoBoothMaskClass
{
	GObjectClass object_class;
};

G_DEFINE_TYPE (PhotoBoothMask, photo_booth_mask, G_TYPE_OBJECT);

GST_DEBUG_CATEGORY_STATIC (photo_booth_masquerade_debug);
#define GST_CAT_DEFAULT photo_booth_masquerade_debug

#define MASK_ICON_SIZE 64

static void photo_booth_mask_connect_events (PhotoBoothMask *mask, gpointer press, gpointer release, gpointer motion);
static void photo_booth_mask_create_overlay (PhotoBoothMask *mask, GstElement *mask_bin);
static void photo_booth_mask_show (PhotoBoothMask *mask, const GValue *face, GstStructure *structure);
static void photo_booth_mask_hide (PhotoBoothMask *mask);

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
	g_object_unref (mask->pixbuf_icon);
	if (mask->pixbuf_copy)
		g_object_unref (mask->pixbuf_copy);
	mask->imagew = mask->eventw = NULL;
	G_OBJECT_CLASS (photo_booth_mask_parent_class)->finalize (object);
}

static void
photo_booth_mask_class_init (PhotoBoothMaskClass *klass)
{
	GST_DEBUG_CATEGORY_INIT (photo_booth_masquerade_debug, "photoboothmask", GST_DEBUG_BOLD | GST_DEBUG_FG_WHITE | GST_DEBUG_BG_BLUE, "PhotoBoothMask");
	G_OBJECT_CLASS (klass)->finalize = photo_booth_mask_finalize;
}

static void
photo_booth_mask_init (PhotoBoothMask *mask)
{
	GST_LOG_OBJECT (mask, "mask init");
	mask->pixbuf = mask->pixbuf_icon = mask->pixbuf_copy = NULL;
	mask->eventw = gtk_event_box_new ();
	mask->imagew = gtk_image_new ();
	gtk_widget_set_can_focus (mask->eventw, FALSE);
}

static void
photo_booth_mask_connect_events (PhotoBoothMask *mask, gpointer press, gpointer release, gpointer motion)
{
	GST_LOG_OBJECT (mask, "connect events");
	gtk_widget_add_events (mask->eventw, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_BUTTON1_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK);
	g_signal_connect (mask->eventw, "button-press-event", G_CALLBACK (press), mask);
	g_signal_connect (mask->eventw, "button-release-event", G_CALLBACK (release), mask);
	g_signal_connect (mask->eventw, "motion-notify-event", G_CALLBACK (motion), mask);
}

static void
photo_booth_mask_create_overlay (PhotoBoothMask *mask, GstElement *mask_bin)
{
	GstPad *ghost_srcpad;
	GstPad *prev_srcpad, *new_sinkpad, *new_srcpad;
	const gchar *element_name;
	GstElement *photo_overlay = NULL;
	guint width, height, x, y;

	width = mask->print_rectangle.w;
	height = mask->print_rectangle.h;

	GValue pos = G_VALUE_INIT;
	g_value_init (&pos, G_TYPE_INT);
	gtk_container_child_get_property (GTK_CONTAINER (mask->fixed), GTK_WIDGET (mask->eventw), "x", &pos);
	x = g_value_get_int (&pos);

	g_value_reset (&pos);
	gtk_container_child_get_property (GTK_CONTAINER (mask->fixed), GTK_WIDGET (mask->eventw), "y", &pos);
	y = g_value_get_int (&pos);

	GST_DEBUG_OBJECT (mask, "mask [%d] original widget size (%dx%d) @ (%d, %d)", mask->index, width, height, x, y);

	// convert screen widget's coordinates, size and offset to print values
	width = (gdouble) width / mask->print_scaling_factor;
	height = (gdouble) height / mask->print_scaling_factor;
	gdouble video_scaling_factor = (gdouble) width / (gdouble) gdk_pixbuf_get_width (mask->pixbuf);
	x = (gdouble) ((x - mask->screen_offset_x) - (gdouble) mask->offset_x * video_scaling_factor * mask->print_scaling_factor) / mask->print_scaling_factor;
	y = (gdouble) ((y - mask->screen_offset_y) - (gdouble) mask->offset_y * video_scaling_factor * mask->print_scaling_factor) / mask->print_scaling_factor;

	GST_DEBUG_OBJECT (mask, "mask->screen_offset_y=%d, mask->offset_y=%d", mask->screen_offset_y, mask->offset_y);
	GST_DEBUG_OBJECT (mask, "mask [%d] scaled   widget size (%dx%d) @ (%d, %d)", mask->index, width, height, x, y);

	element_name = g_strdup_printf (PHOTO_MASKOVERLAY_NAME_TEMPLATE, mask->index);
	photo_overlay = gst_bin_get_by_name (GST_BIN (mask_bin), element_name);
	if (photo_overlay) {
		GST_DEBUG_OBJECT (mask, "using existing photo_overlay element %" GST_PTR_FORMAT, photo_overlay);
	} else {
		photo_overlay = gst_element_factory_make ("gdkpixbufoverlay", element_name);
		GST_DEBUG_OBJECT (mask, "created new photo_overlay element %" GST_PTR_FORMAT, photo_overlay);
		int ret = gst_bin_add (GST_BIN (mask_bin), photo_overlay);
		g_assert (ret);
		ghost_srcpad = gst_element_get_static_pad (mask_bin, "src");
		prev_srcpad = gst_ghost_pad_get_target (GST_GHOST_PAD (ghost_srcpad));
		new_sinkpad = gst_element_get_static_pad (photo_overlay, "sink");
		new_srcpad = gst_element_get_static_pad (photo_overlay, "src");
		ret = gst_ghost_pad_set_target (GST_GHOST_PAD (ghost_srcpad), new_srcpad);
		g_assert (ret);
		GstPadLinkReturn lret = gst_pad_link (prev_srcpad, new_sinkpad);
		g_assert (lret == GST_PAD_LINK_OK);
	}

	mask->pixbuf_copy = gdk_pixbuf_copy (mask->pixbuf); // because GStreamer's gdkpixbufoverlay modifies the channel layout in-place
	g_object_set (photo_overlay, "pixbuf", mask->pixbuf_copy,
	                             "overlay-width", width,
	                             "overlay-height", height,
	                             "offset-x", x,
	                             "offset-y", y,
	                             NULL);

	photo_booth_mask_hide (mask);
}

static void
photo_booth_mask_show (PhotoBoothMask *mask, const GValue *face, GstStructure *structure)
{
	int state;
	gboolean is_video;

	if (!mask->imagew || !structure || !face)
		return;

	gst_structure_get_int (structure, "state", &state);
	gst_structure_get_boolean (structure, "is-video", &is_video);

	const GstStructure *face_struct = gst_value_get_structure (face);
	GdkPixbuf *scaled_mask_pixbuf;
	guint x, y, width, height;
	gdouble video_scaling_factor;

	gst_structure_get_uint (face_struct, "x", &x);
	gst_structure_get_uint (face_struct, "y", &y);
	gst_structure_get_uint (face_struct, "width", &width);
	gst_structure_get_uint (face_struct, "height", &height);

	video_scaling_factor = (gdouble) width / (gdouble) gdk_pixbuf_get_width (mask->pixbuf);

	if (is_video) {
		height = (gdouble) gdk_pixbuf_get_height (mask->pixbuf) * video_scaling_factor;
		x += mask->screen_offset_x + (gdouble) mask->offset_x * video_scaling_factor;
		y += mask->screen_offset_y + (gdouble) mask->offset_y * video_scaling_factor;
		GST_LOG_OBJECT (mask, "VIDEO mask size: (%dx%d) (video scaling factor=%.2f) position: (%d,%d) state: (%s)", width, height, video_scaling_factor, x, y, photo_booth_state_get_name (state));
	}
	else { // Captured Photo
		width = (gdouble) width * mask->print_scaling_factor;
		height = (gdouble) gdk_pixbuf_get_height (mask->pixbuf) * video_scaling_factor * mask->print_scaling_factor;
		x = (gdouble) x * mask->print_scaling_factor + mask->screen_offset_x + (gdouble) mask->offset_x * video_scaling_factor * mask->print_scaling_factor;
		y = (gdouble) y * mask->print_scaling_factor + mask->screen_offset_y + (gdouble) mask->offset_y * video_scaling_factor * mask->print_scaling_factor;
		GST_DEBUG_OBJECT (mask, "PHOTO mask size: (%dx%d) (print scaling factor=%.2f) position: (%d,%d) state: (%s)", width, height, mask->print_scaling_factor, x, y, photo_booth_state_get_name (state));
		mask->print_rectangle.w = width;
		mask->print_rectangle.h = height;
		photo_booth_mask_connect_events (mask, photo_booth_masquerade_press, photo_booth_masquerade_release, photo_booth_masquerade_motion);
	}
	gtk_fixed_move (mask->fixed, mask->eventw, x, y);
	scaled_mask_pixbuf = gdk_pixbuf_scale_simple (mask->pixbuf, width, height, GDK_INTERP_BILINEAR);
	gtk_image_set_from_pixbuf (GTK_IMAGE (mask->imagew), scaled_mask_pixbuf);
	gtk_widget_show (mask->eventw);
	gtk_widget_show (mask->imagew);
	g_object_unref (scaled_mask_pixbuf);
	mask->active = TRUE;
}

static void
photo_booth_mask_hide (PhotoBoothMask *mask)
{
	if (!mask->active || !mask->imagew)
		return;
	GST_LOG_OBJECT (mask, "mask hide!");
	gtk_widget_hide (mask->eventw);
	gtk_widget_hide (mask->imagew);
	mask->active = FALSE;
}

static PhotoBoothMask *
photo_booth_mask_new (guint index, GtkFixed *fixed, const gchar *filename, gint offset_x, gint offset_y, gdouble print_scaling_factor)
{
	PhotoBoothMask *mask = g_object_new (TYPE_PHOTO_BOOTH_MASK, NULL);
	GError *error = NULL;
	mask->index = index;
	mask->filename = filename;
	mask->active = FALSE;
	mask->pixbuf = gdk_pixbuf_new_from_file_at_scale (filename, -1, -1, FALSE, &error);
	if (error) {
		GST_WARNING_OBJECT (mask, "couldn't load mask file '%s': %s", filename, error->message);
		g_error_free (error);
		return NULL;
	}
	mask->pixbuf_icon = gdk_pixbuf_new_from_file_at_size (filename, MASK_ICON_SIZE, MASK_ICON_SIZE, &error);
	mask->fixed = g_object_ref (fixed);
	mask->offset_x = offset_x;
	mask->offset_y = offset_y;
	mask->print_scaling_factor = print_scaling_factor;
	mask->dragstartoffsetx = mask->dragstartoffsety = 0;
	mask->dragging = FALSE;
	gtk_fixed_put (mask->fixed, mask->eventw, 0, 0);
	gtk_container_add (GTK_CONTAINER (mask->eventw), mask->imagew);
	mask->screen_offset_x = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (fixed), "screen-offset-x"));
	mask->screen_offset_y = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (fixed), "screen-offset-y"));

	GST_DEBUG_OBJECT (mask, "new mask [%i] from filename %s with offsets (%d,%d) and fixed widget %" GST_PTR_FORMAT "@%p", 
		index, filename, offset_x, offset_y, fixed, mask->pixbuf);

	return mask;
}

typedef struct _PhotoBoothMasqueradePrivate PhotoBoothMasqueradePrivate;

struct _PhotoBoothMasqueradePrivate
{
	guint primary_mask_index;
	GList *masks;
};

G_DEFINE_TYPE_WITH_PRIVATE (PhotoBoothMasquerade, photo_booth_masquerade, G_TYPE_OBJECT);

static void photo_booth_masquerade_finalize (GObject *object)
{
	PhotoBoothMasquerade *masq = PHOTO_BOOTH_MASQUERADE (object);
	PhotoBoothMasqueradePrivate *priv = photo_booth_masquerade_get_instance_private (masq);

	g_list_free_full (priv->masks, g_object_unref);
	priv->masks = NULL;
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
	GST_LOG_OBJECT (masq, "init masquerade");
	PhotoBoothMasqueradePrivate *priv = photo_booth_masquerade_get_instance_private (masq);
	priv->masks = NULL;
	priv->primary_mask_index = 0;
}

void photo_booth_masquerade_init_masks (PhotoBoothMasquerade *masq, GtkFixed *fixed, const gchar *dir, gchar *list_json, gint print_width)
{
	JsonParser *parser;
	JsonNode *root;
	JsonReader *reader;
	GError *error = NULL;
	gint i, n_masks, index = 0;
	PhotoBoothMasqueradePrivate *priv = photo_booth_masquerade_get_instance_private (masq);

	if (!list_json)
		return;

	parser = json_parser_new ();

	json_parser_load_from_data (parser, list_json, -1, &error);
	if (error)
		goto fail;

	root = json_parser_get_root (parser);
	reader = json_reader_new (root);

	if (!json_reader_is_array(reader))
		goto fail;

	n_masks = json_reader_count_elements (reader);

	GST_DEBUG ("found %i masks in list", n_masks);

	masq->store = gtk_list_store_new (NUM_COLS, G_TYPE_INT, G_TYPE_STRING, GDK_TYPE_PIXBUF);
	GtkTreeIter iter;
	gtk_list_store_append (masq->store, &iter);
	gtk_list_store_set (masq->store, &iter, COL_INDEX, -1, COL_TEXT, _("No mask"), COL_ICON, NULL, -1);

	for (i = 0; i < n_masks; i++)
	{
		const gchar *filename, *title;
		gchar *maskpath;
		gint offset_x, offset_y, width;
		gdouble print_scaling_factor;
		PhotoBoothMask *mask;

		if (!json_reader_read_element (reader, i))
			goto fail;
		if (!json_reader_read_element (reader, 0))
			goto fail;
		filename = json_reader_get_string_value (reader);
		json_reader_end_element (reader);
		if (!json_reader_read_element (reader, 1))
			goto fail;
		offset_x = json_reader_get_int_value (reader);
		json_reader_end_element (reader);
		if (!json_reader_read_element (reader, 2))
			goto fail;
		offset_y = json_reader_get_int_value (reader);
		json_reader_end_element (reader);
		if (!json_reader_read_element (reader, 3))
			goto fail;
		title = json_reader_get_string_value (reader);
		json_reader_end_element (reader);
		maskpath = g_strconcat (dir, filename, NULL);
		width = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (fixed), "video-width"));
		print_scaling_factor = (gdouble) width / print_width;
		mask = photo_booth_mask_new (index, fixed, maskpath, offset_x, offset_y, print_scaling_factor);
		if (mask) {
			priv->masks = g_list_append (priv->masks, mask);
			gtk_list_store_append (masq->store, &iter);
			gtk_list_store_set (masq->store, &iter, COL_INDEX, index, COL_TEXT, title, COL_ICON, mask->pixbuf_icon, -1);
			index++;
		}
		json_reader_end_element (reader);
		g_free (maskpath);
	}
	g_object_unref (reader);
	g_object_unref (parser);
	return;

fail:
	GST_WARNING_OBJECT (masq, "couldn't parse masks list JSON '%s': %s", list_json, error->message);
	g_error_free (error);
	g_object_unref (reader);
	g_object_unref (parser);
}

void photo_booth_masquerade_set_primary_mask (PhotoBoothMasquerade *masq, guint index)
{
	PhotoBoothMasqueradePrivate *priv = photo_booth_masquerade_get_instance_private (masq);
	priv->primary_mask_index = index;
	GST_DEBUG ("setting primary mask index %i", priv->primary_mask_index);
	photo_booth_masquerade_facedetect_update (masq, NULL);
}

static gint _pbm_sort_faces_by_xpos (gconstpointer f1, gconstpointer f2, G_GNUC_UNUSED gpointer user_data)
{
	guint x1, x2;
	const GstStructure *face_struct;
	face_struct = gst_value_get_structure ((GValue*) f1);
	gst_structure_get_uint (face_struct, "x", &x1);
	face_struct = gst_value_get_structure ((GValue*) f2);
	gst_structure_get_uint (face_struct, "x", &x2);
	return ( x1>x2 ? +1 : -1);
}

void photo_booth_masquerade_facedetect_update (PhotoBoothMasquerade *masq, GstStructure *structure)
{
	if (!IS_PHOTO_BOOTH_MASQUERADE (masq))
		return;

	guint i, n_masks, mask_index, n_faces = 0;
	GList *sorted_faces = NULL;
	const GValue *faces = NULL;
	int state = 0;
	PhotoBoothMasqueradePrivate *priv = photo_booth_masquerade_get_instance_private (masq);

	if (structure) {
		faces = gst_structure_get_value (structure, "faces");
		gst_structure_get_int (structure, "state", &state);
	} else {
		GST_LOG ("GstStructure missing for face detection, hide all");
	}
	n_masks = g_list_length (priv->masks);

	if ((PhotoboothState) state > PB_STATE_TAKING_PHOTO)
	{
		GST_LOG ("don't do update in state %s!", photo_booth_state_get_name ((PhotoboothState) state));
		return;
	}

	if (GST_VALUE_HOLDS_LIST (faces) /*&& gst_debug_category_get_threshold (photo_booth_masquerade_debug) > GST_LEVEL_TRACE*/)
	{
		gchar *contents = g_strdup_value_contents (faces);
		n_faces = gst_value_list_get_size (faces);
		GST_LOG ("Detected objects: %s faces=%i masks=%i", *(&contents), n_faces, n_masks);
		g_free (contents);
	}

	for (guint i = 0; i < n_faces; i++)
	{
		const GValue *face = gst_value_list_get_value (faces, i);
		sorted_faces = g_list_insert_sorted_with_data (sorted_faces, (GValue *) face, (GCompareDataFunc) _pbm_sort_faces_by_xpos, NULL);
	}

	if ((PhotoboothState) state == PB_STATE_TAKING_PHOTO)
	{
		sorted_faces = g_list_reverse (sorted_faces);
	}

	mask_index = priv->primary_mask_index;
	for (i = 0; i < n_masks; i++)
	{
		PhotoBoothMask *mask;
		if (mask_index >= n_masks)
			mask_index = 0;
		mask = (g_list_nth (priv->masks, mask_index))->data;
		if (mask && i < n_faces)
		{
			const GValue *face = g_list_nth_data (sorted_faces, i);
			photo_booth_mask_show (mask, face, structure);
		} else {
			photo_booth_mask_hide (mask);
		}
		mask_index++;
	}
}

void
photo_booth_masquerade_create_overlays (PhotoBoothMasquerade *masq, GstElement *mask_bin)
{
	GList *m;
	PhotoBoothMasqueradePrivate *priv = photo_booth_masquerade_get_instance_private (masq);
	GST_DEBUG_OBJECT (mask_bin, "photo_booth_masquerade_create_overlays");
	for (m = priv->masks; m != NULL; m = m->next) {
		if (PHOTO_BOOTH_MASK (m->data)->active) {
			photo_booth_mask_create_overlay (m->data, mask_bin);
		}
	}
}

void
photo_booth_masquerade_clear_mask_bin (G_GNUC_UNUSED PhotoBoothMasquerade *masq, GstElement *mask_bin)
{
	GstIterator *iter = NULL;
	GValue value = { 0 };
	GstElement *elem = NULL;
	gboolean done = FALSE;
	GST_DEBUG_OBJECT (mask_bin, "clearing mask bin!");

	iter = gst_bin_iterate_elements (GST_BIN (mask_bin));
	while (!done) {
		switch (gst_iterator_next (iter, &value)) {
			case GST_ITERATOR_OK:
				elem = (GstElement *) g_value_get_object (&value);
				g_object_set (elem, "pixbuf", NULL, NULL);
				GST_LOG_OBJECT (elem, "unsetting pixbuf property");
				/* Iterator increased the element refcount, so unref */
				g_value_unset (&value);
				break;
			case GST_ITERATOR_RESYNC:
				gst_iterator_resync (iter);
				break;
			case GST_ITERATOR_ERROR:
				GST_WARNING_OBJECT (mask_bin, "error in iterating elements");
				done = TRUE;
				break;
			case GST_ITERATOR_DONE:
				done = TRUE;
				break;
		}
	}
	gst_iterator_free (iter);
}

gboolean photo_booth_masquerade_press (GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
	PhotoBoothMask *mask = PHOTO_BOOTH_MASK (user_data);
	GtkWidget* p;
	gint widgetoffsetx, widgetoffsety, screenoffsetx, screenoffsety;

	GST_TRACE_OBJECT (widget, "MASK PRESS");

	mask->dragging = TRUE;
	p = gtk_widget_get_parent (widget);
	gdk_window_get_position (gtk_widget_get_parent_window (p), &widgetoffsetx, &widgetoffsety);

	p = gtk_widget_get_parent (p);
	gdk_window_get_position (gtk_widget_get_parent_window (p), &screenoffsetx, &screenoffsety);

	GST_INFO_OBJECT (mask, "MASK PRESS event event (%.0f,%.0f) widget offsets (%d,%d)", event->x, event->y, widgetoffsetx, widgetoffsety);
	mask->dragstartoffsetx = (int)event->x + widgetoffsetx + mask->screen_offset_x;
	mask->dragstartoffsety = (int)event->y + widgetoffsety + mask->screen_offset_y;

	return TRUE;
}

gboolean photo_booth_masquerade_release (G_GNUC_UNUSED GtkWidget *widget, G_GNUC_UNUSED GdkEventButton *event, gpointer user_data)
{
	PhotoBoothMask *mask = PHOTO_BOOTH_MASK (user_data);

	if (mask) {
		mask->dragging = FALSE;
	}

	return TRUE;
}

gboolean photo_booth_masquerade_motion (GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
	PhotoBoothMask *mask = PHOTO_BOOTH_MASK (user_data);

	GST_INFO_OBJECT (mask, "event (%.0f,%.0f) root (%d,%d) off", event->x, event->y, (int)event->x_root, (int)event->y_root);

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

PhotoBoothMasquerade *photo_booth_masquerade_new ()
{
	return g_object_new (TYPE_PHOTO_BOOTH_MASQUERADE, NULL);
}
