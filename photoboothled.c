/*
* led.c
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

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include "photobooth.h"
#include "photoboothled.h"

G_DEFINE_TYPE (PhotoBoothLed, photo_booth_led, G_TYPE_OBJECT);

GST_DEBUG_CATEGORY_STATIC (photo_booth_led_debug);
#define GST_CAT_DEFAULT photo_booth_led_debug

static void photo_booth_led_finalize (GObject *object); 

static void photo_booth_led_class_init (PhotoBoothLedClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

	GST_DEBUG_CATEGORY_INIT (photo_booth_led_debug, "photoboothled", GST_DEBUG_BOLD | GST_DEBUG_FG_WHITE | GST_DEBUG_BG_BLUE, "PhotoBoothLed");

	gobject_class->finalize      = photo_booth_led_finalize;
}

static void photo_booth_led_init (PhotoBoothLed *led)
{
	led->fd = -1;
	for (int i=0; i < 10; i++)
	{
		gchar *devicename = g_strdup_printf ("%s%d", LED_DEVICENAME, i);
		GST_DEBUG_OBJECT (led, "trying to open device %s... ", devicename);
		gboolean exists = g_file_test ((const char*) devicename, G_FILE_TEST_EXISTS);
		if (exists)
		{
			led->fd = open(devicename, O_RDWR | O_NOCTTY);
			if (led->fd == -1)
				GST_WARNING_OBJECT (led, "couldn't open %s: %s (%i)", devicename, strerror(errno), errno);
			else
			{
				GST_INFO_OBJECT (led, "successfully opened %s", devicename);
				g_free (devicename);
				break;
			}
		}
		g_free (devicename);
	}
	
	if (led->fd > 0)
	{
		struct termios tty;
		if (tcgetattr (led->fd, &tty) == 0) {
			cfsetospeed(&tty, B115200);
			cfsetispeed(&tty, B115200);
			tty.c_cflag &= ~PARENB;
			tty.c_cflag &= ~CSTOPB;
			tty.c_cflag &= ~CSIZE;
			tty.c_cflag |= CS8;
			tty.c_cflag &= ~CRTSCTS; 
			tty.c_cflag |= CLOCAL | CREAD;
			tty.c_iflag |= IGNPAR | IGNCR;
			tty.c_iflag &= ~(IXON | IXOFF | IXANY);
			tty.c_lflag |= ICANON;
			tty.c_oflag &= ~OPOST;
			tcsetattr(led->fd, TCSANOW, &tty);
			tcsetattr(led->fd, TCSAFLUSH, &tty);
			char tempbuf[32];
			int n = read (led->fd, tempbuf, sizeof(tempbuf)-1);
			GST_DEBUG_OBJECT(led, "read %i bytes: '%s'", n, tempbuf);
			if (g_ascii_strncasecmp ("Photobooth-LED ready", tempbuf, 20) == 0)
			{
				GST_DEBUG_OBJECT (led, "initialized!");
				photo_booth_led_black (led);
			}
			else
			{
				GST_WARNING_OBJECT (led, "wrong device!");
				close (led->fd);
				led->fd = -1;
			}
		} else {
			GST_WARNING_OBJECT(led, "tcgetatt() error %s (%i)", strerror(errno), errno);
		}
	}
}

static void photo_booth_led_finalize (GObject *object)
{
	PhotoBoothLed *led = PHOTO_BOOTH_LED (object);
	if (led->fd > 0)
	{
		photo_booth_led_black (led);
		close (led->fd);
	}
}

void photo_booth_led_black (PhotoBoothLed *led)
{
	if (led->fd > 0)
	{
		char cmd = LED_BLACK;
		GST_DEBUG_OBJECT(led, "turning off leds");
		write (led->fd, &cmd, 1);
	}
}

void photo_booth_led_countdown (PhotoBoothLed *led, gint seconds)
{
	if (led->fd > 0)
	{
		char cmd = LED_COUNTDOWN;
		GST_DEBUG_OBJECT(led, "countdown %i seconds", seconds);
		write (led->fd, &cmd, 1);
	}
}

void photo_booth_led_flash (PhotoBoothLed *led)
{
	if (led->fd > 0)
	{
		char cmd = LED_FLASH;
		GST_DEBUG_OBJECT(led, "flashing leds");
		write (led->fd, &cmd, 1);
	}
}

void photo_booth_led_printer (PhotoBoothLed *led, gint copies)
{
	if (led->fd > 0)
	{
		char *cmd = g_strdup_printf ("%c%d", LED_PRINT, copies);
		GST_DEBUG_OBJECT(led, "turning on printer leds '%s'", cmd);
		write (led->fd, &cmd, strlen(cmd));
		g_free (cmd);
	}
}

PhotoBoothLed * photo_booth_led_new ()
{
	return g_object_new (PHOTO_BOOTH_LED_TYPE, NULL);
}
