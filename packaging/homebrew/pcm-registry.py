import json
from pathlib import Path
import re
import sys

repo, tag, dest = sys.argv[1:]
assert re.fullmatch(r'[\w.-]+/[\w.-]+', repo)
assert re.fullmatch(r'pcm51-test-\d+\.\d+\.\d+', tag)
name = 'io.github.josippapez.dtsenabler'
# A fixed prerelease URL: never resolve to the upstream or an unrelated latest release.
data = dict(title='DTS Enabler PCM Test',
            iconUri=f'https://raw.githubusercontent.com/{repo}/main/assets/icon160.png',
            manifestUrl=f'https://github.com/{repo}/releases/download/{tag}/{name}.manifest.json',
            category='multimedia', pool='main', requirements={'webosRelease': '>=7.4'},
            description='Experimental C5 DTS to PCM 5.1. Replaces the original DTS Enabler app. Root and eARC/MAT required. Keep PCM mode off until testing.')
# JSON is a YAML subset, and avoids interpolating unescaped YAML values.
(Path(dest) / (name + '.yml')).write_text(json.dumps(data, indent=2) + '\n')
