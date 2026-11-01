#!/usr/bin/env python3
"""Experimental C5 PCM/MAT policy; private root-only state, no stream URL logs."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

ROOT = Path('/var/lib/webosbrew/dts25')
RUN = Path('/run/dts25-pcm')
MARKER = ROOT / 'pcm-multichannel'
MAT = [2, 7, 0, 0, 1]
LPCM71 = [2, 1, 8, 0, 1]
stopping = False


def luna(uri, payload):
    master, slave = os.openpty()
    try:
        result = subprocess.run(['luna-send', '-n', '1', '-w', '2500', '-f',
                                 uri, json.dumps(payload)], stdin=slave,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, timeout=5, check=True)
        value = json.loads(result.stdout)
        if isinstance(value, dict) and value.get('returnValue') is False:
            raise RuntimeError('Service rejected request')
        return value
    finally:
        os.close(slave)
        os.close(master)


def foreground():
    return luna('luna://com.webos.applicationManager/getForegroundAppInfo', {}).get('appId')


def settings():
    return luna('luna://com.webos.settingsservice/getSystemSettings', {
        'category': 'sound', 'keys': ['soundOutput', 'soundOutputDigital', 'eArcSupport']})['settings']


def quality(category):
    result = luna('luna://com.webos.service.audiooutput/quality/getSettings', {
        'category': category, 'settings': {}})
    return next(x['value'] for x in result['settings'] if x['category'] == category)


def eligible(sound, receiver, channels=6):
    carrier_supported = 'MAT2PCM' in receiver.get('earcCapability', [])
    if channels == 8:
        carrier_supported = any(
            x.get('audioCodingType') == 'PCM'
            and isinstance(x.get('maxChannelNumber'), int)
            and x['maxChannelNumber'] >= 8
            for x in receiver.get('advancedEarcCapability', [])
            if isinstance(x, dict))
    return (sound.get('soundOutput') == 'external_arc'
            and sound.get('soundOutputDigital') == 'auto'
            and sound.get('eArcSupport') == 'on'
            and receiver.get('arcActivation') == 'eARC'
            and carrier_supported)


def set_ott(value, app):
    luna('luna://com.webos.service.audio/tv/setDolbyOTTMode', {
        'ottmode': bool(value['isOTTEnable']),
        'atmoslocking': bool(value['isATMOSLockingEnable']), 'callerId': app})


def earc(value=None):
    args = ['amixer', '-c', '0', 'cget' if value is None else 'cset',
            'name=Sndout EARC Outputtype']
    if value is not None:
        args.append(','.join(map(str, value)))
    output = subprocess.check_output(args, text=True, timeout=4)
    match = re.search(r'^\s*: values=([0-9,]+)\s*$', output, re.M)
    if not match:
        raise RuntimeError('Unexpected eARC control')
    return [int(v) for v in match[1].split(',')]


def candidate(row, state, app):
    if (not app or row.get('type') != 'media' or row.get('currentAppId') != app
            or not row.get('is_foreground') or state.get('media_state') != 'play'
            or not any(x.get('resource') == 'ADEC' for x in row.get('resource', []))):
        return None
    programs = state.get('sourceInfo', {}).get('programInfo', [])
    if not programs:
        return None
    tracks = programs[0].get('audioTrackInfo', [])
    index = state.get('audioInfo', {}).get('track', 0)
    if not isinstance(index, int) or not 0 <= index < len(tracks):
        return None
    track = tracks[index]
    codec = str(track.get('codec', '')).lower()
    channels = track.get('channels')
    legacy51 = codec.startswith('dts') and channels == 6
    pcm71 = (app == 'org.moonfin.webos' and channels == 8
             and (codec.startswith('dts') or codec in ('true-hd', 'truehd', 'mlp', 'opus')))
    if (not (legacy51 or pcm71) or track.get('sampleRate') not in (48, 48000)):
        return None
    pid, timestamp = row.get('pid'), row.get('timestamp')
    if not isinstance(pid, int) or not isinstance(timestamp, (int, float)):
        return None
    return {'key': (row['id'], pid, index), 'pid': pid, 'channels': channels,
            'codec': codec, 'rate': 48000,
            'since': timestamp / 1000000000.0, 'app': app}


def sink_proven(kernel_log, pid, since, channels=6):
    if channels == 8:
        return sink_proven71(kernel_log, pid, since)
    # Require both this player's successful flag transition and the following
    # native converter's eight-slot multichannel output (acmod 7). Setting the
    # flag after the first caps event can leave the converter in stereo; that
    # is not evidence for switching the eARC carrier to MAT.
    own = r'^\[\s*([0-9]+\.[0-9]+)\].*\(0*' + str(pid) + r':\d+\).*'
    pattern = own + r'lgao_set_pcm_multi_channel.*changing\s+([01])\s*->\s*([01])'
    first_pattern = (own + r'lgao_push_first_event.*channels=\(int\)6\D'
                     r'.*rate=\(int\)48000\D.*acmod=\(int\)7(?:\D|$)')
    transitions, first_events, converters = [], [], []
    for line in kernel_log.splitlines():
        match = re.search(pattern, line)
        # C5 resource bookkeeping was observed 148 ms after the successful
        # setter. Allow two seconds startup lead, while requiring a fresh converter
        # event and this same player's own six-channel first-format event.
        if match and float(match[1]) >= since - 2.0:
            transitions.append((float(match[1]), int(match[3])))
        match = re.search(first_pattern, line)
        if match:
            first_events.append(float(match[1]))
        if 'Pcmcvt_Event.1332' in line:
            match = re.search(r'^\[\s*([0-9.]+)\].*channels=\(int\)8\D'
                              r'.*acmod=\(int\)7(?:\D|$)', line)
            if match and float(match[1]) >= since - .1:
                converters.append(float(match[1]))
    if not transitions or transitions[-1][1] != 1:
        return False
    flag = transitions[-1][0]
    return any(-.05 <= first - flag <= 5.0 and -.05 <= output - first <= 5.0
               and -.05 <= output - flag <= 5.0
               for first in first_events for output in converters)


def pcm71_evidence(kernel_log, pid, since):
    # Require the current owner's most recent primary format, not just an
    # eight-slot transport or an earlier 7.1 track on the same process.
    primary, converter = None, None
    own = re.compile(r'\(0*' + str(pid) + r':\d+\)')
    for line in kernel_log.splitlines():
        stamp = re.match(r'^\[\s*([0-9.]+)\]', line)
        if not stamp or float(stamp[1]) < since - 2:
            continue
        fields = {}
        for key in ('channels', 'acmod', 'rate'):
            value = re.search(r'\b' + key + r'=\(int\)(\d+)(?:\D|$)', line)
            if value:
                fields[key] = int(value[1])
        t = float(stamp[1])
        if (own.search(line) and 'lgao_push_first_event' in line
                and 'BYPASS' not in line and 'channels' in fields):
            primary = (t, fields)
        if 'Pcmcvt_Event' in line and 'channels' in fields:
            converter = (t, fields)
    return primary, converter


def pcm71_primary_valid(primary):
    return bool(primary
                and primary[1].get('channels') == 8
                and primary[1].get('acmod') == 21
                and primary[1].get('rate') == 48000)


def sink_proven71(kernel_log, pid, since):
    primary, converter = pcm71_evidence(kernel_log, pid, since)
    return bool(pcm71_primary_valid(primary) and converter
                and converter[1].get('channels') == 8
                and converter[1].get('acmod') == 21
                and -.05 <= converter[0] - primary[0] <= 5)


class Pcm71Proof:
    """Keep format proof for a decoder identity, not its mutable uMS timestamp.

    C5 updates row.timestamp on seek/pause without changing the decoder or
    resending first-format. Retain proof across those transitions and kernel
    ring-buffer rotation. A different media ID, PID, track, codec or channel
    count clears it; a subsequent own primary format can revoke it.
    """
    def __init__(self):
        self.identity = None
        self.since = None
        self.valid = False

    def _select(self, key, codec, channels, rate, since):
        identity = (key, codec, channels, 48000 if rate in (48, 48000) else rate)
        if identity != self.identity:
            self.identity, self.since, self.valid = identity, since, False

    def observe(self, row, state):
        programs = state.get('sourceInfo', {}).get('programInfo', [])
        tracks = programs[0].get('audioTrackInfo', []) if programs else []
        index = state.get('audioInfo', {}).get('track', 0)
        if not isinstance(index, int) or not 0 <= index < len(tracks):
            self.identity, self.valid = None, False
            return
        track = tracks[index]
        self._select((row.get('id'), row.get('pid'), index),
                     str(track.get('codec', '')).lower(), track.get('channels'),
                     track.get('sampleRate'), row.get('timestamp', 0) / 1e9)
        if state.get('media_state') not in ('play', 'pause'):
            self.valid = False

    def accept(self, stream, kernel_log):
        self._select(stream['key'], stream['codec'], stream['channels'],
                     stream['rate'], stream['since'])
        primary, _ = pcm71_evidence(kernel_log, stream['pid'], self.since)
        if primary and not pcm71_primary_valid(primary):
            self.valid = False
        elif not self.valid:
            self.valid = sink_proven71(kernel_log, stream['pid'], self.since)
        return self.valid


def find_candidate(app, proof=None):
    rows = luna('luna://com.webos.media/getActivePipelines', {})
    for row in rows:
        if row.get('type') != 'media' or row.get('currentAppId') != app:
            continue
        response = luna('luna://com.webos.media/getPipelineState', {'mediaId': row['id']})
        state = json.loads(response['data'])
        if proof is not None:
            proof.observe(row, state)
        result = candidate(row, state, app)
        if result:
            return result
    return None


def write_status(phase, **data):
    value = dict(phase=phase, updated=int(time.time()), **data)
    temp = RUN / 'status.tmp'
    with temp.open('w') as stream:
        os.chmod(temp, 0o600)
        json.dump(value, stream)
    temp.replace(RUN / 'status.json')


class Session:
    def __init__(self, stream):
        self.stream = stream
        self.previous_ott = quality('ottAdecMode')
        self.previous_earc = earc()
        self.changed = False

    def apply(self):
        self.changed = True
        if self.stream.get('channels', 6) == 6:
            set_ott({'isOTTEnable': True, 'isATMOSLockingEnable': False}, self.stream['app'])
        self.maintain()

    def maintain(self):
        target = LPCM71 if self.stream.get('channels', 6) == 8 else MAT
        if earc() != target and earc(target) != target:
            raise RuntimeError('PCM carrier request rejected')

    def restore(self):
        if not self.changed:
            return
        try:
            if self.stream.get('channels', 6) == 6:
                set_ott(self.previous_ott, self.stream['app'])
        finally:
            sound = settings()
            # Never apply the old native PCM channel count to HDMI input,
            # another app, TV speakers, or a newly selected output policy.
            if (foreground() == self.stream['app']
                    and sound.get('soundOutput') == 'external_arc'
                    and sound.get('soundOutputDigital') == 'auto'
                    and sound.get('eArcSupport') == 'on'
                    and (self.stream.get('channels', 6) != 8 or earc() == LPCM71)):
                earc(self.previous_earc)
            self.changed = False


def interrupted(*_):
    global stopping
    stopping = True


def owner_present():
    app = 'io.github.josippapez.dtsenabler'
    return ((ROOT / '.cli-install').exists() or any(
        (Path(base) / app).is_dir() for base in [
            '/media/developer/apps/usr/palm/applications',
            '/media/cryptofs/apps/usr/palm/applications',
            '/usr/palm/applications']))


def stop_existing():
    try:
        pid = int((RUN / 'watcher.pid').read_text())
        command = Path('/proc/' + str(pid) + '/cmdline').read_bytes()
        if str(ROOT / 'pcm-mat-watch.py').encode() not in command:
            return
        os.kill(pid, signal.SIGTERM)
        for _ in range(80):
            if not Path('/proc/' + str(pid)).exists():
                return
            time.sleep(.1)
        raise RuntimeError('Watcher has not stopped')
    except (FileNotFoundError, ProcessLookupError):
        return


def close_inherited_fds():
    # nohup redirects stdio but can retain the launching JS service's LS2
    # sockets. Keeping those alive prevents Homebrew from reconnecting after
    # its own process exits. Discard borrowed handles before creating ours.
    for entry in Path('/proc/self/fd').iterdir():
        fd = int(entry.name)
        if fd > 2:
            try:
                os.close(fd)
            except OSError:
                pass


def watch():
    close_inherited_fds()
    RUN.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(RUN, 0o700)
    with (RUN / 'watcher.pid').open('a+') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return
        lock.seek(0)
        lock.truncate()
        lock.write(str(os.getpid()))
        lock.flush()
        for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
            signal.signal(sig, interrupted)
        session = None
        proof71 = Pcm71Proof()
        try:
            while MARKER.exists() and owner_present() and not stopping:
                try:
                    stream = None
                    app = foreground()
                    if not (ROOT / 'passthrough').exists():
                        stream = find_candidate(app, proof71)
                    if stream and not eligible(settings(), quality('external_arc'), stream['channels']):
                        stream = None
                    if stream and stream['channels'] == 8:
                        kernel = subprocess.check_output(['dmesg'], text=True, timeout=4)
                        if not proof71.accept(stream, kernel):
                            stream = None
                    if session and (not stream or stream['key'] != session.stream['key']):
                        session.restore()
                        session = None
                    if stream and session is None:
                        kernel = subprocess.check_output(['dmesg'], text=True, timeout=4)
                        if (eligible(settings(), quality('external_arc'), stream['channels'])
                                and (stream['channels'] == 8 or sink_proven(kernel, stream['pid'], stream['since']))):
                            session = Session(stream)
                            session.apply()
                    if session:
                        session.maintain()
                        channels = session.stream['channels']
                        write_status('lpcm-requested' if channels == 8 else 'mat-requested',
                                     channels=channels, app=app)
                    else:
                        write_status('waiting', reason='Waiting for eligible PCM layout and sink proof')
                except Exception as error:
                    if session:
                        session.restore()
                        session = None
                    # Never print the exception message: native/network errors
                    # can contain a user's authenticated stream URI.
                    write_status('error', error_class=type(error).__name__)
                time.sleep(1.5)
        finally:
            try:
                if session:
                    session.restore()
            finally:
                write_status('stopped')
                (RUN / 'watcher.pid').unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stop', action='store_true')
    parser.add_argument('--check-idle', action='store_true')
    args = parser.parse_args()
    if os.geteuid() != 0:
        raise SystemExit('Root required')
    if args.check_idle:
        rows = luna('luna://com.webos.media/getActivePipelines', {})
        if any(any(x.get('resource') == 'ADEC' for x in row.get('resource', []))
               for row in rows):
            raise SystemExit('FAIL: Stop audio/video playback before enabling the PCM test')
    elif args.stop:
        stop_existing()
    else:
        watch()


if __name__ == '__main__':
    main()
