#!/usr/bin/env python

import sys, os
import gi
gi.require_version('Gtk', '3.0')
gi.require_version('Gst', '1.0')
gi.require_version('GstVideo', '1.0')
gi.require_version('GdkX11', '3.0')
from gi.repository import Gst, GObject, Gtk, GdkX11, Gdk, GstVideo

COUNTDOWN_SECONDS   = 4
AUDIOFILE_COUNTDOWN = "beep.m4a"
AUDIOFILE_SHUTTER   = "shutter.m4a"


class GTK_Main:
	def __init__(self):
		window = Gtk.Window(Gtk.WindowType.TOPLEVEL)
		window.set_title("Live Video Preview")
		window.set_default_size(1024, 830)
		window.connect("destroy", Gtk.main_quit, "WM destroy")
		Gtk.Window.fullscreen(window)

		style_provider = Gtk.CssProvider()
		css = open('photobooth.css')
		css_data = css.read()
		css.close()
		style_provider.load_from_data(css_data)
		Gtk.StyleContext.add_provider_for_screen(
			Gdk.Screen.get_default(),
			style_provider,
			Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION
		)

		self.overlay = Gtk.Overlay()
		window.add(self.overlay)

		self.movie_window = Gtk.DrawingArea()
		self.movie_window.add_events(Gdk.EventMask.BUTTON_PRESS_MASK)
		self.movie_window.connect("button-press-event", self.snapshot)
		self.overlay.add(self.movie_window)

		vbox = Gtk.VBox()
		hbox = Gtk.HBox()
		vbox.pack_start(hbox, False, False, 0)
		hbox.set_border_width(10)
		hbox.pack_start(Gtk.Label(), False, False, 0)
		self.button_play = Gtk.Button("Start")
		self.button_play.connect("clicked", self.start_stop)
		hbox.pack_start(self.button_play, False, False, 0)
		self.button_quit = Gtk.Button("Quit")
		self.button_quit.connect("clicked", self.exit)
		hbox.pack_start(self.button_quit, False, False, 0)

		vbox.set_valign(Gtk.Align.END)
		vbox.set_halign(Gtk.Align.CENTER)
		self.overlay.add_overlay(vbox)

		self.overlay.show_all()
		window.show_all()

		self.videoplayer = Gst.Pipeline.new("player")
		source = Gst.ElementFactory.make("videotestsrc", "file-source")
		source.set_property("pattern", 1)
		capsfilter = Gst.ElementFactory.make("capsfilter", "capsfilter")
		caps = Gst.Caps.from_string("video/x-raw, width=1024, height=768")
		capsfilter.set_property("caps", caps)
		video_convert = Gst.ElementFactory.make("videoconvert", "video_convert")

		png_source = Gst.ElementFactory.make("filesrc", "png-source")
		png_source.set_property("location", os.path.realpath("overlay.png"))
		png_decoder = Gst.ElementFactory.make("pngdec", "png-decoder")
		alphacolor = Gst.ElementFactory.make("alphacolor", "alphacolor")
		png_convert = Gst.ElementFactory.make("videoconvert", "png_convert")
		imagefreeze = Gst.ElementFactory.make("imagefreeze", "imagefreeze")

		mixer = Gst.ElementFactory.make("videomixer", "mixer")
		mix_convert = Gst.ElementFactory.make("videoconvert", "mix_convert")
		videosink = Gst.ElementFactory.make("xvimagesink", "video-output")

		for ele in (source, capsfilter, video_convert, png_source, png_decoder, alphacolor, png_convert, imagefreeze, mixer, mix_convert, videosink):
			self.videoplayer.add(ele)
			
		source.link(capsfilter)
		capsfilter.link(video_convert)
		video_convert.link(mixer)

		png_source.link(png_decoder)
		png_decoder.link(alphacolor)
		alphacolor.link(png_convert)
		png_convert.link(imagefreeze)
		imagefreeze.link(mixer)

		mixer.link(mix_convert)
		mix_convert.link(videosink)

		bus = self.videoplayer.get_bus()
		bus.add_signal_watch()
		bus.enable_sync_message_emission()
		bus.connect("message", self.on_message)
		bus.connect("sync-message::element", self.on_sync_message)
	
		self.countdown_timer = None
		self.soundplayer = Gst.Pipeline.new("soundplayer")
		self.audioplaybin = Gst.ElementFactory.make("playbin", "soundfile")

	def start_stop(self, w):
		if self.button_play.get_label() == "Start":
			self.xid = self.movie_window.get_property('window').get_xid()
			self.button_play.set_label("Stop")
			self.videoplayer.set_state(Gst.State.PLAYING)
		else:
			self.videoplayer.set_state(Gst.State.NULL)
			self.button_play.set_label("Start")

	def snapshot(self, widget, event):
		if event.button == 1 and self.videoplayer.get_state(0).state == Gst.State.PLAYING and not self.countdown_timer:
			self.countdown_timer = GObject.timeout_add(1000, self.countdown_cb)
			self.snapshot_label = Gtk.Label(label="SNAPSHOT!", angle=25, halign=Gtk.Align.END)
			self.snapshot_label.set_valign(Gtk.Align.CENTER)
			self.snapshot_label.set_halign(Gtk.Align.CENTER)
			self.snapshot_label.set_size_request(500, 300)
			self.snapshot_label.get_style_context().add_class("snapshot_label")
			uri = 'file://'+os.path.abspath(AUDIOFILE_COUNTDOWN)
			self.audioplaybin.set_property('uri',uri)
			self.soundplayer.add(self.audioplaybin)
			self.soundplayer.set_state(Gst.State.PLAYING)
			self.overlay.add_overlay(self.snapshot_label)
			self.overlay.show_all()
			self.counter = COUNTDOWN_SECONDS

	def countdown_cb(self):
		self.counter -= 1
		if self.counter == 0:
			self.snap()
			return False
		self.snapshot_label.set_text("SNAP IN %i" % self.counter)
		return True

	def snap(self):
		print "TAKE SNAPSHOT!"
		self.countdown_timer = None
		self.soundplayer.set_state(Gst.State.READY)
		uri = 'file://'+os.path.abspath(AUDIOFILE_SHUTTER)
		self.audioplaybin.set_property('uri',uri)
		self.soundplayer.set_state(Gst.State.PLAYING)
		self.overlay.remove(self.snapshot_label)

	def exit(self, widget, data=None):
		Gtk.main_quit()

	def on_message(self, bus, message):
		t = message.type
		if t == Gst.MessageType.EOS:
			self.videoplayer.set_state(Gst.State.NULL)
			self.button_play.set_label("Start")
		elif t == Gst.MessageType.ERROR:
			err, debug = message.parse_error()
			print "Error: %s" % err, debug
			self.videoplayer.set_state(Gst.State.NULL)
			self.button_play.set_label("Start")

	def on_sync_message(self, bus, message):
		struct = message.get_structure()
		if not struct:
			return
		message_name = struct.get_name()
		if message_name == "prepare-window-handle":
			message.src.set_window_handle(self.xid)

Gst.init(None)
GTK_Main()
GObject.threads_init()
Gtk.main()
