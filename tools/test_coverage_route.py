"""Build a synthetic cartridge and exercise real native/interpreter QA routes."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import zlib

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--cc',default='gcc')
p.add_argument('--cmake',default='cmake')
p.add_argument('--ninja',default='ninja')
args=p.parse_args()
engine=Path(__file__).resolve().parents[1]
options={'creationflags':subprocess.CREATE_NO_WINDOW} if os.name=='nt' else {}

def run(command,cwd,expected=0):
    r=subprocess.run(list(map(str,command)),cwd=cwd,capture_output=True,text=True,timeout=180,**options)
    if r.returncode!=expected:raise AssertionError(f'{command}: exit {r.returncode}, wanted {expected}\n{r.stdout[-2000:]}\n{r.stderr[-2000:]}')
    return r

with tempfile.TemporaryDirectory(prefix='sms route integration ') as temporary:
    root=Path(temporary);build=root/'build'
    rom=bytearray([0xc9])*16384;rom[:4]=bytes([0xf3,0xc3,1,0]);rom[0x3ff0:0x3ff8]=b'TMR SEGA';rom[0x3fff]=0x4b
    crc=zlib.crc32(rom);(root/'fixture.sms').write_bytes(rom)
    (root/'game.toml').write_text(f'[game]\noutput_prefix="fixture"\nplatform="sms"\nrom="fixture.sms"\ncrc32=0x{crc:08x}\n',encoding='utf-8')
    (root/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.20)\nproject(RouteTest C)\ninclude("'+(engine/'cmake/SmsGame.cmake').as_posix()+'")\nsms_add_game(Synthetic)\n',encoding='utf-8')
    run([args.cmake,'-S',root,'-B',build,'-G','Ninja','-DCMAKE_BUILD_TYPE=Release','-DCMAKE_C_COMPILER='+args.cc,'-DCMAKE_MAKE_PROGRAM='+args.ninja,'-DSMSGG_ENABLE_SDL=OFF'],root)
    run([args.cmake,'--build',build,'-j','2'],root)
    exe=build/('Synthetic.exe' if os.name=='nt' else 'Synthetic')
    route=root/'route with spaces.txt'
    route.write_text(f'rom {crc:08x}\n0 pad R\n1 write C123 4A\n1 or C123 10\n1 expect C123 5A\n1 capture first capture\n2 write C124 A5\n2 capture second capture\n3 expect C123 5A\n',encoding='utf-8')
    for mode in ('hybrid','interpreter'):
        report=root/f'{mode}.json'
        run([exe,root/'fixture.sms','--route',route,'--frames','3','--report',report,*(['--interp'] if mode=='interpreter' else [])],root)
        data=json.loads(report.read_text());assert data['frame']==3 and data['total_cycles']>0 and data['dispatch_misses']==0
        assert data['interpreted_cycles']==(data['total_cycles'] if mode=='interpreter' else 0)
        ram=(root/'second capture.wram').read_bytes();assert len(ram)==8192 and ram[0x123:0x125]==b'\x5a\xa5'
        assert (root/'second capture.png').read_bytes().startswith(b'\x89PNG')
    print('PASS: ROM-bound native/interpreter route, spaced paths, memory guards and native captures')
    for body in (f'rom {crc^1:08x}\n1 pad R\n',f'rom {crc:08x}\n2 pad R\n1 pad -\n',f'rom {crc:08x}\n1 write FFFC FF\n',f'rom {crc:08x}\n1 pad XYZ\n',f'rom {crc:08x}\n1 unknown X\n'):
        route.write_text(body,encoding='utf-8');run([exe,root/'fixture.sms','--route',route],root,2)
    route.write_text(f'rom {crc:08x}\n1 expect C123 FF\n',encoding='utf-8');run([exe,root/'fixture.sms','--route',route],root,1)
    route.write_text(f'rom {crc:08x}\n3 pad R\n',encoding='utf-8');run([exe,root/'fixture.sms','--route',route,'--frames','2'],root,2)
    print('PASS: malformed/unsafe/wrong-ROM routes, unmet guard and short frame limit reject')
    wrong=bytearray(rom);wrong[-1]^=1;(root/'wrong.sms').write_bytes(wrong)
    run([exe,root/'wrong.sms','--frames','1'],root,1)
    generator=build/'sms-recompiler'/('SmsRecomp.exe' if os.name=='nt' else 'SmsRecomp')
    run([generator,root/'wrong.sms','--game',root/'game.toml'],root,1)
    run([exe,root/'fixture.sms','--gg','--frames','1'],root,2)
    print('PASS: generation/runtime ROM identity and platform mismatch rejection')
    # A deliberately uncompiled reset loop never returns before the frame limit.
    # Its real fallback execution must survive the shutdown longjmp in counters.
    stub=root/'fallback.c';stub.write_text('#include "include/sms_runtime.h"\nvoid call_by_address(uint16_t a){sms_dispatch_miss(a);}\n',encoding='utf-8')
    fallback=root/('fallback.exe' if os.name=='nt' else 'fallback')
    sources=['runner/main.c','runner/glue.c','runner/coverage_route.c','runner/video/sms_vdp.c','runner/audio/sn76489.c','runner/external/superzazu/z80.c']
    run([args.cc,'-std=c11','-O1','-I'+str(engine/'runner'),'-I'+str(engine/'runner/include'),*[engine/s for s in sources],stub,'-o',fallback],root)
    report=root/'fallback.json';run([fallback,root/'fixture.sms','--frames','3','--report',report],root)
    data=json.loads(report.read_text());assert data['fallback_calls']==1 and data['dispatch_misses']==1 and data['interpreted_cycles']==data['total_cycles']>0 and data['native_cycles']==0,data
    print('PASS: active nonreturning fallback is fully accounted at the frame limit')
    # Only bank 3's $8000 is a discovered entry. A runtime mapper value loaded
    # from WRAM must execute bank 2's real code through fallback, then bank 3's
    # generated code. Promoting bank 3 must never steal bank 2's calls.
    banked=bytearray([0xc9])*65536
    program=bytes.fromhex('f3 31 f0 df 3a 00 c2 32 ff ff cd 00 80 32 01 c1 3e 03 32 ff ff cd 00 80 32 02 c1 c3 1b 00')
    banked[:len(program)]=program
    banked[0x8000:0x8003]=bytes.fromhex('3e bb c9')
    banked[0xc000:0xc003]=bytes.fromhex('3e aa c9')
    banked[0x7ff0:0x7ff8]=b'TMR SEGA';banked[0x7fff]=0x4e
    crc=zlib.crc32(banked);(root/'fixture.sms').write_bytes(banked)
    (root/'game.toml').write_text(f'[game]\noutput_prefix="fixture"\nplatform="sms"\nrom="fixture.sms"\ncrc32=0x{crc:08x}\n',encoding='utf-8')
    fnv=2166136261
    for value in banked[0xc000:0xc100]:fnv=((fnv^value)*16777619)&0xffffffff
    (root/'dispatch_manifest.txt').write_text(f'8000 00 01 03 {fnv:08X}\n',encoding='ascii')
    run([args.cmake,'-S',root,'-B',build],root)
    run([args.cmake,'--build',build,'-j','2'],root)
    route.write_text(f'rom {crc:08x}\n0 write C200 02\n3 expect C101 BB\n3 expect C102 AA\n',encoding='ascii')
    for mode in ([],['--interp']):run([exe,root/'fixture.sms','--route',route,'--frames','3',*mode],root)
    print('PASS: a sole generated bank variant never captures another live bank')
