# pw-airplay

AirPlay 2 output for PipeWire. AirPlay 2 speakers on the network show up as
regular audio outputs, next to the built-in speakers, and are picked the same
way from the desktop's sound settings.

- One output per speaker. A HomePod stereo pair is one output, named after
  the pair, playing in stereo.
- Speaker groups defined in the configuration, and an output for all
  speakers, played in sync.
- The AirPlay latency (about 2.4 s) is reported to PipeWire, so video players
  and browsers delay the picture accordingly and stay in sync.
- The volume slider sets the speaker's own volume rather than scaling the
  audio.
- Speakers are connected when audio starts and released after a period of
  silence. Another device taking over a speaker is left alone.
- What is playing is sent to the speakers, taken from the application over
  MPRIS: HomePods report title and play state, e.g. to Siri and in the Home
  app. Pausing the application pauses the AirPlay session.

Tested on Fedora 44 (aarch64, PipeWire 1.6) with HomePod mini on firmware 27.
It should work with other AirPlay 2 receivers, which have not been tested.

## How it works

```
apps --> PipeWire sink(s) --> pw-airplayd --> pipe --> AirPlay engine --> speakers
                              (clock, pacing,          (OwnTone 29.3,
                               volume, MPRIS)           patched)
```

**pw-airplayd** (this repository, C) creates the sinks. Each sink drives its
own graph from `CLOCK_MONOTONIC`, the clock the engine runs on, and the audio
is written to the engine at a constant rate, so latency does not drift and is
the same in every session. It controls the engine over its local HTTP API,
groups devices into speakers and pairs from their mDNS records, follows the
MPRIS player feeding the active sink, and passes speaker commands back to it.

**The engine** is [OwnTone](https://github.com/owntone/owntone-server), which
implements the AirPlay 2 sender protocol: HomeKit transient pairing,
encryption, PTP timing and multi-room sync. It is built from source with a
patch, `engine/owntone-29.3.patch`, which adapts it to a live source and
limits what it exposes on the network. The patch header lists the changes.

**airptpd**, from the same source tree, is the PTP clock the speakers
synchronise to. It needs UDP ports 319 and 320, so it runs as a small system
service.

## Installation

```sh
./install.sh                # as your user: builds and installs everything
sudo ./install-system.sh    # once: PTP clock service, firewall ports
systemctl --user restart pipewire pipewire-pulse wireplumber
systemctl --user start pw-airplay
```

`install.sh` checks the build dependencies first and prints the command to
install the missing ones on Fedora. On other distributions, the same
libraries are needed: the list of pkg-config names is in `install.sh`. The
engine is built in `~/.cache/pw-airplay` and installed to
`~/.local/opt/owntone`; running `install.sh` again updates everything.

PipeWire's own AirPlay 1 sinks (`module-raop-discover`) are disabled by a
configuration drop-in, since they would show the same speakers a second time
with the wrong latency.

## Configuration

`~/.config/pw-airplay/pw-airplay.conf`, see the comments in it. Groups of
speakers are defined there:

```ini
[group "Downstairs"]
members = Living room, Kitchen
```

After a change: `systemctl --user restart pw-airplay-engine pw-airplay`.

## Latency

The reported latency is `start_buffer_ms + pipe_start_buffer_ms` plus about
35 ms of buffering in pw-airplayd, 2402 ms with the defaults, which is close
to what Apple devices use. `pw-airplayd --print-latency` prints it. If picture
and sound are slightly off, adjust `latency_offset_ms`.

Sounds that are not tied to a picture (notifications, games) are heard about
2.4 s late, as with any AirPlay sender.

## Known limitations

- Not confirmed on hardware yet: play/pause/next/previous given on the
  speaker itself being passed to the application, and volume changes made on
  the speaker being reflected in the slider. Both are implemented.
- An iPhone does not show what is playing on the HomePod, nor offer controls
  for it, while Siri and the Home app do get title and play state. Apple
  senders appear to use a MediaRemote data channel (AirPlay stream type 130)
  for this, which is not implemented.
- Now playing needs MPRIS. Firefox, Chromium based browsers, Spotify, Kodi
  and most media players have it.
- Only one output plays at a time: starting audio on another AirPlay output
  moves the speakers over.
- Speakers must be on the same network segment. VPN clients that block local
  network traffic (for instance Mullvad with "Local network sharing" off)
  prevent any connection.

## Security

- The engine's HTTP server listens on the network, because speakers report
  volume changes and play/pause through DACP requests to it. Everything else
  on it is refused to other hosts: the JSON API is only trusted from
  localhost, the web interface likewise, and the DAAP library share is locked
  with a random password.
- DACP requests are accepted from a speaker we stream to, identified by its
  Active-Remote id. That id derives from the speaker's device id, which is
  public in its mDNS records, so a host on the local network can send
  play/pause/next/previous and volume commands while a session is active.
  This is how AirPlay senders work in general (iTunes, OwnTone); the impact
  is limited to controlling playback.
- Only the DACP service is advertised over mDNS. OwnTone's library, web and
  remote control adverts are disabled.
- Pipes and state are private to the user (mode 0700/0600); the generated
  engine configuration, which holds the library password, is 0600.

`tests/check_exposure.sh` checks these points on a running installation.

## Tests

```sh
python3 -I tests/run.py          # end to end, needs a running PipeWire session
tests/check_exposure.sh          # network and file exposure of an installation
```

`tests/run.py` builds pw-airplayd with AddressSanitizer and
UndefinedBehaviorSanitizer and runs it against a mock of the engine and a
mock MPRIS player, playing a test tone to test sinks only (no speaker is
involved). It covers sink creation, reported latency, pacing of the audio
feed, volume in both directions, now playing metadata and artwork, remote
commands, pause and resume, switching outputs, idle release, malformed
input, the engine going away, descriptor and memory stability over repeated
play/release cycles, and sanitizer reports. Requirements: `pw-play`,
`wpctl`, `pw-dump`, PyGObject.

The engine patch is not covered: its live-pipe, MediaRemote and DACP changes
were tested against HomePod mini hardware only.

## Troubleshooting

```sh
journalctl --user -u pw-airplay -u pw-airplay-engine -f
less ~/.local/share/pw-airplay/log/owntone.log
wpctl status
```

Set `engine_log_level = info` (or `debug`) in the configuration for more
detail from the engine.

## Uninstall

```sh
./uninstall.sh              # add --purge to also remove the configuration
sudo ./uninstall.sh --system
```

## Credits

- [OwnTone](https://github.com/owntone/owntone-server) for the AirPlay 2
  sender, and airptpd.
- [cliairplay](https://github.com/elsagranger/airplay-cli) (Music Assistant),
  whose documentation of the MediaRemote `/command` requests the
  now playing support follows.
- [pyatv](https://github.com/postlund/pyatv) for the MediaRemote protobuf
  definitions.

## License

GPL-2.0-or-later, like OwnTone. See [LICENSE](LICENSE).
