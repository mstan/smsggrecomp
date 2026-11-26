import hashlib,zipfile,zlib
from pathlib import Path
root=Path(__file__).resolve().parent/'fixtures';root.mkdir(exist_ok=True)
target=bytearray(16384);target[0x300:0x304]=bytes.fromhex('11223344');target[0x3ff0:0x3ff8]=b'TMR SEGA'
donor=bytearray(16384);donor[0x200:0x204]=bytes.fromhex('deadbeef');donor[0x3ff0:0x3ff8]=b'TMR SEGA'
small=bytearray(8192);small[0x200:0x204]=bytes.fromhex('cafebabe');small[0x1ff0:0x1ff8]=b'TMR SEGA'
(root/'target.sms').write_bytes(target);(root/'donor.sms').write_bytes(donor);(root/'same-donor-other-path.sms').write_bytes(donor)
target_sha=hashlib.sha256(target).hexdigest();donor_sha=hashlib.sha256(donor).hexdigest()
(root/'donor-é.sms').write_bytes(small);small_sha=hashlib.sha256(small).hexdigest()
(root/'fixture_identity.h').write_text(f'#define FIXTURE_CRC 0x{zlib.crc32(target):08x}u\n#define FIXTURE_SHA "{target_sha}"\n#define DONOR_SHA "{donor_sha}"\n#define SMALL_DONOR_SHA "{small_sha}"\n')
for ident,hooks in [('fixture.good',['console.start-pause','fixture.increment-counter']),('fixture.rollback',['console.start-pause','fixture.increment-counter','fixture.reject'])]:
 descriptor=f'''format_version=1
id="{ident}"
name="Synthetic original-image overlay"
description="Synthetic donor window, guarded cartridge bytes and trusted native callback"
game_id="fixture"
target_sha256="{target_sha}"
[overlay]
original_sha256="{donor_sha}"
original_size=16384
source_offset=512
target_space="cartridge"
target_offset=768
size=4
expected_hex="11223344"
'''+''.join(f'[[hook]]\nid="{hook}"\n' for hook in hooks)
 with zipfile.ZipFile(root/(ident+'.smsmod'),'w',compression=zipfile.ZIP_DEFLATED) as archive:archive.writestr('overlay.toml',descriptor)
descriptor=f'''format_version=1
id="fixture.8k"
name="Synthetic8 KiB original-image RAM overlay"
game_id="fixture"
target_sha256="{target_sha}"
[overlay]
original_sha256="{small_sha}"
original_size=8192
source_offset=512
target_space="ram"
target_offset=256
size=4
expected_hex="00000000"
'''
with zipfile.ZipFile(root/'fixture.8k.smsmod','w',compression=zipfile.ZIP_DEFLATED) as archive:archive.writestr('overlay.toml',descriptor)
with zipfile.ZipFile(root/'fixture.invalid.smsmod','w',compression=zipfile.ZIP_DEFLATED) as archive:archive.writestr('overlay.toml','format_version=99\nid="invalid"\n')
print('Synthetic original-image/descriptor fixtures prepared')
