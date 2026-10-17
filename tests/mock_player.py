# SPDX-License-Identifier: GPL-2.0-or-later
"""An MPRIS media player for tests.

Owns org.mpris.MediaPlayer2.pwairplaytest, plays a quiet sine wave to the
given PipeWire sink with a pw-play child process while "Playing" (so the
stream belongs to this process tree, as with a real player), and records the
methods called on it to a JSON lines file. Control from the test runner goes
through a fifo: lines like "play", "pause", "stop", "track <title>",
"target <sink node name>", "huge" (oversized metadata).
"""

import argparse
import json
import math
import os
import struct
import subprocess
import wave

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

BUS_NAME = "org.mpris.MediaPlayer2.pwairplaytest"
PATH = "/org/mpris/MediaPlayer2"

XML = """
<node>
  <interface name="org.mpris.MediaPlayer2">
    <method name="Raise"/><method name="Quit"/>
    <property name="Identity" type="s" access="read"/>
    <property name="DesktopEntry" type="s" access="read"/>
    <property name="CanQuit" type="b" access="read"/>
    <property name="CanRaise" type="b" access="read"/>
    <property name="HasTrackList" type="b" access="read"/>
    <property name="SupportedUriSchemes" type="as" access="read"/>
    <property name="SupportedMimeTypes" type="as" access="read"/>
  </interface>
  <interface name="org.mpris.MediaPlayer2.Player">
    <method name="Next"/><method name="Previous"/><method name="Pause"/>
    <method name="PlayPause"/><method name="Stop"/><method name="Play"/>
    <method name="Seek"><arg name="Offset" type="x" direction="in"/></method>
    <signal name="Seeked"><arg name="Position" type="x"/></signal>
    <property name="PlaybackStatus" type="s" access="read"/>
    <property name="Metadata" type="a{sv}" access="read"/>
    <property name="Position" type="x" access="read"/>
    <property name="CanGoNext" type="b" access="read"/>
    <property name="CanGoPrevious" type="b" access="read"/>
    <property name="CanPlay" type="b" access="read"/>
    <property name="CanPause" type="b" access="read"/>
    <property name="CanSeek" type="b" access="read"/>
    <property name="CanControl" type="b" access="read"/>
  </interface>
</node>
"""


def make_sine(path, seconds=600, amplitude=0.05):
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(44100)
        frames = bytearray()
        for i in range(44100):
            v = int(amplitude * 32767 * math.sin(2 * math.pi * 441 * i / 44100))
            frames += struct.pack("<hh", v, v)
        for _ in range(seconds):
            w.writeframes(frames)


