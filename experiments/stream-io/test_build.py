#!/usr/bin/env python3
import tempfile
import importlib.util
import os
import unittest
from pathlib import Path
from unittest.mock import patch as mock
import build
import patch

OWN=Path(__file__).resolve().parent
def source_path():
    return OWN/'pristine.c' if (OWN/'pristine.c').is_file() else OWN.parents[1]/'slmbridge.c'
class Checks(unittest.TestCase):
    def test_off_functions_and_resampler_preserved(self):
        original=source_path().read_text();new=patch.patch(original)
        a=original.index('static int helper_main(');b=original.index('/* ---------------------------------------------------------------- broker --')
        helper_original=original[a:b]
        needle='static int helper_main(int pcm_fd, int as_fd)\n{'
        dispatch='\n    int stream_io = bio_enabled();\n    if (stream_io < 0) return 1;\n    if (stream_io) return helper_stream_main(pcm_fd, as_fd);'
        self.assertIn(helper_original.replace(needle,needle+dispatch),new)
        a=original.index('static int relay_and_watch(');b=original.index('static int broker_main(')
        broker_original=original[a:b];needle='static int relay_and_watch(int a, int b, int *tfdp)\n{'
        dispatch='\n    int stream_io = bio_enabled();\n    if (stream_io < 0) return -1;\n    if (stream_io) return relay_stream_and_watch(a, b, tfdp);'
        self.assertIn(broker_original.replace(needle,needle+dispatch),new)
        start=original.index('static int rs_process(');end=original.index('static void rs_dump(')
        self.assertIn(original[start:end],new)
        self.assertNotIn('waste[',patch.helper(original))
        with self.assertRaises(ValueError):patch.patch(new)
    def test_refuse_source_drift_and_symlink(self):
        with tempfile.TemporaryDirectory() as t,mock('platform.system',return_value='Linux'):
            t=Path(t);source=t/'source.c';source.write_text('wrong');dest=t/'output'
            with self.assertRaisesRegex(ValueError,'source hash'):build.prepare(source,dest)
            self.assertFalse(dest.exists())
            source.unlink();source.symlink_to('/dev/null')
            with self.assertRaisesRegex(ValueError,'regular source'):build.prepare(source,dest)
    def test_refuse_existing_output_and_patch_drift(self):
        with tempfile.TemporaryDirectory() as t,mock('platform.system',return_value='Linux'):
            dest=Path(t)/'new';dest.mkdir()
            with self.assertRaisesRegex(ValueError,'new output'):build.prepare(source_path(),dest)
            dest.rmdir()
            with mock('patch.patch',return_value='different'):
                with self.assertRaisesRegex(ValueError,'reviewed candidate'):build.prepare(source_path(),dest)
            self.assertFalse(dest.exists())
    def test_exact_reviewed_transform(self):
        with tempfile.TemporaryDirectory() as t,mock('platform.system',return_value='Linux'):
            original,result=build.prepare(source_path(),Path(t)/'new')
            self.assertEqual(original,source_path().read_bytes())
            self.assertTrue(result.startswith('/*'))

    def test_bounded_commands_and_changed_inputs(self):
        with mock('build.subprocess.run') as invoke:
            invoke.return_value.returncode=0
            invoke.return_value.stdout=invoke.return_value.stderr=''
            build.execute(['true'],OWN)
            self.assertEqual(invoke.call_args.kwargs['timeout'],90)
        with tempfile.TemporaryDirectory() as t:
            p=Path(t)/'source';p.write_text('before')
            expected={p:build.sha(p)};p.write_text('after')
            with self.assertRaisesRegex(ValueError,'input changed'):build.verify_inputs(expected)
            p.unlink();p.symlink_to(source_path())
            with self.assertRaisesRegex(ValueError,'input changed'):build.verify_inputs(expected)

    def test_paced_fixture_pins_all_helper_controls(self):
        spec=importlib.util.spec_from_file_location('paced_fixture',OWN/'continuous-cycle.py')
        fixture=importlib.util.module_from_spec(spec);spec.loader.exec_module(fixture)
        fixed={'DSP_RATE':'9600','TX_CLOCK':'rx','RING_FRAMES':'8','RX_PREFILL':'8',
               'RS_DUMP':'','RS_PROFILE':'','RS_FC':'3800','ELASTIC':'0','RXGAP_LOG_MS':'0','STREAM_IO':'1'}
        hostile={'SLMBRIDGE_'+key:'unreviewed' for key in fixed}
        hostile.update(SLMBRIDGE_EXTRA_CONTROL='unreviewed',PATH='/fixture')
        with mock.dict(os.environ,hostile,clear=True):result=fixture.fixture_environment(9600)
        self.assertEqual(result,{'PATH':'/fixture',**{'SLMBRIDGE_'+key:value for key,value in fixed.items()}})
if __name__=='__main__':unittest.main()
