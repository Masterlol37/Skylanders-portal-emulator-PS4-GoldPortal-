"""End-to-end test: PC client <-> mock PS4 (with a simulated game). Run: python test_portal.py"""
import os
import sys
import tempfile
import threading
import time
import unittest
from http.server import ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mock_ps4  # noqa: E402
from goldportal_pc import (PortalClient, PortalError, figure_info, write_back,  # noqa: E402
                          scan_library, index_by_name, AutoSaver)


def fake_dump(fig_id=0x1CE, variant=0x3000):
    d = bytearray(1024)
    d[0x10:0x12] = fig_id.to_bytes(2, "little")
    d[0x1C:0x1E] = variant.to_bytes(2, "little")
    for i in range(0x20, 1024):
        d[i] = i & 0xFF
    return bytes(d)


class ProtocolTest(unittest.TestCase):
    def test_status_transitions_and_reads(self):
        p = mock_ps4.Portal()
        p.command(b"A\x01" + bytes(30))
        rep, is_reply = p.report()
        self.assertTrue(is_reply)
        self.assertEqual(rep[:4], bytes([0x41, 1, 0xFF, 0x77]))

        p.load(2, fake_dump(), "x.dump")
        seq = []
        for _ in range(3):
            rep, _ = p.report()
            seq.append((int.from_bytes(rep[1:5], "little") >> 4) & 3)
        self.assertEqual(seq, [3, 1, 1])       # added -> present -> present
        self.assertEqual(rep[6], 1)            # active flag

        p.command(bytes([ord("Q"), 0x12, 1]) + bytes(29))
        rep, _ = p.report()
        self.assertEqual(rep[:3], bytes([ord("Q"), 0x12, 1]))
        self.assertEqual(rep[3:19], fake_dump()[16:32])

        p.command(bytes([ord("Q"), 0x15, 1]) + bytes(29))   # empty slot
        self.assertEqual(p.report()[0][:3], bytes([ord("Q"), 0x05, 1]))

        p.remove(2)
        seq = [(int.from_bytes(p.report()[0][1:5], "little") >> 4) & 3 for _ in range(3)]
        self.assertEqual(seq, [2, 0, 0])

    def test_figure_info(self):
        self.assertEqual(figure_info(fake_dump(462, 0x3000)), (462, 0x3000))


class EndToEndTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.portal = mock_ps4.Portal()
        cls.srv = ThreadingHTTPServer(("127.0.0.1", 0), mock_ps4.make_handler(cls.portal, b"<html>"))
        mock_ps4.make_handler  # silence linters
        threading.Thread(target=cls.srv.serve_forever, daemon=True).start()
        threading.Thread(target=mock_ps4.simulate_game, args=(cls.portal,), daemon=True).start()
        cls.client = PortalClient("127.0.0.1", cls.srv.server_address[1])

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()

    def test_load_play_save_remove(self):
        c = self.client
        c.load(0, fake_dump(), "Spyro.dump")
        st = c.status()
        self.assertTrue(st["slots"][0]["loaded"])
        self.assertEqual(st["slots"][0]["id"], 0x1CE)
        self.assertEqual(st["slots"][0]["name"], "Spyro.dump")

        for _ in range(60):                    # wait for the "game" to write to it
            if c.status()["slots"][0]["dirty"]:
                break
            time.sleep(0.05)
        self.assertTrue(c.status()["active"])
        self.assertTrue(c.status()["slots"][0]["dirty"])

        data = c.dump(0)
        self.assertEqual(len(data), 1024)
        self.assertEqual(data[8 * 16:9 * 16], b"GAME-WAS-HERE!!!")

        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "Spyro.dump")
            with open(path, "wb") as f:
                f.write(fake_dump())
            write_back(path, data)
            with open(path, "rb") as f:
                self.assertEqual(f.read(), data)
            with open(path + ".bak", "rb") as f:
                self.assertEqual(f.read(), fake_dump())

        c.remove(0)
        self.assertFalse(c.status()["slots"][0]["loaded"])

    def test_errors(self):
        with self.assertRaises(PortalError):
            self.client.load(0, b"short", "bad.dump")
        with self.assertRaises(PortalError):
            self.client._req("POST", "/api/remove", {"slot": 9})
        with self.assertRaises(PortalError):
            PortalClient("127.0.0.1", 1, timeout=1).status()


