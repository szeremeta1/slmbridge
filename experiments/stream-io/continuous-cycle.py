"""Generated AF_UNIX data and test-owned helper child only, at 20ms pacing."""
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import sys
import time

def fill(s, byte):
    n = 0
    while True:
        try:
            n += s.send(bytes([byte]) * 4096, socket.MSG_DONTWAIT)
        except BlockingIOError:
            return n

def fixture_environment(rate):
    env = {k:v for k,v in os.environ.items() if not k.startswith('SLMBRIDGE_')}
    fixed = {'DSP_RATE':str(rate), 'TX_CLOCK':'rx', 'RING_FRAMES':'8', 'RX_PREFILL':'8',
             'RS_DUMP':'', 'RS_PROFILE':'', 'RS_FC':'3800', 'ELASTIC':'0', 'RXGAP_LOG_MS':'0',
             'STREAM_IO':'1'}
    # Each helper setting has an explicit public value, including empty
    # defaults, so neither ambient values nor compatibility aliases select it.
    env.update({'SLMBRIDGE_'+k:v for k,v in fixed.items()})
    return env

def run(payload_len, small=False, rate=8000):
    a, ah = socket.socketpair()
    p, ph = socket.socketpair()
    if small:
        p.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4096)
        ph.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4096)
    fill_byte, frame_byte = (0, 0) if rate == 9600 else (0xA5, 0x39)
    dsp_backlog, helper_backlog = fill(p, fill_byte), fill(ph, fill_byte)
    env = fixture_environment(rate)
    proc = subprocess.Popen([str(Path(__file__).with_name('helper-only')), str(ph.fileno()), str(ah.fileno())],
                            env=env, pass_fds=[ph.fileno(), ah.fileno()], stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    ph.close(); ah.close()
    a.setblocking(False); p.setblocking(False)
    frame = bytes([0x10, payload_len >> 8, payload_len & 255]) + bytes([frame_byte]) * payload_len
    began = time.monotonic(); due = began
    sent = got = read_pcm = offset = 0
    raw = b''; writer_unblocked = False; done = False; peer_closed = False
    try:
        while time.monotonic() - began < 14 and proc.poll() is None:
            now = time.monotonic()
            if not writer_unblocked and now >= due:
                try:
                    offset += a.send(frame[offset:])
                    if offset == len(frame):
                        sent += 1; offset = 0; due += .02
                except BlockingIOError:
                    pass
                except (BrokenPipeError, ConnectionResetError):
                    peer_closed = True; break
            if not writer_unblocked and select.select([], [p], [], 0)[1]:
                writer_unblocked = True
            if writer_unblocked:
                try:
                    chunk = p.recv(65536)
                    if not chunk: peer_closed = True; break
                    expected = bytes([fill_byte]) * max(0, min(len(chunk), helper_backlog-read_pcm))
                    expected += bytes([frame_byte]) * (len(chunk)-len(expected))
                    assert chunk == expected, 'PCM byte stream changed'
                    read_pcm += len(chunk)
                except BlockingIOError:
                    pass
            try:
                chunk = a.recv(65536)
                if not chunk: peer_closed = True; break
                raw += chunk
                while len(raw) >= 323:
                    assert raw[:3] == b'\x10\x01\x40'
                    expected = bytes(320) if got < 8 else bytes([fill_byte])*320
                    assert raw[3:323] == expected, 'ring output bytes changed'
                    raw = raw[323:]; got += 1
            except BlockingIOError:
                pass
            except ConnectionResetError:
                peer_closed = True; break
            pcm_target = sent*payload_len if rate == 8000 else ((sent*payload_len//2*6+4)//5)*2
            if writer_unblocked and not offset and got == sent and read_pcm == helper_backlog + pcm_target:
                a.send(b'\0\0\0'); done = True; break
            time.sleep(.001)
        if not done and not peer_closed and proc.poll() is None:
            proc.terminate()
        _, stderr = proc.communicate(timeout=3)
    finally:
        if proc.poll() is None:
            proc.kill(); proc.wait()
        a.close(); p.close()
    result = dict(rate=rate, payload_len=payload_len, requested_small_buffer=small, dsp_backlog=dsp_backlog,
                  helper_backlog=helper_backlog, sent_frames=sent, output_frames=got,
                  seconds=round(time.monotonic()-began,3), writer_unblocked=writer_unblocked,
                  complete_exactly=done and proc.returncode == 0, returncode=proc.returncode,
                  stderr=stderr.decode())
    print(json.dumps(result), flush=True)

if __name__ == '__main__':
    run(int(sys.argv[1]) if len(sys.argv) > 1 else 320,
        len(sys.argv) > 2 and sys.argv[2] == 'small', int(sys.argv[3]) if len(sys.argv) > 3 else 8000)
