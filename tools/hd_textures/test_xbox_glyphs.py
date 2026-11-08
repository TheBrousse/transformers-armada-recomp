"""No disc needed: generated glyphs stay inside cells and PNG bytes repeat exactly."""
import io
import unittest
import numpy as np
import make_crisp_fonts as fonts

class XboxGlyphTests(unittest.TestCase):
    def test_cells_and_determinism(self):
        for kind in fonts.XBOX_LABELS:
            for w, h in ((48, 64), (128, 64), (13, 19)):
                with self.subTest(kind=kind, size=(w,h)):
                    a = fonts.draw_xbox(kind, w, h, (3,3,w-3,h-3), .75)
                    b = fonts.draw_xbox(kind, w, h, (3,3,w-3,h-3), .75)
                    self.assertEqual(a.shape, (h,w,4))
                    self.assertTrue(np.isfinite(a).all())
                    self.assertGreater(a[...,3].max(), 0)
                    self.assertLessEqual(a[...,3].max(), .75)
                    self.assertFalse(a[:3,:,3].any())
                    self.assertFalse(a[-3:,:,3].any())
                    self.assertFalse(a[:,:3,3].any())
                    self.assertFalse(a[:,-3:,3].any())
                    canvas = np.zeros((h+12,w+14,4), dtype=np.float32)
                    canvas[5:5+h,7:7+w] = a
                    self.assertFalse(canvas[:5].any())
                    self.assertFalse(canvas[5+h:].any())
                    self.assertFalse(canvas[:,:7].any())
                    self.assertFalse(canvas[:,7+w:].any())
                    def png(arr):
                        data = io.BytesIO()
                        fonts.to_image(arr).save(data, format='PNG', optimize=False)
                        return data.getvalue()
                    self.assertEqual(png(a), png(b))

if __name__ == '__main__':
    unittest.main()
