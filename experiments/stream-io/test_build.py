#!/usr/bin/env python3
import tempfile
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
            result=build.prepare(source_path(),Path(t)/'new')
            self.assertTrue(result.startswith('/*'))
if __name__=='__main__':unittest.main()
