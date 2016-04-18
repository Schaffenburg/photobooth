#!/usr/bin/env python

import sys, os
import gi
gi.require_version('Gtk', '3.0')
gi.require_version('Gst', '1.0')
gi.require_version('GstVideo', '1.0')
gi.require_version('GdkX11', '3.0')
from gi.repository import Gst, GObject, Gtk, GdkX11, Gdk, GstVideo

class GTK_Main:
	def __init__(self):
		window = Gtk.Window(Gtk.WindowType.TOPLEVEL)
		window.set_title("Live Video Preview")
		window.set_default_size(1024, 830)
		window.connect("destroy", Gtk.main_quit, "WM destroy")
		Gtk.Window.fullscreen(window)
		vbox = Gtk.VBox()
		window.add(vbox)
		self.movie_window = Gtk.DrawingArea()
		self.movie_window.add_events(Gdk.EventMask.BUTTON_PRESS_MASK)
		self.movie_window.connect("button-press-event", self.snapshot)
		vbox.add(self.movie_window)

		hbox = Gtk.HBox()
		vbox.pack_start(hbox, False, False, 0)
		hbox.set_border_width(10)
		hbox.pack_start(Gtk.Label(), False, False, 0)
		self.button = Gtk.Button("Start")
		self.button.connect("clicked", self.start_stop)
		hbox.pack_start(self.button, False, False, 0)
		self.button2 = Gtk.Button("Quit")
		self.button2.connect("clicked", self.exit)
		hbox.pack_start(self.button2, False, False, 0)
		hbox.add(Gtk.Label())
		window.show_all()

		self.player = Gst.Pipeline.new("player")
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
			self.player.add(ele)
			
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

		bus = self.player.get_bus()
		bus.add_signal_watch()
		bus.enable_sync_message_emission()
		bus.connect("message", self.on_message)
		bus.connect("sync-message::element", self.on_sync_message)

	def start_stop(self, w):
		if self.button.get_label() == "Start":
			self.xid = self.movie_window.get_property('window').get_xid()
			self.button.set_label("Stop")
			self.player.set_state(Gst.State.PLAYING)
		else:
			self.player.set_state(Gst.State.NULL)
			self.button.set_label("Start")

	def snapshot(self, widget, event):
		if event.button == 1:
			print "TAKE SNAPSHOT!"
			
	def exit(self, widget, data=None):
		Gtk.main_quit()

	def on_message(self, bus, message):
		t = message.type
		if t == Gst.MessageType.EOS:
			self.player.set_state(Gst.State.NULL)
			self.button.set_label("Start")
		elif t == Gst.MessageType.ERROR:
			err, debug = message.parse_error()
			print "Error: %s" % err, debug
			self.player.set_state(Gst.State.NULL)
			self.button.set_label("Start")

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
