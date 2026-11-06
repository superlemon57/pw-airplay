self:
{ config, lib, pkgs, ... }:

let
  cfg = config.services.pw-airplay;
  owntone-airplay = self.packages.${pkgs.system}.owntone-airplay;
in {
  options.services.pw-airplay = {
    enable = lib.mkEnableOption ''
      airptpd, the PTP clock AirPlay 2 speakers synchronise to for
      multi-room sync. System-wide half of pw-airplay; the per-user
      daemon, engine and configuration are in the Home Manager module
    '';

    openFirewall = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Open UDP ports 319 and 320 (PTP) so speakers can reach airptpd.";
    };
  };

  config = lib.mkIf cfg.enable {
    systemd.services.airptpd = {
      description = "PTP clock for AirPlay 2 senders (airptpd)";
      wants = [ "network-online.target" ];
      after = [ "network.target" "network-online.target" ];
      wantedBy = [ "multi-user.target" ];
      serviceConfig = {
        ExecStart = "${owntone-airplay}/sbin/airptpd -f";
        DynamicUser = true;
        AmbientCapabilities = [ "CAP_NET_BIND_SERVICE" ];
        CapabilityBoundingSet = [ "CAP_NET_BIND_SERVICE" ];
        NoNewPrivileges = true;
        Restart = "on-failure";
      };
    };

    networking.firewall = lib.mkIf cfg.openFirewall {
      allowedUDPPorts = [ 319 320 ];
    };
  };
}
