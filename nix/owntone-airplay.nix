{ lib
, stdenv
, fetchFromGitHub
, autoreconfHook
, pkg-config
, bison
, flex
, gperf
, libtool
, gettext
, avahi
, curl
, ffmpeg
, gnutls
, json_c
, libconfuse
, libevent
, libgcrypt
, libgpg-error
, libplist
, libsodium
, libunistring
, libwebsockets
, libxml2
, sqlite
, zlib
, alsa-lib
, pipewire
, systemd
}:

# OwnTone, patched (../engine/owntone-29.3.patch) and with its PTP clock
# daemon, airptpd, enabled. Mirrors install.sh's build_engine().
stdenv.mkDerivation (finalAttrs: {
  pname = "owntone-airplay";
  version = "29.3";

  src = fetchFromGitHub {
    owner = "owntone";
    repo = "owntone-server";
    tag = finalAttrs.version;
    hash = "sha256-a7OpRQBzeOcZEINRxWvOlbmKCXFBtQTk9nMJAUkCqgM=";
  };

  patches = [ ../engine/owntone-29.3.patch ];

  nativeBuildInputs = [
    autoreconfHook
    pkg-config
    bison
    flex
    gperf
    libtool
    gettext
  ];

  buildInputs = [
    avahi
    curl
    ffmpeg
    gnutls
    json_c
    libconfuse
    libevent
    libgcrypt
    libgpg-error
    libplist
    libsodium
    libunistring
    libwebsockets
    libxml2
    sqlite
    zlib
    alsa-lib
    pipewire
    systemd
  ];

  configureFlags = [
    "--disable-chromecast"
    "--disable-spotify"
    "--disable-mpd"
  ];

  # airptpd (src/libairptp, a nested autotools package) is off by default;
  # reconfigure it with --enable-daemon so the recursive make/make install
  # below builds and installs it too.
  postConfigure = ''
    (cd src/libairptp && ./configure --prefix=$prefix --enable-daemon)
  '';

  enableParallelBuilding = true;

  meta = {
    description = "OwnTone AirPlay 2 sender, patched for pw-airplay, with airptpd";
    homepage = "https://github.com/owntone/owntone-server";
    changelog = "https://github.com/owntone/owntone-server/releases/tag/${finalAttrs.version}";
    license = lib.licenses.gpl2Plus;
    platforms = lib.platforms.linux;
  };
})
