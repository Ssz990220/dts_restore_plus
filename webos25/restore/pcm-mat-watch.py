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


def eligible(sound, receiver):
    return (sound.get('soundOutput') == 'external_arc'
            and sound.get('soundOutputDigital') == 'auto'
            and sound.get('eArcSupport') == 'on'
            and receiver.get('arcActivation') == 'eARC'
            and 'MAT2PCM' in receiver.get('earcCapability', []))


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
    if (not str(track.get('codec', '')).lower().startswith('dts')
            or track.get('channels') != 6 or track.get('sampleRate') not in (48, 48000)):
        return None
    pid, timestamp = row.get('pid'), row.get('timestamp')
    if not isinstance(pid, int) or not isinstance(timestamp, (int, float)):
        return None
    return {'key': (row['id'], pid, index), 'pid': pid,
            'since': timestamp / 1000000000.0, 'app': app}


def sink_proven(kernel_log, pid, since):
    # LG's own setter logs this successful false -> true transition. Require
    # it after this pipeline's resource timestamp, not an old NDL/app event.
    pattern = (r'^\[\s*([0-9]+\.[0-9]+)\].*\(0*' + str(pid)
               + r':\d+\).*lgao_set_pcm_multi_channel.*changing\s+0\s*->\s*1')
    return any(float(m.group(1)) >= since - .1
               for line in kernel_log.splitlines()
               for m in [re.search(pattern, line)] if m)


def find_candidate(app):
    rows = luna('luna://com.webos.media/getActivePipelines', {})
    for row in rows:
        if row.get('type') != 'media' or row.get('currentAppId') != app:
            continue
        response = luna('luna://com.webos.media/getPipelineState', {'mediaId': row['id']})
        result = candidate(row, json.loads(response['data']), app)
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
        set_ott({'isOTTEnable': True, 'isATMOSLockingEnable': False}, self.stream['app'])
        self.maintain()

    def maintain(self):
        if earc() != MAT and earc(MAT) != MAT:
            raise RuntimeError('MAT request rejected')

    def restore(self):
        if not self.changed:
            return
        try:
            set_ott(self.previous_ott, self.stream['app'])
        finally:
            sound = settings()
            # Never apply the old native PCM channel count to HDMI input,
            # another app, TV speakers, or a newly selected output policy.
            if (foreground() == self.stream['app']
                    and sound.get('soundOutput') == 'external_arc'
                    and sound.get('soundOutputDigital') == 'auto'
                    and sound.get('eArcSupport') == 'on'):
                earc(self.previous_earc)
            self.changed = False


def interrupted(*_):
    global stopping
    stopping = True


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


def watch():
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
        try:
            while MARKER.exists() and not stopping:
                try:
                    stream = None
                    app = foreground()
                    if not (ROOT / 'passthrough').exists() and eligible(settings(), quality('external_arc')):
                        stream = find_candidate(app)
                    if session and (not stream or stream['key'] != session.stream['key']):
                        session.restore()
                        session = None
                    if stream and session is None:
                        kernel = subprocess.check_output(['dmesg'], text=True, timeout=4)
                        if sink_proven(kernel, stream['pid'], stream['since']):
                            session = Session(stream)
                            session.apply()
                    if session:
                        session.maintain()
                        write_status('mat-requested', channels=6, app=app)
                    else:
                        write_status('waiting', reason='Waiting for eligible DTS 5.1 and PCM sink proof')
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
