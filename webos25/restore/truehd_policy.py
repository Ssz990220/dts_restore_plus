"""Private C-plugin/native-format proof. No stream addresses or audio data.

This module does not change routing on import. The existing policy watcher owns
that decision, after matching the foreground media PID and its granted port.
"""
import configparser
import io
import os
from pathlib import Path
import re
import stat
import tempfile

DIRECTORY = Path('/run/dts25-truehd-es')
FIELDS = {'version', 'pid', 'start_ticks', 'token', 'port', 'channels', 'ready'}
TOKEN = re.compile(r'[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}\Z')
PERMIT = b'TrueHD ES temporary test permission v1\n'


def set_allowed(channels, allowed, directory=DIRECTORY):
    """Volatile per-channel permission; never a persistent enable setting."""
    if channels not in (6, 8):
        raise ValueError('Unverified TrueHD layout')
    if not directory.exists() and not allowed:
        return
    directory.mkdir(mode=0o700, exist_ok=True)
    info = directory.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or stat.S_IMODE(info.st_mode) != 0o700:
        raise ValueError('Unsafe permission directory')
    path = directory / f'allow-{channels}'
    try:
        entry = path.lstat()
        if (not stat.S_ISREG(entry.st_mode) or entry.st_uid != 0
                or stat.S_IMODE(entry.st_mode) != 0o600 or path.read_bytes() != PERMIT):
            raise ValueError('Unknown permission file')
        if not allowed:
            path.unlink()
        return
    except FileNotFoundError:
        if not allowed:
            return
    fd, name = tempfile.mkstemp(prefix='.allow-', dir=directory)
    try:
        with os.fdopen(fd, 'wb') as stream:
            os.fchmod(stream.fileno(), 0o600); stream.write(PERMIT)
        os.replace(name, path)
    finally:
        try: os.unlink(name)
        except FileNotFoundError: pass


def start_ticks(pid, proc=Path('/proc')):
    data = (proc / str(pid) / 'stat').read_text()
    return int(data[data.rindex(')') + 2:].split()[19])


def private_read(path, uid=0):
    parent = path.parent.lstat()
    if not stat.S_ISDIR(parent.st_mode) or parent.st_uid != uid or stat.S_IMODE(parent.st_mode) != 0o700:
        raise ValueError('Unsafe proof directory')
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        info = os.fstat(fd)
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != uid
                or stat.S_IMODE(info.st_mode) != 0o600 or info.st_nlink != 1
                or not 0 < info.st_size <= 4096):
            raise ValueError('Unsafe proof file')
        data = os.read(fd, 4097)
        if len(data) > 4096:
            raise ValueError('Oversized proof')
    finally:
        os.close(fd)
    parser = configparser.ConfigParser(interpolation=None)
    parser.read_string(data.decode('ascii'))
    if parser.sections() != ['truehd-es'] or parser.defaults():
        raise ValueError('Unexpected proof schema')
    return dict(parser['truehd-es'])


def read_proof(pid, port, channels, directory=DIRECTORY, proc=Path('/proc'), uid=0):
    if type(pid) is not int or pid < 2 or port not in (0, 1) or channels not in (6, 8):
        return None
    try:
        data = private_read(directory / f'{pid}.{port}.ini', uid)
        if set(data) != FIELDS or not TOKEN.fullmatch(data['token']):
            return None
        expected = {'version': 1, 'pid': pid, 'port': port, 'channels': channels,
                    'start_ticks': start_ticks(pid, proc)}
        if any(int(data[key]) != value for key, value in expected.items()):
            return None
        if data['ready'] not in ('true', 'false'):
            return None
        return {**expected, 'token': data['token'], 'ready': data['ready'] == 'true'}
    except (OSError, ValueError, UnicodeError, configparser.Error, KeyError, IndexError):
        return None


def acknowledge(proof, route, directory=DIRECTORY):
    expected = [2, 6, proof['channels'], 0, 0]
    if route != expected or not proof['ready']:
        raise ValueError('Only a read-back matching no-mixing TrueHD route can be acknowledged')
    current = read_proof(proof['pid'], proof['port'], proof['channels'], directory)
    if current != proof:
        raise ValueError('Proof changed before route acknowledgement')
    values = {key: str(value).lower() for key, value in proof.items()}
    values['earc'] = ','.join(map(str, route))
    parser = configparser.ConfigParser(interpolation=None)
    parser['truehd-es'] = values
    stream = io.StringIO(); parser.write(stream)
    fd, name = tempfile.mkstemp(prefix='.ack-', dir=directory)
    try:
        with os.fdopen(fd, 'w') as target:
            os.fchmod(target.fileno(), 0o600)
            target.write(stream.getvalue())
        os.replace(name, directory / f"{proof['pid']}.{proof['port']}.ack")
    finally:
        try: os.unlink(name)
        except FileNotFoundError: pass


def revoke(proof, directory=DIRECTORY):
    path = directory / f"{proof['pid']}.{proof['port']}.ack"
    try:
        data = private_read(path)
        if (data.get('token') == proof['token']
                and int(data.get('start_ticks', '0')) == proof['start_ticks']):
            path.unlink()
    except (OSError, ValueError, configparser.Error):
        pass
