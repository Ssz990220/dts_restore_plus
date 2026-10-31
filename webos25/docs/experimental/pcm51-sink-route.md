# Experimental decoded-PCM 5.1 sink routing

Status: **source prototype, not a complete playback fix or a released TV binary**.
Based on `83e8f17fb95040d189ca20f750594a6bdeb8ce28`. No installed TV files
were changed while preparing it.

## Device evidence

On an LG C5 / webOS 25, Moonfin 3.0.0 was observed playing an MKV with its
six-channel DTS track selected. The installed dtsdec library was loaded.
The native audio sink received S32LE, 48 kHz, six channels, mask `0xc0f`,
while Sonos Arc Ultra reported PCM 2.0. Thus DTS decoding worked during that
observation, but discrete multichannel output did not.

A separate NDL PCM test activated the LG sink's `pcm-multi-ch` property and
used a temporary MAT route. Sonos reported Dolby Multichannel PCM 5.1, and the
listener separately confirmed rear-left, rear-right and LFE output. That is
evidence for the combination, not proof that the property alone fixes native
playback: sample format, channel mask and framework also differed.

The DTS capability marker was enabled but the effective configuration was
`TrueHD`, without DTS. The plugin's last-status file explicitly reported that
the capability switch had been skipped when audio started. Moonfin later
played DTS anyway. This is a separate capability-advertising issue and is not
proven to explain the earlier generic "Go Back" playback error.

## First-stage code

The new `dtsdec pcm-multichannel=true` property is **off by default** and is
intended to be set before playback. This prototype:

- Applies only to decoded six-channel, 48 kHz PCM, before output-format setup.
- Follows the decoder's actual linked pad chain, including ghost pads, to a
  compatible `audiosink` boolean property `pcm-multi-ch`.
- Rejects ambiguous output branches and shared input paths rather than searching
  the whole pipeline for an element named `audio_sink`.
- Uses a weak reference and restores only a flag it changed from false to true.
  Already-enabled flags are not cleared. Stream stop, format renegotiation and
  object finalization release the owned change.
- Leaves the decoder's samples, PCM format, channel positions and reorder map
  unchanged. Bitstream passthrough uses its separate negotiation path.
- Posts a technical `webos-dts-pcm-route` element message for diagnostics, with
  channel count, rate and whether sink configuration succeeded; no URLs or
  credentials are included.

It does not retry a late/unlinked sink after the initial caps event, does not
support arbitrary dynamic rewiring, and does not introduce an installed marker
or app setting. Stock Moonfin cannot enable this property yet.

## Still required for the complete feature

Preserving PCM channels is only the first part. OTT/MAT selection and restoration
must be controlled separately, preferably by the plugin service, with a reliable
association to the active decoded-DTS stream and sound-output selection. Do not
run Luna calls or global mixer-policy changes on the decoder's audio thread.

The current prototype does **not** enable OTT, force MAT, change eARC controls,
modify capability declarations, or add an automatic service watcher. It must not
be presented as a working DTS-to-Dolby-PCM option. An eventual UI mode must be
mutually exclusive with "Send DTS to a receiver", which sends compressed DTS
instead of decoded PCM.

Before enabling it in Moonfin/native playback, build the complete plugin for
C5's 32-bit ARM soft-float ABI, test a temporary override in an isolated process,
and confirm both receiver format and physical channel locations. Only then test
the native player's complete lifecycle, including pause, seek, track changes,
backgrounding and restoration. No 7.1 success is claimed.

## Validation performed

- Existing `src/test/run-tests.sh`: all checks passed, including DRC and IEC 61937.
- New `src/test/run-pcm-route-tests.sh`: compiled and passed against a workspace
  build of official GStreamer 1.24.10 core on macOS. Cases cover a direct chain,
  queue/ghost-pad traversal, a pre-enabled flag, an unrelated sink, unlinked and
  incompatible sinks, fan-out, shared-input rejection and destroyed-target cleanup.
- The graph test uses a mock `audiosink`. It validates GStreamer traversal and
  property ownership, **not LG firmware audio behavior**.
- No complete ARM plugin build or on-TV test of this patch was performed. The
  existing cross-build script needs Docker and the armel toolchain, which were
  not available on this Mac.

Run the additional graph checks on a host with GStreamer core development files:

```sh
sh webos25/restore/src/test/run-pcm-route-tests.sh
```

Missing development files produce exit 77 (not a passed test).
