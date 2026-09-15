import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("compare", Path(__file__).with_name("compare.py"))
compare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compare)


class CompareTest(unittest.TestCase):
    def test_planes_and_macroblocks(self):
        w, h = 34, 18
        with tempfile.TemporaryDirectory() as root:
            a, b = Path(root)/"a", Path(root)/"b"
            data = bytearray(w*h*3//2 * 2)
            a.write_bytes(data)
            for offset, expected in [(17 + w*16, "plane=Y pixel=(17,16) macroblock=(1,1)"),
                                     (w*h+9, "plane=U pixel=(9,0) macroblock=(1,0)"),
                                     (w*h*5//4+17*8, "plane=V pixel=(0,8) macroblock=(0,1)")]:
                changed = bytearray(data)
                changed[len(data)//2 + offset] = 5
                b.write_bytes(changed)
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    self.assertEqual(compare.compare_raw(a, b, w, h), 1)
                self.assertIn("frame=1", output.getvalue())
                self.assertIn(expected, output.getvalue())
            b.write_bytes(data)
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(compare.compare_raw(a, b, w, h), 0)

    def test_md5(self):
        with tempfile.TemporaryDirectory() as root:
            a, b = Path(root)/"a", Path(root)/"b"
            a.write_text("#header\n0, 0, 0, 1, 20, hash\n")
            b.write_text("#other header\n0, 0, 0, 1, 20, hash\n")
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(compare.compare_md5(a, b), 0)
            b.write_text("")
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(compare.compare_md5(a, b), 1)
