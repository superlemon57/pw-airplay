# AGENTS.md

Notes for coding agents and contributors working on this repository.

## What this is

An AirPlay 2 output for PipeWire. Two programs cooperate:

- `pw-airplayd` (this repo, `src/`): PipeWire sinks, pacing, volume, MPRIS,
  control of the engine.
- The engine: OwnTone 29.3 built with `engine/owntone-29.3.patch`. It does the
  AirPlay protocol. `install.sh` clones, patches and builds it.

`README.md` describes the behaviour; read it first.

## Layout

| Path | Contents |
|---|---|
| `src/pw-airplayd.c` | main loop, speaker discovery, choosing which sink owns the speakers, idle release |
| `src/sink.c` | one PipeWire `Audio/Sink` stream: driver timer, volume interception, latency reporting |
| `src/engine.c` | ring buffer and pacer writing PCM to the engine pipe at a constant rate |
| `src/owntone.c` | client for the engine's JSON API |
| `src/mdns.c` | Avahi browser: stereo pairs (`tsid`, `igl`, `gpn`), computers |
| `src/mpris.c` | sd-bus MPRIS client |
| `src/nowplaying.c` | matching MPRIS players to sinks, metadata pipe writer, remote commands |
| `src/config.c` | configuration file, and generation of the engine's `owntone.conf` |
| `engine/owntone-29.3.patch` | changes to OwnTone, with a header describing them |
| `systemd/` | user units (daemon, engine) and the `airptpd` system unit |
| `install.sh`, `install-system.sh`, `uninstall.sh` | installation |
| `tests/run.py` | end to end tests against `mock_engine.py` and `mock_player.py`, under sanitizers |
| `tests/check_exposure.sh` | network and file exposure checks of a running installation |

## Building

```sh
make            # needs pipewire, libcurl, json-c, avahi-client, libsystemd devel files
make install    # to ~/.local/bin
```

Before submitting, run the tests (see README, "Tests"); they must all pass:

```sh
python3 -I tests/run.py
```

The build must be free of warnings, and the GCC analyzer clean:

```sh
for f in src/*.c; do
	gcc -std=gnu11 -fanalyzer -Wall -Wextra -Wno-unused-parameter \
	    -Wno-missing-field-initializers \
	    $(pkg-config --cflags libpipewire-0.3 libcurl json-c avahi-client libsystemd) \
	    -c "$f" -o /dev/null
done
```

Shell scripts are POSIX `sh`; check them with `sh -n` (and `shellcheck` if
available).

## Changing the engine patch

Work in a checkout of OwnTone at tag `29.3` with the patch applied (the one in
`~/.cache/pw-airplay/owntone-29.3` after an install will do), then regenerate:

```sh
git diff -- configure.ac src > /tmp/changes.diff
```

and replace everything below the header of `engine/owntone-29.3.patch` with
it. Keep the header in sync with what the patch does. Check that it applies
to a pristine tree (`git apply --check`). Patch code follows OwnTone's style
(GNU-ish braces, two-space indent).

## Conventions

- C11 (gnu11), tabs, PipeWire-like style. Match the surrounding code.
- Comments explain why, not what. No banner or decorative comments.
- Logging goes through `pw_log_*`; info level is for state changes a user
  would want in the journal, not for per-buffer events.
- Allocation failures are fatal: use `xmalloc`/`xcalloc`/`xstrdup` from
  `src/util.h`.
- New options go in `config.h`/`config.c` and in the commented example
  `config/pw-airplay.conf`. Engine settings are derived from our config in
  `config_write_owntone_conf()`; never ask users to edit `owntone.conf`.

## Threads

- Main loop (`pw_main_loop`): everything except the below.
- One PipeWire data loop (realtime): sink `process` callbacks, sink driver
  timers and the pacer in `engine.c`. The context is created with
  `context.num-data-loops = 1` so they share a thread. Cross the boundary only
  with `pw_loop_invoke` or atomics, and never block on the data loop.
- Avahi threaded poll (`mdns.c`): shares state under a mutex.
- Metadata writer thread (`nowplaying.c`): artwork fetches and blocking
  writes to the metadata pipe.

## Things that are easy to get wrong

- Latency must stay constant. Anything that adds buffering on the way to the
  engine changes the reported latency (`total_latency_ms()`); keep them in
  line.
- Both halves of a stereo pair must be streamed to. Sending only to the pair
  leader plays one channel.
- HomePod firmware 27 rejects AirPlay requests unless the User-Agent looks
  like `AirPlay/...`; the generated engine config sets it.
- OwnTone only opens the metadata pipe while it plays our audio pipe; data
  written before that is lost. `nowplaying.c` resends everything when it
  manages to open it.
- Timestamps written by the data thread (e.g. `sink_last_audio_ns()`) can be
  newer than a "now" taken earlier on the main thread. Compare times with
  `elapsed_ns()`, never `now - then` on unsigned values.
- Our sinks are `pw_stream`s, which WirePlumber would treat as streams and
  restore volumes on; `state.restore-props = false` keeps the speaker's own
  volume authoritative.

## Testing with real speakers

AirPlay speakers are usually somebody's household devices. Do not play
audio on them without asking the person whose speakers they are. Things that
can be checked silently:

- discovery: `avahi-browse -rpt _airplay._tcp`, `curl -s localhost:3689/api/outputs`
- reachability: selecting an output while the engine is stopped makes it
  probe the speaker without playing anything
  (`curl -X PUT -d '{"outputs":["<id>"]}' localhost:3689/api/outputs/set`,
  then deselect with `{"outputs":[]}`)
- sinks and reported latency: `wpctl status`, `pw-dump`