class Player:
    def __init__(self, args):
        self.args = args
        self.status = "Stopped"
        self.title = "Test title"
        self.artist = "Test artist"
        self.extra = {}
        self.target = None
        self.child = None
        self.conn = None
        self.started = 0
        self.wav = os.path.join(args.work_dir, "sine.wav")
        self.art = os.path.join(args.work_dir, "cover.jpg")
        make_sine(self.wav)
        with open(self.art, "wb") as f:
            # Only the JPEG signature matters to pw-airplayd
            f.write(b"\xff\xd8\xff\xe0" + b"\0" * 2000 + b"\xff\xd9")

    def record(self, name):
        with open(self.args.calls, "a") as f:
            f.write(json.dumps({"call": name, "status": self.status}) + "\n")

    def metadata(self):
        md = {
            "mpris:trackid": GLib.Variant("o", "/test/track/" + str(abs(hash(self.title)) % 100000)),
            "xesam:title": GLib.Variant("s", self.title),
            "xesam:artist": GLib.Variant("as", [self.artist]),
            "xesam:album": GLib.Variant("s", "Test album"),
            "mpris:length": GLib.Variant("x", 180 * 1000000),
            "mpris:artUrl": GLib.Variant("s", "file://" + self.art),
        }
        md.update(self.extra)
        return md

    def props(self, iface):
        if iface == "org.mpris.MediaPlayer2":
            return {
                "Identity": GLib.Variant("s", "pw-airplay test player"),
                "DesktopEntry": GLib.Variant("s", "pwairplaytest"),
                "CanQuit": GLib.Variant("b", False),
                "CanRaise": GLib.Variant("b", False),
                "HasTrackList": GLib.Variant("b", False),
                "SupportedUriSchemes": GLib.Variant("as", []),
                "SupportedMimeTypes": GLib.Variant("as", []),
            }
        position = int((GLib.get_monotonic_time() - self.started)) if self.status == "Playing" else 0
        return {
            "PlaybackStatus": GLib.Variant("s", self.status),
            "Metadata": GLib.Variant("a{sv}", self.metadata()),
            "Position": GLib.Variant("x", position),
            "CanGoNext": GLib.Variant("b", True),
            "CanGoPrevious": GLib.Variant("b", True),
            "CanPlay": GLib.Variant("b", True),
            "CanPause": GLib.Variant("b", True),
            "CanSeek": GLib.Variant("b", False),
            "CanControl": GLib.Variant("b", True),
        }

    def changed(self):
        props = self.props("org.mpris.MediaPlayer2.Player")
        self.conn.emit_signal(None, PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                              GLib.Variant("(sa{sv}as)", ("org.mpris.MediaPlayer2.Player",
                                                          {k: props[k] for k in ("PlaybackStatus", "Metadata")},
                                                          [])))

    def set_status(self, status):
        if status == "Playing" and not self.child and self.target:
            self.child = subprocess.Popen(["pw-play", "--target", self.target, self.wav])
            self.started = GLib.get_monotonic_time()
        if status != "Playing" and self.child:
            self.child.terminate()
            self.child.wait()
            self.child = None
        self.status = status
        self.changed()

    def on_call(self, conn, sender, path, iface, method, params, invocation):
        self.record(method)
        if method in ("Play",):
            self.set_status("Playing")
        elif method in ("Pause", "Stop"):
            self.set_status("Paused" if method == "Pause" else "Stopped")
        elif method == "PlayPause":
            self.set_status("Paused" if self.status == "Playing" else "Playing")
        elif method in ("Next", "Previous"):
            self.title = method + " track"
            self.changed()
        invocation.return_value(None)

    def on_get(self, conn, sender, path, iface, prop):
        return self.props(iface)[prop]

    def on_control(self, channel, condition):
        line = channel.readline()
        if not line:
            return True
        cmd, _, arg = line.strip().partition(" ")
        if cmd == "target":
            self.target = arg
        elif cmd == "play":
            self.set_status("Playing")
        elif cmd == "pause":
            self.set_status("Paused")
        elif cmd == "stop":
            self.set_status("Stopped")
        elif cmd == "track":
            self.title = arg
            self.changed()
        elif cmd == "huge":
            self.title = "x" * 100000
            self.extra = {"xesam:artist": GLib.Variant("s", "not a list"),
                          "mpris:length": GLib.Variant("d", -1.0),
                          "mpris:artUrl": GLib.Variant("s", "file:///nonexistent/cover.jpg"),
                          "xesam:weird": GLib.Variant("a{sv}", {"k": GLib.Variant("i", 1)})}
            self.changed()
        return True

    def run(self):
        node = Gio.DBusNodeInfo.new_for_xml(XML)

        def on_bus(conn, name):
            self.conn = conn
            for iface in node.interfaces:
                conn.register_object(PATH, iface, self.on_call, self.on_get, None)

        Gio.bus_own_name(Gio.BusType.SESSION, BUS_NAME, Gio.BusNameOwnerFlags.NONE, on_bus, None, None)

        os.mkfifo(self.args.control)
        fd = os.open(self.args.control, os.O_RDWR | os.O_NONBLOCK)
        channel = GLib.IOChannel.unix_new(fd)
        GLib.io_add_watch(channel, GLib.PRIORITY_DEFAULT, GLib.IOCondition.IN, self.on_control)
        try:
            GLib.MainLoop().run()
        finally:
            if self.child:
                self.child.terminate()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--work-dir", required=True)
    parser.add_argument("--control", required=True)
    parser.add_argument("--calls", required=True)
    Player(parser.parse_args()).run()


if __name__ == "__main__":
    main()