class LibraryTest(unittest.TestCase):
    def test_scan_nested_folders(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "4. Trap Team", "7) Traps"))
            os.makedirs(os.path.join(d, "empty folder"))
            for rel in ["4. Trap Team/7) Traps/Air Hourglass.dump", "1. Spyro/Spyro.dump"]:
                os.makedirs(os.path.dirname(os.path.join(d, rel)), exist_ok=True)
                with open(os.path.join(d, rel), "wb") as f:
                    f.write(fake_dump())
            with open(os.path.join(d, "notes.txt"), "w") as f:
                f.write("x")
            tree = scan_library(d)
            subs, files = tree[d]
            self.assertEqual([os.path.basename(s) for s in subs], ["1. Spyro", "4. Trap Team"])
            self.assertEqual(files, [])                         # notes.txt ignored
            self.assertNotIn(os.path.join(d, "empty folder"), tree)
            traps = tree[os.path.join(d, "4. Trap Team", "7) Traps")][1]
            self.assertEqual([os.path.basename(p) for p in traps], ["Air Hourglass.dump"])
            self.assertIn("spyro.dump", index_by_name(tree))

    def test_natural_sort(self):
        with tempfile.TemporaryDirectory() as d:
            for name in ["10) Unreleased", "2) New", "1) Trap Masters"]:
                os.makedirs(os.path.join(d, name))
                with open(os.path.join(d, name, "x.dump"), "wb") as f:
                    f.write(fake_dump())
            subs = [os.path.basename(s) for s in scan_library(d)[d][0]]
            self.assertEqual(subs, ["1) Trap Masters", "2) New", "10) Unreleased"])


class AutoSaveTest(unittest.TestCase):
    def setUp(self):
        self.portal = mock_ps4.Portal()
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), mock_ps4.make_handler(self.portal, b"<html>"))
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        threading.Thread(target=mock_ps4.simulate_game, args=(self.portal,), daemon=True).start()
        self.client = PortalClient("127.0.0.1", self.srv.server_address[1])
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.srv.shutdown()
        self.tmp.cleanup()

    def wait_dirty(self, slot):
        for _ in range(100):
            if self.client.status()["slots"][slot]["dirty"]:
                return
            time.sleep(0.05)
        self.fail("game never wrote to the figure")

    def test_autosave_writes_back_once_and_keeps_original_bak(self):
        path = os.path.join(self.tmp.name, "Spyro.dump")
        with open(path, "wb") as f:
            f.write(fake_dump())
        saver = AutoSaver(settle_seconds=0.05)
        self.client.load(0, fake_dump(), "Spyro.dump")
        saver.placed(0, path, fake_dump())
        self.wait_dirty(0)

        self.assertEqual(saver.save_all(self.client, {}), [path])
        with open(path, "rb") as f:
            self.assertEqual(f.read()[8 * 16:9 * 16], b"GAME-WAS-HERE!!!")
        with open(path + ".bak", "rb") as f:
            self.assertEqual(f.read(), fake_dump())             # original kept

        self.assertEqual(saver.save_all(self.client, {}), [])   # unchanged: no rewrite
        # a later change is saved again, but the .bak stays the pre-session original
        self.portal.command(bytes([ord("W"), 0x10, 9]) + b"SECOND-SAVE-OK!!" + bytes(13))
        self.assertEqual(saver.save_all(self.client, {}), [path])
        with open(path + ".bak", "rb") as f:
            self.assertEqual(f.read(), fake_dump())

    def test_figure_placed_elsewhere_is_found_by_name(self):
        folder = os.path.join(self.tmp.name, "1. Spyro's Adventure")
        os.makedirs(folder)
        path = os.path.join(folder, "Spyro.dump")
        with open(path, "wb") as f:
            f.write(fake_dump())
        self.client.load(1, fake_dump(), "Spyro.dump")          # not placed through the saver
        self.wait_dirty(1)
        saver = AutoSaver(settle_seconds=0.05)
        index = index_by_name(scan_library(self.tmp.name))
        self.assertEqual(saver.save_all(self.client, index), [path])


class WebPageTest(unittest.TestCase):
    def test_embedded_page_extracts(self):
        html = mock_ps4.load_index_html()
        self.assertIn(b"/api/status", html)
        self.assertIn(b"</html>", html)


if __name__ == "__main__":
    unittest.main(verbosity=2)
