# Streaming services on the X45: what is feasible

Research of 2026-10-08, before any implementation. What the stock firmware did:
Tidal and Deezer through the manufacturer's Airable tenant, Qobuz through a direct client with the manufacturer's
own API key, Spotify through the licensed embedded SDK, Roon through the RAAT SDK. **None of that can be reused**:
the keys and SDKs belong to the manufacturer and its agreements, and the replacement does not copy them.

The replacement already receives: **AirPlay** (shairport-sync, classic AirPlay 1), **UPnP/DLNA renderer**
(upmpdcli), **Spotify Connect** (librespot, built; playback from a phone still to be tried), **Bluetooth** (A2DP
sink, SBC), **MPD** with a UPnP server browser on the panel, internet radio. Everything below is about what is
missing on top of that.

## Verdict per service

| Service | Straight on the unit | What works today / the sensible route | Effort | Risk |
|---|---|---|---|---|
| **Spotify** | librespot (Spotify Connect, Premium only) | built and supervised by `x45stack`; the phone's Spotify app lists the unit. Needs one real test | none | unofficial (the licensed SDK is for manufacturers); Spotify changed its backend in late 2025 and librespot 0.8.0 follows it |
| **Qobuz** | not without credentials | Qobuz Connect is for licensed partners (hi-fi brands, StreamUnlimited's SDK); the REST API is partner-only (an app id and secret from Qobuz under a developer agreement). Works today through a control point on the LAN: UPnP/OpenHome apps with Qobuz built in play to the unit's upmpdcli renderer; AirPlay and Bluetooth from the Qobuz app | low (setup), high (native client) | a native client needs partner keys; scraped or reused keys breach the terms |
| **Tidal** | not without a licence | Tidal Connect is a licensed SDK for device makers. Control points with Tidal (see above), AirPlay and Bluetooth work. Unofficial Tidal Connect builds for the Raspberry Pi circulate as binaries lifted from a vendor's firmware: not used here | low (setup) | the unofficial route is legally doubtful and breaks when Tidal changes something |
| **Deezer** | no | Deezer retired Deezer Connect in 2025 and offers no playback API to third parties. The community receivers (pleezer, OpenDeezer) depend on an unofficial login and a protocol Deezer has shut down. Use AirPlay or Bluetooth from the Deezer app | none | not recommended |
| **Roon** | no (RAAT) | Roon Ready needs the RAAT SDK and certification (Roon's partner programme). Roon plays to AirPlay devices, which works with the unit today at CD quality. RoonBridge is the official free endpoint for Linux ARMv7, but it needs glibc 2.27 and OpenSSL 1.1.1 or newer since the .NET 10 build; the unit's system library (eglibc 2.19, OpenSSL 1.0) is too old, the userland here is static musl | high (a modern glibc userland just for it) | fragile, unsupported; AirPlay is the practical answer |
| **Apple Music** | no | AirPlay from iPhone, iPad, Mac (works, classic AirPlay; AirPlay 2 is not built) | none | |
| **Amazon Music, YouTube Music** | no | no third-party playback API; AirPlay and Bluetooth from the phone | none | |
| **Self-hosted** (Navidrome, Jellyfin, Plex, a NAS, foobar2000) | yes | UPnP/DLNA servers are browsed on the panel already (the FRITZ!Box folders); Subsonic-compatible servers have open APIs and could get a native browser | low | none: open protocols |
| **Internet radio** | yes | the radio list comes from the router's directory; radio-browser.info is an open directory that could be searched on the panel | low | none |

## The practical routes, in order of value

1. **Use what is built.** Spotify Connect, AirPlay, UPnP and Bluetooth cover every service on the list for someone
   who owns a phone with the app. The limit is quality: AirPlay and Bluetooth are CD quality or lossy; a UPnP
   control point can send the service's lossless stream (MPD hands it to the DAC at the stream's own
   rate, up to the 192 kHz the XMOS takes).
2. **A bridge on the LAN for Qobuz and Tidal on the panel.** upmpdcli can also act as a UPnP media server with
   Qobuz and Tidal plug-ins (Python, runs on a PC, NAS or Raspberry Pi with the user's own account). The unit's
   panel already browses UPnP servers, so the streaming catalogue would appear as one more folder, with the
   credentials held on the bridge and not on the unit. Secondary sources report the Qobuz plug-in working again with
   upmpdcli 1.9.17 (OAuth) and a working Tidal plug-in since 2024; this needs a trial before anyone relies on it.
3. **Roon** through AirPlay now; the certified path stays closed.
4. **A native Qobuz or Tidal client on the unit** only with the service's blessing (partner keys). Without that it
   would rest on keys taken from another app, which the services' terms forbid and which they revoke without
   notice; the stock firmware's own Qobuz key is the manufacturer's and is not touched.

Not implemented: nothing in this list changes the unit's software; it records what could be done and what should not.

## Sources (read 2026-10-08, mostly secondary)

* Qobuz Connect launch and partner list: [What Hi-Fi](https://www.whathifi.com/streaming-entertainment/music-streaming/qobuz-connect-finally-launches-with-wide-support-across-hi-fi-brands), [Qobuz Connect via StreamUnlimited](https://www.streamunlimited.com/qobuz-connect-now-available-via-streamsdk/), [100 partners](https://community.qobuz.com/press-en/qobuz-connect-celebrates-over-100-partners)
* Qobuz API partner-only: [API Evangelist profile](https://providers.apievangelist.com/providers/qobuz/)
* Tidal Connect: [What Hi-Fi explainer](https://www.whathifi.com/features/tidal-connect-everything-you-need-to-know), [Tidal](https://tidal.com/connect), unofficial builds [Volumio thread](https://community.volumio.com/t/so-long-and-thanks-for-all-the-fish/76915), [Roon forum](https://community.roonlabs.com/t/ropieee-tidal-connect-unsupported/125067)
* Spotify: [librespot releases](https://github.com/librespot-org/librespot/releases), [CoreELEC thread on the late-2025 change](https://discourse.coreelec.org/t/librespot-stopped-working/58040)
* Roon: [Linux install](https://help.roonlabs.com/portal/en/kb/articles/linux-install), [Bridge requirements change](https://community.roonlabs.com/t/important-roon-bridge-minimum-system-requirements-are-changing-on-august-30-2026/322567), [partner programmes](https://help.roonlabs.com/portal/en/kb/articles/roon-partner-programs)
* upmpdcli: [Arch package](https://archlinux.org/packages/extra-staging/x86_64/upmpdcli/), [Kazoo/Tidal issue](https://framagit.org/medoc92/upmpdcli/-/issues/96)
* Deezer: [Deezer Connect end of life](https://support.deezer.com/hc/en-gb/articles/5449309457949-Deezer-Connect), [pleezer](https://docs.rs/pleezer)
