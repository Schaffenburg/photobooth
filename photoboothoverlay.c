/*
 * GStreamer photoboothoverlay.c
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
#include "photoboothoverlay.h"

#define TYPE_PHOTO_BOOTH_OVERLAY (photo_booth_overlay_get_type())
#define PHOTO_BOOTH_OVERLAY(obj) \
(G_TYPE_CHECK_INSTANCE_CAST((obj),TYPE_PHOTO_BOOTH_OVERLAY,\
PhotoBoothOverlay))
#define IS_PHOTO_BOOTH_OVERLAY(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj),TYPE_PHOTO_BOOTH_OVERLAY))

typedef struct _PhotoBoothOverlay PhotoBoothOverlay;
typedef struct _PhotoBoothOverlayClass PhotoBoothOverlayClass;

static GType photo_booth_overlay_get_type (void);

struct _PhotoBoothOverlay
{
	GstObject parent;
	guint index;
	const gchar *filename;
	GtkFixed *fixed;
	GdkPixbuf *pixbuf, *pixbuf_icon, *pixbuf_copy;
    GstVideoRectangle rect;
};

struct _PhotoBoothOverlayClass
{
	GObjectClass object_class;
};

G_DEFINE_TYPE (PhotoBoothOverlay, photo_booth_overlay, G_TYPE_OBJECT);

GST_DEBUG_CATEGORY_STATIC (photo_booth_overlay_debug);
#define GST_CAT_DEFAULT photo_booth_overlay_debug

#define OVERLAY_ICON_SIZE 64

static void
photo_booth_overlay_finalize (GObject *object)
{
	PhotoBoothOverlay *overlay;
	overlay = PHOTO_BOOTH_OVERLAY (object);
	GST_DEBUG_OBJECT (overlay, "finalize");
	g_object_unref (overlay->pixbuf);
	g_object_unref (overlay->pixbuf_icon);
	if (overlay->pixbuf_copy)
		g_object_unref (overlay->pixbuf_copy);
	G_OBJECT_CLASS (photo_booth_overlay_parent_class)->finalize (object);
}

static void
photo_booth_overlay_class_init (PhotoBoothOverlayClass *klass)
{
	GST_DEBUG_CATEGORY_INIT (photo_booth_overlay_debug, "photoboothoverlay", GST_DEBUG_BOLD | GST_DEBUG_FG_WHITE | GST_DEBUG_BG_BLUE, "PhotoBoothOverlay");
	G_OBJECT_CLASS (klass)->finalize = photo_booth_overlay_finalize;
}

static void
photo_booth_overlay_init (PhotoBoothOverlay *overlay)
{
	GST_LOG_OBJECT (overlay, "overlay init");
	overlay->pixbuf = overlay->pixbuf_icon = overlay->pixbuf_copy = NULL;
}

static void
photo_booth_overlay_show (PhotoBoothOverlay *overlay, GtkFixed *fixed, GtkImage *imagew)
{
	GST_DEBUG_OBJECT (overlay, "show overlay index=%d filename=%s", overlay->index, overlay->filename);
	gtk_image_set_from_pixbuf (imagew, overlay->pixbuf);
    gtk_fixed_move (fixed, GTK_WIDGET (imagew), overlay->rect.x, overlay->rect.y);
    g_object_set_data (G_OBJECT (fixed), "screen-offset-y", GINT_TO_POINTER (overlay->rect.y));
    GValue off = G_VALUE_INIT;
    g_value_init (&off, G_TYPE_INT);
    gtk_container_child_get_property (GTK_CONTAINER (fixed), GTK_WIDGET (imagew), "x", &off);
    gint screen_offset_x = g_value_get_int (&off);
    g_object_set_data (G_OBJECT (fixed), "screen-offset-x", GINT_TO_POINTER (screen_offset_x));
}

static PhotoBoothOverlay *
photo_booth_overlay_new (guint index, const gchar *filename, GstVideoRectangle rect, GtkAllocation size2)
{
	PhotoBoothOverlay *overlay = g_object_new (TYPE_PHOTO_BOOTH_OVERLAY, NULL);
	GError *error = NULL;
	overlay->index = index;
	overlay->filename = filename;
    overlay->rect = rect;

	overlay->pixbuf = gdk_pixbuf_new_from_file_at_size (filename, rect.w, rect.h, &error);
	if (error) {
		GST_WARNING_OBJECT (overlay, "couldn't load overlay file '%s': %s", filename, error->message);
		g_error_free (error);
		return NULL;
	}
	overlay->pixbuf_icon = gdk_pixbuf_new_from_file_at_size (filename, OVERLAY_ICON_SIZE, OVERLAY_ICON_SIZE, &error);

	if (gdk_pixbuf_get_width (overlay->pixbuf) != rect.w || gdk_pixbuf_get_height (overlay->pixbuf) != rect.h)
	{
		GST_DEBUG ("overlay_image original dimensions %dx%d. aspect mismatch -> we need to scale!", gdk_pixbuf_get_width (overlay->pixbuf), gdk_pixbuf_get_height (overlay->pixbuf));
		overlay->pixbuf = gdk_pixbuf_scale_simple (overlay->pixbuf, rect.w, rect.h, GDK_INTERP_BILINEAR);
	}
	overlay->rect.x = (size2.width-gdk_pixbuf_get_width (overlay->pixbuf))/2;
	overlay->rect.y = (size2.height-gdk_pixbuf_get_height (overlay->pixbuf))/2;
	
	GST_DEBUG_OBJECT (overlay, "new overlay [%i] from filename %s with dimensions %dx%d pos@%d,%d", overlay->index, overlay->filename, gdk_pixbuf_get_width (overlay->pixbuf), gdk_pixbuf_get_height (overlay->pixbuf), overlay->rect.x, overlay->rect.y);

	return overlay;
}

typedef struct _PhotoBoothOverlaysPrivate PhotoBoothOverlaysPrivate;

struct _PhotoBoothOverlaysPrivate
{
	guint active_index;
	GList *overlays;
    GtkFixed *fixed; GtkImage *imagew;
};

G_DEFINE_TYPE_WITH_PRIVATE (PhotoBoothOverlays, photo_booth_overlays, G_TYPE_OBJECT);

static void photo_booth_overlays_finalize (GObject *object)
{
	PhotoBoothOverlays *overlays = PHOTO_BOOTH_OVERLAYS (object);
	PhotoBoothOverlaysPrivate *priv = photo_booth_overlays_get_instance_private (overlays);

	g_list_free_full (priv->overlays, g_object_unref);
	priv->overlays = NULL;
	G_OBJECT_CLASS (photo_booth_overlays_parent_class)->finalize (object);
}

static void photo_booth_overlays_class_init (PhotoBoothOverlaysClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

	GST_DEBUG_CATEGORY_INIT (photo_booth_overlay_debug, "photoboothoverlay", GST_DEBUG_BOLD | GST_DEBUG_FG_WHITE | GST_DEBUG_BG_BLUE, "PhotoBoothOverlay");

	gobject_class->finalize = photo_booth_overlays_finalize;
}

static void photo_booth_overlays_init (PhotoBoothOverlays *overlays)
{
	GST_LOG_OBJECT (overlays, "init overlays");
	PhotoBoothOverlaysPrivate *priv = photo_booth_overlays_get_instance_private (overlays);
	priv->overlays = NULL;
	priv->active_index = 0;
}

PhotoBoothOverlays *
photo_booth_overlays_new (PhotoBoothWindow *win, const gchar *dir, gchar *list_json)
{
    PhotoBoothOverlays *overlays = g_object_new (TYPE_PHOTO_BOOTH_OVERLAYS, NULL);
    PhotoBoothOverlaysPrivate *priv = photo_booth_overlays_get_instance_private (overlays);
    GtkRequisition size;
	GtkAllocation size2;
	GstVideoRectangle s1, s2, rect;
	JsonParser *parser;
	JsonNode *root;
	JsonReader *reader;
	GError *error = NULL;
	gint i, n_overlays, index = 0;

	gtk_widget_get_preferred_size (win->gtkgstwidget, NULL, &size);
	gtk_widget_get_allocated_size (win->gtkgstwidget, &size2, NULL);
	s1.w = size.width;
	s1.h = size.height;
	s2.w = size2.width;
	s2.h = size2.height;
	gst_video_sink_center_rect (s1, s2, &rect, TRUE);

    g_object_set_data (G_OBJECT (win->fixed), "video-width", GINT_TO_POINTER(rect.w));
    g_object_set_data (G_OBJECT (win->fixed), "video-height", GINT_TO_POINTER(rect.h));

    priv->imagew = win->image;
    priv->fixed = win->fixed;

	if (!list_json)
		return NULL;

	parser = json_parser_new ();

	json_parser_load_from_data (parser, list_json, -1, &error);
	if (error)
		goto fail;

	root = json_parser_get_root (parser);
	reader = json_reader_new (root);

	if (!json_reader_is_array(reader))
		goto fail;

	n_overlays = json_reader_count_elements (reader);

	GST_DEBUG ("init %d overlays new with preferred dimensions: %dx%d allocated %dx%d", n_overlays, size.width, size.height, size2.width, size2.height);

	overlays->store = gtk_list_store_new (NUM_COLS, G_TYPE_INT, G_TYPE_STRING, GDK_TYPE_PIXBUF);
	GtkTreeIter iter;

	for (i = 0; i < n_overlays; i++)
	{
		const gchar *filename, *title;
		gchar *overlaypath;
		PhotoBoothOverlay *overlay;

		if (!json_reader_read_element (reader, i))
			goto fail;
		if (!json_reader_read_element (reader, 0))
			goto fail;
		filename = json_reader_get_string_value (reader);
		json_reader_end_element (reader);
		if (!json_reader_read_element (reader, 1))
			goto fail;
		title = json_reader_get_string_value (reader);
		json_reader_end_element (reader);
		overlaypath = g_strconcat (dir, filename, NULL);

		overlay = photo_booth_overlay_new (index, overlaypath, rect, size2);
		if (overlay) {
			priv->overlays = g_list_append (priv->overlays, overlay);
			gtk_list_store_append (overlays->store, &iter);
			gtk_list_store_set (overlays->store, &iter, COL_INDEX, index, COL_TEXT, title, COL_ICON, overlay->pixbuf_icon, -1);
			index++;
		}
		json_reader_end_element (reader);
		// g_free (overlaypath);
	}
	g_object_unref (reader);
	g_object_unref (parser);
	return overlays;

fail:
	GST_WARNING_OBJECT (overlays, "couldn't parse overlays list JSON '%s': %s", list_json, error->message);
	g_error_free (error);
	g_object_unref (reader);
	g_object_unref (parser);
    return NULL;
}

void photo_booth_overlays_set_index (PhotoBoothOverlays *overlays, guint index, GstElement *photo_bin)
{
	guint n;
    PhotoBoothOverlaysPrivate *priv = photo_booth_overlays_get_instance_private (overlays);

    n = g_list_length (priv->overlays);

    GST_DEBUG_OBJECT (overlays, "have %d overlays, set index %d", n, index);

    if (index < n) {
        PhotoBoothOverlay *overlay;
		overlay = (g_list_nth (priv->overlays, index))->data;
        GST_DEBUG_OBJECT (overlay, "overlay %s", overlay->filename);
		if (overlay)
        {
            photo_booth_overlay_show (overlay, priv->fixed, priv->imagew);
            if (GST_IS_ELEMENT (photo_bin)) {
                GstElement *photo_overlay = gst_bin_get_by_name (GST_BIN (photo_bin), "photo-overlay");
                g_assert (photo_overlay);
                g_object_set (photo_overlay, "location", overlay->filename, NULL);
                gst_object_unref (photo_overlay);
            }
            priv->active_index = index;
            return;
        }
    }
    GST_WARNING_OBJECT (overlays, "Couldn't show overlay index %i", index);
}
guint photo_booth_overlays_get_count (PhotoBoothOverlays *overlays)
{
    if (!IS_PHOTO_BOOTH_OVERLAYS(overlays))
        return 0;
    PhotoBoothOverlaysPrivate *priv = photo_booth_overlays_get_instance_private (overlays);
    return g_list_length (priv->overlays);
}
