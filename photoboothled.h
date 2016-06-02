/*
 * GStreamer led.h
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

#ifndef __PHOTO_BOOTH_LED_H__
#define __PHOTO_BOOTH_LED_H__

#include <glib-object.h>
#include <glib.h>

#define LED_BLACK       'b'
#define LED_COUNTDOWN   'c'
#define LED_FLASH       'f'
#define LED_DEVICENAME  "/dev/ttyACM"

G_BEGIN_DECLS

#define PHOTO_BOOTH_LED_TYPE                (photo_booth_led_get_type ())
#define PHOTO_BOOTH_LED(obj)                (G_TYPE_CHECK_INSTANCE_CAST ((obj),PHOTO_BOOTH_LED_TYPE,PhotoBoothLed))
#define PHOTO_BOOTH_LED_CLASS(klass)        (G_TYPE_CHECK_CLASS_CAST ((klass), PHOTO_BOOTH_LED_TYPE,PhotoBoothLedClass))
#define IS_PHOTO_BOOTH_LED(obj)             (G_TYPE_CHECK_INSTANCE_TYPE ((obj),PHOTO_BOOTH_LED_TYPE))
#define IS_PHOTO_BOOTH_LED_CLASS(klass)     (G_TYPE_CHECK_CLASS_TYPE ((klass), PHOTO_BOOTH_LED_TYPE))

typedef struct _PhotoBoothLed              PhotoBoothLed;
typedef struct _PhotoBoothLedClass         PhotoBoothLedClass;

struct _PhotoBoothLed
{
	GObject parent;
	int fd;
};

struct _PhotoBoothLedClass
{
	GObjectClass parent_class;
};

GType           photo_booth_led_get_type        (void);
PhotoBoothLed  *photo_booth_led_new             (void);
void            photo_booth_led_black           (PhotoBoothLed *led);
void            photo_booth_led_countdown       (PhotoBoothLed *led, gint seconds);
void            photo_booth_led_flash           (PhotoBoothLed *led);

G_END_DECLS

#endif /* __PHOTO_BOOTH_LED_H__ */
