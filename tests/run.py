# SPDX-License-Identifier: GPL-2.0-or-later
"""End to end tests of pw-airplayd, without speakers.

Builds pw-airplayd with AddressSanitizer and UndefinedBehaviorSanitizer, runs
it against mock_engine.py (in place of OwnTone) and mock_player.py (an MPRIS
player), and exercises it through PipeWire. Needs a running PipeWire session.
Audio only goes to the test sinks, nothing reaches real speakers.

Usage: python3 -I tests/run.py [--keep] [--cycles N]
"""

import argparse
import glob
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
PORT = 3799
RATE_BYTES = 44100 * 4
TEST_SINKS = ("airplay2_sink.testa", "airplay2_sink.testb", "airplay2_sink.all")
SAN_FLAGS = "-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined"

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(("PASS " if ok else "FAIL ") + name + (f"  ({detail})" if detail else ""), flush=True)
    return ok


def wait_for(fn, timeout, interval=0.2):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = fn()
        if value:
            return value
        time.sleep(interval)
    return fn()


class Env:
    def __init__(self, args):
        self.args = args
        self.dir = tempfile.mkdtemp(prefix="pw-airplay-test-")
        self.data = os.path.join(self.dir, "data")
        self.procs = {}

    def path(self, *p):
        return os.path.join(self.dir, *p)

    def build(self, sanitize=True):
        build = self.path("build-san" if sanitize else "build")
        shutil.copytree(os.path.join(REPO, "src"), os.path.join(build, "src"))
        shutil.copy(os.path.join(REPO, "Makefile"), build)
        flags = [f"CFLAGS={SAN_FLAGS}", "LDFLAGS=-fsanitize=address,undefined"] if sanitize else []
        subprocess.run(["make", "-s", "-C", build] + flags, check=True)
        return os.path.join(build, "pw-airplayd")

    def start(self, name, cmd, env=None, log=True):
        out = open(self.path(name + ".log"), "w") if log else subprocess.DEVNULL
        self.procs[name] = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT,
                                            env={**os.environ, **(env or {})})

    def stop(self, name, sig=signal.SIGTERM, timeout=15):
        p = self.procs.pop(name, None)
        if not p:
            return None
        p.send_signal(sig)
        try:
            return p.wait(timeout)
        except subprocess.TimeoutExpired:
            p.kill()
            return p.wait()

    def cleanup(self):
        for name in list(self.procs):
            self.stop(name)
        if not self.args.keep:
            shutil.rmtree(self.dir, ignore_errors=True)
        else:
            print("kept", self.dir)


def http(method, path, body=None):
    req = urllib.request.Request(f"http://127.0.0.1:{PORT}{path}", method=method,
                                 data=json.dumps(body).encode() if body is not None else None)
    with urllib.request.urlopen(req, timeout=5) as r:
        data = r.read()
        return json.loads(data) if data else None


def stats():
    return http("GET", "/test/stats")


def calls(method, path):
    return [c for c in stats()["calls"] if c[0] == method and c[1] == path]


def player(env, line):
    with open(env.path("player.ctl"), "w") as f:
        f.write(line + "\n")


def player_calls(env):
    try:
        with open(env.path("player-calls.jsonl")) as f:
            return [json.loads(l)["call"] for l in f]
    except FileNotFoundError:
        return []


def pw_nodes():
    dump = json.loads(subprocess.run(["pw-dump"], capture_output=True, text=True).stdout)
    nodes = {}
    for o in dump:
        info = o.get("info") or {}
        props = info.get("props") or {}
        name = props.get("node.name", "")
        if o.get("type", "").endswith("Node") and name in TEST_SINKS:
            nodes[name] = (o["id"], info)
    return nodes


def daemon_pid(env):
    return env.procs["daemon"].pid


def daemon_resources(env, name="daemon"):
    pid = env.procs[name].pid
    with open(f"/proc/{pid}/status") as f:
        rss = next(int(l.split()[1]) for l in f if l.startswith("VmRSS"))
    fds = {}
    for fd in os.listdir(f"/proc/{pid}/fd"):
        try:
            fds[fd] = os.readlink(f"/proc/{pid}/fd/{fd}")
        except OSError:
            pass
    return rss, fds


def sink_volume(node_id):
    out = subprocess.run(["wpctl", "get-volume", str(node_id)], capture_output=True, text=True).stdout
    return float(out.split()[1]) if out.startswith("Volume:") else None


def play_to(env, node_name):
    player(env, f"target {node_name}")
    player(env, "play")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--keep", action="store_true", help="keep the work directory")
    parser.add_argument("--cycles", type=int, default=20, help="play/stop cycles for the endurance test")
    args = parser.parse_args()

    env = Env(args)
    try:
        run(env, args)
    finally:
        env.cleanup()

    failed = [r for r in results if not r[1]]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed")
    sys.exit(1 if failed else 0)


