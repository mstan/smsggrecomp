# Native content-provider fixture

`make_content_fixture.py` creates synthetic cartridge images, descriptor ZIPs
and `fixtures/fixture_identity.h`. No retail image is used.

Compile `content_contract.cpp` as a C++17 executable linked to the existing
`smsgg_runtime` target, adding the generated `fixtures` directory to its include
path. The fixture supplies its own `GameSpec`, trusted callback and CPU-entry
stub; it never executes guest code or opens a window/audio device.

Run the executable in a fresh writable directory containing the generated
`.sms` and `.smsmod` inputs. Then run the same executable with `--verify-saved`
in that directory. The first process checks byte overlays, expected/target
guards, conflicting claims, fingerprint stability and callback rollback; the
second reloads saved feature/source choices and revalidates the original
donor image. Each independent test run needs its own fresh directory.
