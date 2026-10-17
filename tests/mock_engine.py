# SPDX-License-Identifier: GPL-2.0-or-later
"""Stand-in for the OwnTone engine, for tests without speakers.

Serves the subset of OwnTone's JSON API pw-airplayd uses, reads the audio
pipe like OwnTone does (autostart on data, autostop on EOF) while measuring
its byte rate, reads the metadata pipe, and can inject remote commands the
way the patched OwnTone forwards them. Test hooks live under /test/.
"""

import argparse
import json
import os
import re
import select
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

FRAME_BYTES = 4
RATE = 44100


class Engine:
    def __init__(self, data_dir):
        self.lock = threading.Lock()
        self.pipe = os.path.join(data_dir, "pipes", "pw-airplay")
        self.metadata_pipe = self.pipe + ".metadata"
        self.remote_fifo = os.path.join(data_dir, "control", "remote")
        self.outputs = {
            "testa": {"name": "pw-airplay test A", "volume": 40, "selected": False},
            "testb": {"name": "pw-airplay test B", "volume": 60, "selected": False},
        }
        self.state = "stop"
        self.calls = []          # (method, path, body)
        self.audio_bytes = 0
        self.audio_nonzero = 0
        self.samples = []        # (monotonic time, total bytes), while playing
        self.metadata = []       # (code, value or length)
        self.metadata_reads = 0
        self.eofs = 0

    def log(self, method, path, body):
        with self.lock:
            self.calls.append((method, path, body))

    def stats(self):
        with self.lock:
            return {
                "state": self.state,
                "calls": self.calls,
                "audio_bytes": self.audio_bytes,
                "audio_nonzero": self.audio_nonzero,
                "samples": self.samples[-2000:],
                "metadata": self.metadata,
                "metadata_reads": self.metadata_reads,
                "eofs": self.eofs,
                "outputs": self.outputs,
            }

    def wait_pipe(self, path):
        while not os.path.exists(path):
            time.sleep(0.1)

    def audio_reader(self):
        """Keeps the pipe open for reading, like OwnTone's pipe watcher."""
        self.wait_pipe(self.pipe)
        fd = os.open(self.pipe, os.O_RDONLY | os.O_NONBLOCK)
        poller = select.poll()
        poller.register(fd, select.POLLIN)
        writer_seen = False
        while True:
            poller.poll(50)
            try:
                data = os.read(fd, 65536)
            except BlockingIOError:
                continue
            now = time.monotonic()
            with self.lock:
                if data:
                    writer_seen = True
                    if self.state == "stop" and any(o["selected"] for o in self.outputs.values()):
                        self.state = "play"
                        threading.Thread(target=self.metadata_reader, daemon=True).start()
                    self.audio_bytes += len(data)
                    self.audio_nonzero += len(data) - data.count(0)
                    self.samples.append((now, self.audio_bytes))
                elif writer_seen:
                    # EOF: the writer closed, OwnTone autostops
                    writer_seen = False
                    self.eofs += 1
                    self.state = "stop"
            if not data:
                time.sleep(0.01)

    def metadata_reader(self):
        """OwnTone opens the metadata pipe once it plays the audio pipe."""
        self.wait_pipe(self.metadata_pipe)
        fd = os.open(self.metadata_pipe, os.O_RDONLY | os.O_NONBLOCK)
        buf = b""
        try:
            while True:
                with self.lock:
                    if self.state == "stop":
                        return
                try:
                    data = os.read(fd, 1 << 20)
                except BlockingIOError:
                    data = b""
                if not data:
                    time.sleep(0.05)
                    continue
                with self.lock:
                    self.metadata_reads += 1
                buf += data
                while b"</item>" in buf:
                    item, buf = buf.split(b"</item>", 1)
                    self.parse_item(item.decode("ascii", "replace"))
        finally:
            os.close(fd)

    def parse_item(self, item):
        import base64

        code = re.search(r"<code>([0-9a-f]{8})</code>", item)
        data = re.search(r'<data encoding="base64">\s*(.*?)</data>', item, re.S)
        if not code:
            return
        name = bytes.fromhex(code.group(1)).decode("ascii", "replace")
        raw = base64.b64decode(data.group(1)) if data else b""
        value = len(raw) if name == "PICT" else raw.decode("utf-8", "replace")
        with self.lock:
            self.metadata.append((name, value))

    def remote(self, command):
        fd = os.open(self.remote_fifo, os.O_WRONLY | os.O_NONBLOCK)
        os.write(fd, command.encode() + b"\n")
        os.close(fd)


class Handler(BaseHTTPRequestHandler):
    engine = None

    def log_message(self, *args):
        pass

    def reply(self, code, obj=None):
        body = json.dumps(obj).encode() if obj is not None else b""
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def body(self):
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length) if length else b""
        return json.loads(raw) if raw else None

    def do_GET(self):
        e = self.engine
        if self.path == "/api/outputs":
            with e.lock:
                outs = [{"id": i, "name": o["name"], "type": "AirPlay 2", "selected": o["selected"],
                         "volume": o["volume"]} for i, o in e.outputs.items()]
            return self.reply(200, {"outputs": outs})
        if self.path == "/api/player":
            with e.lock:
                return self.reply(200, {"state": e.state})
        if self.path == "/test/stats":
            return self.reply(200, e.stats())
        self.reply(404)

    def do_PUT(self):
        e = self.engine
        body = self.body()
        e.log("PUT", self.path, body)
        with e.lock:
            if self.path == "/api/outputs/set":
                ids = body.get("outputs", [])
                for i, o in e.outputs.items():
                    o["selected"] = i in ids
                return self.reply(204)
            m = re.fullmatch(r"/api/outputs/(\w+)", self.path)
            if m and m.group(1) in e.outputs:
                e.outputs[m.group(1)].update(body or {})
                return self.reply(204)
            if self.path == "/api/player/pause" and e.state == "play":
                e.state = "pause"
                return self.reply(204)
            if self.path == "/api/player/play" and e.state == "pause":
                e.state = "play"
                return self.reply(204)
            if self.path == "/api/player/stop":
                e.state = "stop"
                return self.reply(204)
            if self.path.startswith("/api/player/"):
                return self.reply(204)
        self.reply(404)

    def do_POST(self):
        e = self.engine
        body = self.body() or {}
        if self.path == "/test/remote":
            e.remote(body["command"])
            return self.reply(204)
        if self.path == "/test/output":
            with e.lock:
                if body.get("remove"):
                    e.outputs.pop(body["id"], None)
                else:
                    e.outputs.setdefault(body["id"], {"selected": False}).update(body.get("set", {}))
            return self.reply(204)
        self.reply(404)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--data-dir", required=True)
    args = parser.parse_args()

    engine = Engine(args.data_dir)
    threading.Thread(target=engine.audio_reader, daemon=True).start()
    Handler.engine = engine
    ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
