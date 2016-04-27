#!/usr/bin/env python

import sys, os
import tempfile, subprocess
import gi
gi.require_version('Gtk', '3.0')
gi.require_version('Gst', '1.0')
gi.require_version('GstVideo', '1.0')
gi.require_version('GdkX11', '3.0')
from gi.repository import Gst, GObject, Gtk, GdkX11, Gdk, GstVideo

COUNTDOWN_SECONDS   = 3
AUDIOFILE_COUNTDOWN = "beep.m4a"
AUDIOFILE_SHUTTER   = "shutter.m4a"

#Gst.debug_set_active(True)
#Gst.debug_set_default_threshold(3)

class GTK_Main:
	def __init__(self):
		cmd = ['gphoto2', '--set-config=/main/capturesettings/imagequality=1']
		ret = subprocess.call(cmd)
		if ret != 0:
			exit()

		self.moviecapture = None
		tmpdir = tempfile.mkdtemp()
		self.video_pipe_name = os.path.join(tmpdir, 'videopreview')
		os.mkfifo(self.video_pipe_name)

		window = Gtk.Window(Gtk.WindowType.TOPLEVEL)
		window.set_title("Live Video Preview")
		window.set_default_size(1024, 768)
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

		self.gst_setup_live_preview()

		self.countdown_timer = None
		self.audioplayer = Gst.Pipeline.new("audioplayer")
		self.audioplaybin = Gst.ElementFactory.make("playbin", "audioplaybin")
		self.audioplayer.add(self.audioplaybin)

	def gst_setup_live_preview(self):
		self.videoplayer = Gst.Pipeline.new("player")
		source = Gst.ElementFactory.make("filesrc", "filesrc")
		source.set_property("location", self.video_pipe_name)
		mjpeg_filter = Gst.ElementFactory.make("capsfilter", "mjpeg_capsfilter")
		caps = Gst.Caps.from_string("image/jpeg, width=640, height=424, framerate=30/1, pixel-aspect-ratio=1/1")
		mjpeg_filter.set_property("caps", caps)

		jpegdec = Gst.ElementFactory.make("jpegdec", "jpegdec")
		video_scale = Gst.ElementFactory.make("videoscale", "videoscale")
		video_convert = Gst.ElementFactory.make("videoconvert", "video_convert")
		capsfilter = Gst.ElementFactory.make("capsfilter", "capsfilter")
		caps = Gst.Caps.from_string("video/x-raw, width=1076, height=708")
		capsfilter.set_property("caps", caps)

		png_source = Gst.ElementFactory.make("filesrc", "png-source")
		png_source.set_property("location", os.path.realpath("overlay.png"))
		png_decoder = Gst.ElementFactory.make("pngdec", "png-decoder")
		alphacolor = Gst.ElementFactory.make("alphacolor", "alphacolor")
		png_convert = Gst.ElementFactory.make("videoconvert", "png_convert")
		imagefreeze = Gst.ElementFactory.make("imagefreeze", "imagefreeze")

		mixer = Gst.ElementFactory.make("videomixer", "mixer")
		mix_convert = Gst.ElementFactory.make("videoconvert", "mix_convert")
		videosink = Gst.ElementFactory.make("xvimagesink", "video-output")

		for ele in (source, jpegdec, mjpeg_filter, capsfilter, video_scale, video_convert, png_source, png_decoder, alphacolor, png_convert, imagefreeze, mixer, mix_convert, videosink):
			self.videoplayer.add(ele)

		source.link(mjpeg_filter)
		mjpeg_filter.link(jpegdec)
		jpegdec.link(video_scale)
		video_scale.link(video_convert)
		video_convert.link(capsfilter)
		capsfilter.link(mixer)

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

	def gst_setup_snapshot_preview(self):
		self.videoplayer = Gst.Pipeline.new("player")
		source = Gst.ElementFactory.make("filesrc", "filesrc")
		source.set_property("location", "print.jpg")
		jpegdec = Gst.ElementFactory.make("jpegdec", "jpegdec")
		imagefreeze = Gst.ElementFactory.make("imagefreeze", "imagefreeze")
		video_scale = Gst.ElementFactory.make("videoscale", "videoscale")
		video_convert = Gst.ElementFactory.make("videoconvert", "video_convert")
		capsfilter = Gst.ElementFactory.make("capsfilter", "capsfilter")
		caps = Gst.Caps.from_string("video/x-raw, width=1076, height=708")
		capsfilter.set_property("caps", caps)
		videosink = Gst.ElementFactory.make("xvimagesink", "video-output")
		for ele in (source, jpegdec, imagefreeze, capsfilter, video_scale, video_convert, videosink):
			self.videoplayer.add(ele)
		source.link(jpegdec)
		jpegdec.link(imagefreeze)
		imagefreeze.link(video_scale)
		video_scale.link(video_convert)
		video_convert.link(capsfilter)
		capsfilter.link(videosink)
		self.videoplayer.set_state(Gst.State.PLAYING)
		bus = self.videoplayer.get_bus()
		bus.add_signal_watch()
		bus.enable_sync_message_emission()
		bus.connect("message", self.on_message)
		bus.connect("sync-message::element", self.on_sync_message)

	def start_stop(self, w):
		if self.button_play.get_label() == "Start":
			self.xid = self.movie_window.get_property('window').get_xid()
			self.button_play.set_label("Stop")
			self.gst_setup_live_preview()
			if not self.moviecapture:
				video_pipe = open(self.video_pipe_name,"w+")
				print "video_pipe", video_pipe
				cmd = ['gphoto2', '--capture-movie', '--stdout']
				self.moviecapture = subprocess.Popen(cmd, stdout=video_pipe, stderr=subprocess.PIPE)
			self.videoplayer.set_state(Gst.State.PLAYING)
		else:
			print "STOPPING..."
			self.videoplayer.set_state(Gst.State.NULL)
			GObject.timeout_add(10, self.stop_capture)
			self.button_play.set_label("Start")

	def snapshot(self, widget, event):
		if event.button == 1 and self.videoplayer.get_state(0).state == Gst.State.PLAYING and not self.countdown_timer:
			self.countdown_timer = GObject.timeout_add(100, self.countdown_cb)
			self.snapshot_label = Gtk.Label(label="SNAPSHOT!", angle=25, halign=Gtk.Align.END)
			self.snapshot_label.set_valign(Gtk.Align.CENTER)
			self.snapshot_label.set_halign(Gtk.Align.CENTER)
			self.snapshot_label.set_size_request(500, 300)
			self.snapshot_label.get_style_context().add_class("snapshot_label")
			uri = 'file://'+os.path.abspath(AUDIOFILE_COUNTDOWN)
			self.audioplaybin.set_property('uri',uri)
			self.audioplayer.set_state(Gst.State.PLAYING)
			self.overlay.add_overlay(self.snapshot_label)
			self.overlay.show_all()
			self.counter = COUNTDOWN_SECONDS*10

	def countdown_cb(self):
		self.counter -= 1
		self.snapshot_label.set_text("SNAP IN %i" % self.counter)
		if self.counter == 0:
			self.countdown_timer = None
			self.overlay.remove(self.snapshot_label)
			self.snap()
			return False
		return True

	def snap(self):
		print "TAKE SNAPSHOT!", self.counter
		self.moviecapture.terminate()
		out,err = self.moviecapture.communicate()
		self.moviecapture = None
		print "video capture terminated", out, err, self.counter
		cmd = ['gphoto2', '--capture-image-and-download', '--force-overwrite']
		photocapture = subprocess.Popen(cmd)
		print "snashop taken", self.counter, out, err
		self.audioplayer.set_state(Gst.State.READY)
		uri = 'file://'+os.path.abspath(AUDIOFILE_SHUTTER)
		self.audioplaybin.set_property('uri',uri)
		self.audioplayer.set_state(Gst.State.PLAYING)
		out,err = photocapture.communicate()
		self.videoplayer.set_state(Gst.State.NULL)
		cmd = ['gm', 'mogrify', '-resize', '2152x1417!', 'capt0000.jpg']
		print "execute", cmd
		subprocess.call(cmd)
		cmd = ['gm', 'composite', '-compose', 'Over', 'overlay_print.png', 'capt0000.jpg', 'print.jpg']
		print "execute", cmd
		subprocess.call(cmd)
		self.audioplayer.set_state(Gst.State.READY)
		self.gst_setup_snapshot_preview()

	def exit(self, widget=None, data=None):
		print "EXIT"
		self.stop_capture()
		os.unlink(self.video_pipe_name)
		Gtk.main_quit()

	def stop_capture(self):
		if self.moviecapture:
			print "stop_capture...", self.moviecapture
			self.moviecapture.terminate()
			print "terminated...", self.moviecapture
			video_pipe = open(self.video_pipe_name,"w+")
			print "video_pipe=", video_pipe
			#self.moviecapture.communicate()
			bla = video_pipe.read(1)
			print "bla=", bla
			self.moviecapture = None
		return False

	def on_message(self, bus, message):
		t = message.type
		if t == Gst.MessageType.EOS:
			print "EOS!"
			self.videoplayer.set_state(Gst.State.NULL)
			self.button_play.set_label("Start")
		elif t == Gst.MessageType.ERROR:
			err, debug = message.parse_error()
			print "Error: %s" % err, debug
			self.videoplayer.set_state(Gst.State.NULL)
			self.button_play.set_label("Start")
		#else:
			#print "on_message", message, message.type

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