def run(env, args):
    binary = env.build()

    conf = env.path("pw-airplay.conf")
    with open(conf, "w") as f:
        f.write(f"api_port = {PORT}\nidle_timeout = 3\npause_timeout = 8\n"
                "all_speakers_name = pw-airplay test all\n")
    # The mock's output ids ("testa", "testb") become the sink node names
    # airplay2_sink.test*, distinct from a real installation's
    os.makedirs(env.data)

    env.start("engine", [sys.executable, "-I", os.path.join(HERE, "mock_engine.py"),
                         "--port", str(PORT), "--data-dir", os.path.join(env.data, "pw-airplay")])
    time.sleep(0.5)

    san_log = env.path("san")
    env.start("daemon", [binary, "-c", conf], env={
        "XDG_DATA_HOME": env.data,
        "PIPEWIRE_DEBUG": "2,default:3",
        "ASAN_OPTIONS": f"log_path={san_log}:detect_leaks=1:halt_on_error=0",
        "UBSAN_OPTIONS": f"log_path={san_log}:print_stacktrace=1:halt_on_error=0",
    })
    env.start("player", [sys.executable, "-I", os.path.join(HERE, "mock_player.py"),
                         "--work-dir", env.dir, "--control", env.path("player.ctl"),
                         "--calls", env.path("player-calls.jsonl")])

    # Discovery: two speakers, unknown to mDNS, appear after the grace delay,
    # plus the all speakers sink
    nodes = wait_for(lambda: len(pw_nodes()) == 3 and pw_nodes(), 12)
    check("sinks created for each speaker and all speakers", bool(nodes) and len(nodes) == 3,
          ", ".join(sorted(nodes)) if nodes else "none")
    if not nodes:
        return
    a_id, a_info = nodes["airplay2_sink.testa"]
    b_id, _ = nodes["airplay2_sink.testb"]

    # Latency reported to PipeWire
    expected_ms = int(subprocess.run([binary, "-c", conf, "--print-latency"], capture_output=True,
                                     text=True).stdout)
    lat = [p for p in (a_info.get("params") or {}).get("ProcessLatency", [])]
    ns = lat[0].get("ns") if lat else None
    check("process latency reported", ns == expected_ms * 1000000, f"{ns} ns, expected {expected_ms} ms")

    # Speaker volume mirrored on the sink. wpctl shows the cubic scale, as
    # volume sliders do, so 40% is 0.40
    vol = wait_for(lambda: (v := sink_volume(a_id)) is not None and abs(v - 0.40) < 0.011 and v, 5)
    check("speaker volume mirrored on the sink", bool(vol), f"{sink_volume(a_id)}")

    # Playback: speakers selected, audio paced at the nominal rate
    play_to(env, "airplay2_sink.testa")
    sel = wait_for(lambda: calls("PUT", "/api/outputs/set"), 5)
    check("outputs selected on playback", bool(sel) and sel[-1][2] == {"outputs": ["testa"]},
          str(sel[-1][2]) if sel else "no call")
    wait_for(lambda: stats()["state"] == "play", 5)
    time.sleep(1)
    s0 = stats()
    time.sleep(10)
    s1 = stats()
    rate = (s1["audio_bytes"] - s0["audio_bytes"]) / (s1["samples"][-1][0] - s0["samples"][-1][0])
    check("audio paced at 44100 Hz", abs(rate - RATE_BYTES) / RATE_BYTES < 0.005, f"{rate:.0f} B/s")
    check("audio reaches the engine", s1["audio_nonzero"] > RATE_BYTES, f"{s1['audio_nonzero']} non-zero bytes")

    # Pacing jitter: gaps between reads of the pipe
    samples = s1["samples"]
    gaps = [b[0] - a[0] for a, b in zip(samples, samples[1:]) if b[0] - a[0] > 0]
    check("no stall in the audio feed", max(gaps) < 0.1, f"max gap {max(gaps) * 1000:.0f} ms")

    # Now playing: one write with everything, once the engine opened the pipe
    md = wait_for(lambda: [m for m in stats()["metadata"] if m[0] == "minm"], 5)
    codes = [m[0] for m in stats()["metadata"]]
    check("now playing metadata sent", bool(md) and md[-1][1] == "Test title"
          and {"asar", "asal", "PICT", "prgr"} <= set(codes), ",".join(codes))
    check("artwork sent", any(m[0] == "PICT" and m[1] > 2000 for m in stats()["metadata"]))
    reads = stats()["metadata_reads"]
    player(env, "track Second title")
    wait_for(lambda: any(m == ["minm", "Second title"] for m in stats()["metadata"]), 5)
    time.sleep(0.5)
    check("track change sent in one write", stats()["metadata_reads"] - reads == 1,
          f"{stats()['metadata_reads'] - reads} reads")

    # Sink volume goes to the speaker: 0.70 on the slider -> 70%
    subprocess.run(["wpctl", "set-volume", str(a_id), "0.70"], check=True)
    v = wait_for(lambda: [c for c in calls("PUT", "/api/outputs/testa") if c[2] == {"volume": 70}], 5)
    check("sink volume sent to the speaker", bool(v))

    # Remote commands from the speakers reach the player
    for cmd, method in (("next", "Next"), ("prev", "Previous"), ("playpause", "PlayPause")):
        http("POST", "/test/remote", {"command": cmd})
        ok = wait_for(lambda: method in player_calls(env), 3)
        check(f"remote '{cmd}' -> MPRIS {method}", bool(ok))
    # PlayPause paused the player: AirPlay follows
    paused = wait_for(lambda: calls("PUT", "/api/player/pause"), 5)
    check("application pause pauses AirPlay", bool(paused))
    http("POST", "/test/remote", {"command": "playpause"})
    resumed = wait_for(lambda: calls("PUT", "/api/player/play"), 5)
    check("application resume resumes AirPlay", bool(resumed))

    # Garbage on the remote fifo, odd metadata
    with open(os.path.join(env.data, "pw-airplay", "control", "remote"), "wb") as f:
        f.write(b"\x00\xff" * 200 + b"x" * 1000 + b"\nnot-a-command\n\n")
    player(env, "huge")
    time.sleep(2)
    check("survives malformed input", env.procs["daemon"].poll() is None)
    player(env, "track Normal again")

    # Switching to another sink moves the speakers
    player(env, "stop")
    time.sleep(0.5)
    play_to(env, "airplay2_sink.testb")
    sw = wait_for(lambda: [c for c in calls("PUT", "/api/outputs/set") if c[2] == {"outputs": ["testb"]}], 6)
    check("switching sinks moves the speakers", bool(sw))

    # Idle release: pipe closed (engine sees EOF), speakers deselected
    player(env, "stop")
    eofs = stats()["eofs"]
    released = wait_for(lambda: stats()["eofs"] > eofs and calls("PUT", "/api/player/stop"), 8)
    check("speakers released after idle_timeout", bool(released))

    # Endurance: claim/release cycles must not leak descriptors (memory is
    # checked with a regular build below, the sanitizers hold on to it)
    def cycles(n, proc="daemon"):
        for i in range(n):
            play_to(env, "airplay2_sink.testa" if i % 2 else "airplay2_sink.testb")
            time.sleep(1.5)
            player(env, "stop")
            time.sleep(3.5)
        time.sleep(1)

    cycles(2)
    _, fds0 = daemon_resources(env)
    cycles(args.cycles)
    _, fds1 = daemon_resources(env)
    grown = sorted(set(fds1.values()) - set(fds0.values()))
    check(f"no fd leak over {args.cycles} play/release cycles", len(fds1) <= len(fds0),
          f"{len(fds0)} -> {len(fds1)}" + (f", new: {grown}" if grown else ""))

    # Engine going away and coming back
    env.stop("engine")
    time.sleep(3)
    check("survives the engine going away", env.procs["daemon"].poll() is None)
    env.start("engine", [sys.executable, "-I", os.path.join(HERE, "mock_engine.py"),
                         "--port", str(PORT), "--data-dir", os.path.join(env.data, "pw-airplay")])
    back = wait_for(lambda: len(pw_nodes()) >= 2, 10)
    check("recovers when the engine is back", bool(back))

    # Speakers disappearing: sinks are kept for a minute (mDNS is flaky),
    # not checked here to keep the run short

    # Clean shutdown, then the sanitizer verdict
    code = env.stop("daemon")
    check("clean shutdown", code == 0, f"exit {code}")
    time.sleep(0.5)
    reports = []
    for path in glob.glob(san_log + "*"):
        with open(path) as f:
            reports.append(f.read())
    check("no AddressSanitizer/LeakSanitizer/UBSan reports", not reports,
          f"{len(reports)} report(s), see {san_log}.*" if reports else "")
    if reports:
        args.keep = True
        print(reports[0][:3000])

    # Memory over many cycles, with a regular build
    plain = env.build(sanitize=False)
    env.start("plain", [plain, "-c", conf], env={"XDG_DATA_HOME": env.data})
    wait_for(lambda: len(pw_nodes()) == 3, 12)
    env.procs["daemon"] = env.procs["plain"]
    cycles(3)
    rss0, _ = daemon_resources(env, "plain")
    cycles(args.cycles)
    rss1, _ = daemon_resources(env, "plain")
    del env.procs["daemon"]
    check(f"memory stable over {args.cycles} cycles", rss1 - rss0 < 512, f"{rss0} -> {rss1} KiB")
    env.stop("plain")


if __name__ == "__main__":
    main()
