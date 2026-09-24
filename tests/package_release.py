"""Package already-tested static binaries with machine-readable provenance."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def version(value, tag):
    if not re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?', value):
        raise ValueError('VERSION must contain a semantic version')
    expected = 'v' + value
    if tag and tag != expected:
        raise ValueError('Tag must match VERSION: ' + expected)
    return expected


def main():
    tag = os.environ.get('GITHUB_REF_NAME', '') if os.environ.get('GITHUB_REF_TYPE') == 'tag' else ''
    release = version(Path('VERSION').read_text().strip(), tag)
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
    output = Path('build/release')
    output.mkdir(parents=True, exist_ok=True)
    manifest = {'schema_version': 1, 'version': release, 'revision': revision,
                'repository': 'https://github.com/laurierloi/linux-serial-test',
                'build_recipe': 'tests/build.Dockerfile and tests/build.sh',
                'build_image': Path('build/image-id').read_text().strip(),
                'build_flags': '-std=gnu11 -O2 -Wall -Wextra -Werror -static -Wl,--build-id=sha1',
                'linkage': 'static', 'binaries': {}}
    for role, arch, compiler in [('host', 'x86_64', 'gcc'), ('target', 'armhf', 'arm-linux-gnueabihf-gcc')]:
        name = 'linux-serial-test-linux-' + arch
        dest = output / name
        shutil.copyfile('build/linux-serial-test-' + role, dest)
        dest.chmod(0o755)
        data = dest.read_bytes()
        manifest['binaries'][name] = {'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data),
            'architecture': arch, 'compiler': subprocess.check_output([compiler, '--version'], text=True).splitlines()[0]}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    files = sorted(output.glob('linux-serial-test-*')) + [output / 'manifest.json']
    (output / 'SHA256SUMS').write_text(''.join(hashlib.sha256(f.read_bytes()).hexdigest() + '  ' + f.name + '\n' for f in files))


if __name__ == '__main__':
    main()
