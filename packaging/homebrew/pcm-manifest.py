"""Create a fixed experimental release manifest; no upstream download URLs."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

repo, tag, decoder_commit_file = sys.argv[1:]
assert re.fullmatch(r'[\w.-]+/[\w.-]+', repo)
assert re.fullmatch(r'pcm51-test-\d+\.\d+\.\d+', tag)
root = Path('webos25/app')
app = json.loads((root / 'appinfo.json').read_text())
ipks = list((root / 'dist').glob('*.ipk'))
assert len(ipks) == 1
ipk = ipks[0]
manifest = {key: app[key] for key in ['id', 'version', 'type', 'title']}
manifest.update(appDescription='Experimental rooted C5 DTS decode to PCM 5.1 route. Requires listening validation; not verified for 7.1.',
                iconUri=f'https://raw.githubusercontent.com/{repo}/main/assets/icon160.png',
                sourceUrl=f'https://github.com/{repo}', rootRequired=True,
                ipkUrl=ipk.name, ipkSize=ipk.stat().st_size,
                ipkHash={'sha256': hashlib.sha256(ipk.read_bytes()).hexdigest()})
(root / 'dist' / (app['id'] + '.manifest.json')).write_text(json.dumps(manifest, indent=2) + '\n')
provenance = dict(repository=repo, release=tag,
                  app_commit=subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
                  decoder_commit=Path(decoder_commit_file).read_text().strip(),
                  decoder_sha256=hashlib.sha256(Path('webos25/restore/out/libgstdtsdec.so').read_bytes()).hexdigest(),
                  switch_sha256=hashlib.sha256(Path('webos25/restore/switch-out/libgstadecswitch.so').read_bytes()).hexdigest(),
                  ipk_sha256=manifest['ipkHash']['sha256'], status='experimental: physical channel test required')
(root / 'dist/build-provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
