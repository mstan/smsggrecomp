#!/usr/bin/env python3
"""Client for the smsref TCP oracle (Phase B). Line-delimited JSON.

  python smsref_client.py PORT dump FRAME PREFIX   # run_to FRAME, write .vram/.cram/.cpu
  python smsref_client.py PORT cmd '{"cmd":"state"}'

Dump writes PREFIX.vram (16KB), PREFIX.cram (32B), PREFIX.cpu (recomp key=val
format) so tools/oracle/{vdp_diff,state_diff}.py consume it unchanged.
"""
import socket, json, sys

CPU_KEYS = ["a","f","b","c","d","e","h","l","ix","iy","sp","pc","wz",
            "i","r","iff1","iff2","im","halted"]


def rpc(sock, obj):
    sock.sendall((json.dumps(obj) + "\n").encode())
    buf = b""
    while not buf.endswith(b"\n"):
        d = sock.recv(65536)
        if not d:
            break
        buf += d
    return json.loads(buf.decode())


def main():
    port = int(sys.argv[1]); op = sys.argv[2]
    s = socket.create_connection(("127.0.0.1", port), timeout=30)
    if op == "dump":
        frame, prefix = int(sys.argv[3]), sys.argv[4]
        rpc(s, {"cmd": "run_to", "frame": frame})
        open(prefix + ".vram", "wb").write(bytes.fromhex(rpc(s, {"cmd": "read_vram"})["vram"]))
        open(prefix + ".cram", "wb").write(bytes.fromhex(rpc(s, {"cmd": "read_cram"})["cram"]))
        r = rpc(s, {"cmd": "regs"})
        with open(prefix + ".cpu", "w") as f:
            for k in CPU_KEYS:
                f.write(f"{k}={r[k]}\n")
        print(f"dumped frame {frame} -> {prefix}.{{vram,cram,cpu}}")
    elif op == "cmd":
        print(json.dumps(rpc(s, json.loads(sys.argv[3]))))
    s.close()


if __name__ == "__main__":
    sys.exit(main())
