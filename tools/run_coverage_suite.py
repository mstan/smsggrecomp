"""Run ROM-bound assisted routes on the current host-native HLE build.

Optional previously captured reference evidence compares only listed image and
VDP surfaces. It does not establish independent hardware parity, ordinary play
feel, or complete CPU/WRAM equivalence. No reference runtime is launched.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import zlib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('runtime', 'rom', 'suite', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--case', action='append', help='Only this suite case (repeatable)')
    parser.add_argument('--reference', type=Path, help='Existing matching report.json; evidence only')
    parser.add_argument('--reference-mode', choices=('native', 'interpreter'), default='native',
                        help='Existing evidence directory; never starts that runtime')
    args = parser.parse_args()
    runtime, rom, suite_path, out = (getattr(args, key).resolve() for key in ('runtime', 'rom', 'suite', 'out'))
    suite = json.loads(suite_path.read_text(encoding='utf-8'))
    if rom.stat().st_size > 16 * 1024 * 1024 + 512:
        raise SystemExit('Unsupported suite ROM extent')
    original = rom.read_bytes()
    data = original[512:] if len(original) % 16384 == 512 else original
    if (f'{zlib.crc32(data):08X}', len(data)) != (suite['rom_crc32'].upper(), suite['rom_size']):
        raise SystemExit('Unsupported suite ROM')
    names = [case['name'] for case in suite['cases']]
    if len(names) != len(set(names)) or any(not n or Path(n).name != n or n in ('.', '..') for n in names):
        raise SystemExit('Invalid or duplicate case name')
    if args.case and set(args.case) - set(names):
        raise SystemExit('Unknown requested case')
    report = dict(schema=2, rom_sha256=hashlib.sha256(original).hexdigest(),
                  normalized_sha256=hashlib.sha256(data).hexdigest(),
                  runtime_sha256=hashlib.sha256(runtime.read_bytes()).hexdigest(),
                  suite_sha256=hashlib.sha256(suite_path.read_bytes()).hexdigest(),
                  cases=[], scope=__doc__.strip())
    reference = None
    if args.reference:
        reference = json.loads(args.reference.read_text(encoding='utf-8'))
        if reference['rom_sha256'] != report['rom_sha256']:
            raise SystemExit('Reference belongs to another original image')
        report['reference'] = dict(report_sha256=hashlib.sha256(args.reference.read_bytes()).hexdigest(),
                                   runtime_sha256=reference['runtime_sha256'], mode=args.reference_mode)
    out.mkdir(parents=True, exist_ok=False)
    environment = dict(os.environ, SDL_AUDIODRIVER='dummy', SDL_VIDEODRIVER='dummy')
    options = dict(creationflags=subprocess.CREATE_NO_WINDOW) if os.name == 'nt' else {}
    for case in suite['cases']:
        name = case['name']
        if args.case and name not in args.case:
            continue
        source = (suite_path.parent / case['route']).resolve()
        if not source.is_relative_to(suite_path.parent):
            raise ValueError('Route escapes suite directory')
        captures = [parts[2] for line in source.read_text().splitlines()
                    if len(parts := line.split(maxsplit=2)) == 3 and parts[1] == 'capture']
        if any(Path(c).name != c or c in ('.', '..') for c in captures):
            raise ValueError('Capture escapes case directory')
        directory = out / name / 'native'
        directory.mkdir(parents=True)
        route = directory / 'route.txt'
        shutil.copyfile(source, route)
        command = [str(runtime), str(rom), '--no-launcher', '--mute', '--route', str(route),
                   '--frames', str(case['frames']), '--report', str(directory / 'execution.json'),
                   '--audio-wav', str(directory / 'audio.wav')]
        with (directory / 'run.log').open('w', encoding='utf-8') as log:
            subprocess.run(command, cwd=directory, env=environment, stdout=log,
                           stderr=subprocess.STDOUT, check=True, **options)
        execution = json.loads((directory / 'execution.json').read_text())
        row = dict(name=name, route_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                   modes=dict(native=execution), comparisons=[], captures=[])
        for capture in captures:
            planes = {}
            for suffix in ('.png', '.vram', '.cram', '.regs'):
                current = (directory / (capture + suffix)).read_bytes()
                plane = dict(sha256=hashlib.sha256(current).hexdigest())
                if reference:
                    old_case = next((c for c in reference['cases'] if c['name'] == name), None)
                    if not old_case:
                        raise ValueError('Reference lacks requested case: ' + name)
                    if old_case.get('route_sha256', row['route_sha256']) != row['route_sha256']:
                        raise ValueError('Reference route differs: ' + name)
                    previous = (args.reference.parent / name / args.reference_mode / (capture + suffix)).read_bytes()
                    plane.update(equal=current == previous, reference_sha256=hashlib.sha256(previous).hexdigest())
                planes[suffix] = plane
            row['captures'].append(dict(capture=capture, planes=planes))
            if reference:
                row['comparisons'].append(dict(capture=capture, planes=planes))
        row['zero_fallback'] = execution['dispatch_misses'] == 0 and execution['interpreted_cycles'] == 0
        report['cases'].append(row)
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(name, 'HLE coverage:', row['zero_fallback'], 'captures:', len(captures), flush=True)
    if not all(c['zero_fallback'] and all(all(p['equal'] for p in x['planes'].values())
           for x in c['comparisons']) for c in report['cases']):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
