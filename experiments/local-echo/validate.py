#!/usr/bin/env python3
"""Offline validation only: compiler, local socket pairs and pre-I/O refusals."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import patch

OUTPUT_SHA256 = '1fb71c7cfd19114f71eee87ce0455b5d8cf9445508f4bc83af31918f963978a0'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate(source, output):
    own = Path(__file__).resolve().parent
    source, output = source.absolute(), output.absolute()
    inputs = [source] + [own / n for n in ('local_echo.h', 'patch.py', 'validate.py',
                                          'test_reference.c', 'test_helper.c')]
    inputs.append(own.parent / 'stream-io/test_integration.c')
    if any(p.is_symlink() or not p.is_file() for p in inputs):
        raise ValueError('regular source inputs required')
    pins = {str(p): sha(p) for p in inputs}
    base = source.read_bytes()
    candidate = patch.transform(base)
    if hashlib.sha256(candidate.encode()).hexdigest() != OUTPUT_SHA256:
        raise ValueError('candidate differs from reviewed source')
    # Actual legacy helper and all broker functions are untouched.
    old = base.decode()
    for left, right in [
        ('static int helper_main(', '/* ---------------------------------------------------------------- broker --'),
        ('static int relay_stream_and_watch(', 'int main(int argc, char **argv)')]:
        assert old[old.index(left):old.index(right)] == candidate[candidate.index(left):candidate.index(right)]
    for bad in (base+b' ', base.replace(b'static int helper_stream_main(', b'static int changed_helper(', 1)):
        try:
            patch.transform(bad)
        except ValueError:
            pass
        else:
            raise AssertionError('source drift accepted')
    output.mkdir(mode=0o700)  # Existing destinations, including symlinks, refuse.
    generated = output / 'bridge.c'
    generated.write_text(candidate)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('LITENET_', 'SLMBRIDGE_'))}
    records = []

    def run(args, extra=None, status=0):
        r = subprocess.run([str(x) for x in args], env=dict(env, **(extra or {})),
                           capture_output=True, text=True, timeout=90)
        record = {'argv': [str(x) for x in args], 'returncode': r.returncode,
                  'stdout': r.stdout, 'stderr': r.stderr}
        records.append(record)
        (output / 'commands.json').write_text(json.dumps(records, indent=2)+'\n')
        if r.returncode != status:
            raise RuntimeError('offline command failed; see private commands.json')
        return r

    cc = os.environ.get('CC', 'cc')
    flags = ['-O1', '-g', '-fsanitize=address,undefined']
    run([cc, *flags, '-Wall', '-Wextra', '-Werror', own/'test_reference.c', '-o', output/'reference-test'])
    run([output/'reference-test'])
    run([cc, *flags, '-DBRIDGE_TEST_SOURCE="'+str(generated)+'"', own/'test_helper.c',
         '-o', output/'helper-test', '-pthread', '-lm'])
    helper = run([output/'helper-test'])
    if helper.stdout.count('PASS real helper ') != 14:
        raise ValueError('not all fourteen real echo helper cases completed')
    if platform.system() == 'Linux' and 'SKIP' in helper.stdout:
        raise ValueError('Linux transport integration was skipped')
    run([cc, *flags, generated, '-o', output/'bridge', '-pthread', '-lm'])
    for mode, transport, clock in [('bad','1','rx'), ('','1','rx'),
                                    ('observe','0','rx'), ('mix','1','timer'), ('mix','1','')]:
        args = {'SLMBRIDGE_LOCAL_ECHO':mode, 'SLMBRIDGE_STREAM_IO':transport,
                'SLMBRIDGE_TX_CLOCK':clock}
        r = run([output/'bridge'], args, 1)
        if 'bridge_local_echo: INSTRUMENT ERROR' not in r.stderr:
            raise ValueError('invalid config did not fail at the pre-I/O guard')
    if pins != {str(p): sha(p) for p in inputs} or sha(generated) != OUTPUT_SHA256:
        raise ValueError('source changed during offline validation')
    manifest = {'profile':'public', 'platform':platform.platform(), 'source_pins':pins,
                'candidate_source_sha256':sha(generated), 'candidate_binary_sha256':sha(output/'bridge'),
                'compiler':run([cc,'--version']).stdout.splitlines()[0],
                'reference_sanitizers_passed':True, 'actual_echo_helper_cases':14,
                'pre_io_refusals':5, 'legacy_helper_and_brokers_source_unchanged':True,
                'legacy_transport_suite_passed':platform.system()=='Linux',
                'input_hashes_unchanged':True, 'physical_validation':False,
                'scope':'Offline generated data over AF_UNIX only; no listener, TTY, modem or service actions.'}
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return manifest


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    print(json.dumps(validate(a.source,a.output),indent=2))
