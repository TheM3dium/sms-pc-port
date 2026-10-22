"""get.py eclipse with a file you already have: --eclipse-iso (an Eclipse image,
checked by its MD5 and linked or copied into mods/eclipse) and --archive (the
release you downloaded yourself, checked and used instead of downloading it,
and never deleted). Small stand-ins take the place of the discs, with the
checksums set to theirs."""
import contextlib
import hashlib
import importlib.util
import io
import os
import pathlib
import tempfile
import unittest

GET = pathlib.Path(__file__).resolve().parents[1] / 'get.py'
SUNSHINE = b'the Super Mario Sunshine disc'
ECLIPSE = b'the Super Mario Eclipse v1.1.0 image'
RELEASE = b'the Eclipse release archive'


def load_get():
    spec = importlib.util.spec_from_file_location('sms_get', GET)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def md5(data):
    return hashlib.md5(data).hexdigest()


class EclipseImportTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='sms get eclipse import ')
        root = pathlib.Path(self.tmp.name)
        self.mine, self.mods = root / 'mine', root / 'port' / 'mods'
        self.mine.mkdir()
        self.mods.mkdir(parents=True)
        self.get = load_get()
        self.get.ROOT = str(root / 'port')
        self.get.MODS, self.get.DOWNLOADS = str(self.mods), str(self.mods / '.downloads')
        self.install = self.mods / 'eclipse'
        self.get.ECLIPSE.update(install=str(self.install), source_md5=md5(SUNSHINE), result_md5=md5(ECLIPSE),
                                md5=md5(RELEASE), size=1, patch_size=1)
        self.out = self.install / self.get.ECLIPSE['iso']
        self.maps = 0
        self.fetched = []

        def maps(keep, if_outdated=False):
            self.maps += 1
        self.get.get_eclipse_maps = maps
        self.get.fetch = lambda mod: self.fetched.append(mod) or self.fail('downloaded the release')

    def tearDown(self):
        self.tmp.cleanup()

    def mine_file(self, name, data):
        path = self.mine / name
        path.write_bytes(data)
        return str(path)

    def run_get(self, *args, **kwargs):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.get.get_eclipse(False, *args, **kwargs)
        return out.getvalue()

    def assert_fails(self, text, *args, **kwargs):
        with self.assertRaises(self.get.Failure) as caught:
            self.run_get(*args, **kwargs)
        self.assertIn(text, str(caught.exception))

    # --- --eclipse-iso ---------------------------------------------------------

    def test_links_your_eclipse_image(self):
        image = self.mine_file('Eclipse.iso', ECLIPSE)
        out = self.run_get(None, eclipse_iso=image)
        self.assertEqual(self.out.read_bytes(), ECLIPSE)
        self.assertTrue(os.path.samefile(image, self.out))  # linked, not copied
        self.assertIn('Linked it into mods/eclipse', out)
        self.assertIn('Installed mods/eclipse/Super Mario Eclipse v1.1.0.iso.', out)
        self.assertEqual(self.maps, 1)
        self.assertEqual(sorted(os.listdir(self.install)), [self.get.ECLIPSE['iso']])

    def test_copies_when_it_cannot_link(self):
        image = self.mine_file('Eclipse.iso', ECLIPSE)

        def no_links(*args):
            raise OSError('another drive')
        self.get.os.link, link = no_links, os.link
        try:
            out = self.run_get(None, eclipse_iso=image)
        finally:
            self.get.os.link = link
        self.assertEqual(self.out.read_bytes(), ECLIPSE)
        self.assertFalse(os.path.samefile(image, self.out))
        self.assertIn('Copying it into mods/eclipse', out)
        self.assertTrue(os.path.exists(image))

    def test_replaces_a_previous_install(self):
        self.install.mkdir()
        (self.install / 'changelog.md').write_text('old')
        self.out.write_bytes(b'something else')
        self.run_get(None, eclipse_iso=self.mine_file('Eclipse.iso', ECLIPSE))
        self.assertEqual(sorted(os.listdir(self.install)), [self.get.ECLIPSE['iso']])
        self.assertEqual(self.out.read_bytes(), ECLIPSE)

    def test_the_installed_image_stays(self):
        self.install.mkdir()
        self.out.write_bytes(ECLIPSE)
        (self.install / 'changelog.md').write_text('kept')
        out = self.run_get(None, eclipse_iso=str(self.out))
        self.assertIn('It is the installed image already.', out)
        self.assertEqual(self.out.read_bytes(), ECLIPSE)
        self.assertTrue((self.install / 'changelog.md').exists())

    def test_renames_one_put_in_mods_eclipse_by_hand(self):
        self.install.mkdir()
        by_hand = self.install / 'eclipse.iso'
        by_hand.write_bytes(ECLIPSE)
        (self.install / 'changelog.md').write_text('kept')
        out = self.run_get(None, eclipse_iso=str(by_hand))
        self.assertIn('Renamed it to mods/eclipse/Super Mario Eclipse v1.1.0.iso.', out)
        self.assertEqual(sorted(os.listdir(self.install)), [self.get.ECLIPSE['iso'], 'changelog.md'])
        self.assertEqual(self.out.read_bytes(), ECLIPSE)

    def test_rejects_another_image_and_keeps_the_install(self):
        self.install.mkdir()
        self.out.write_bytes(ECLIPSE)
        self.assert_fails('that is not the image of Super Mario Eclipse v1.1.0',
                          None, eclipse_iso=self.mine_file('v1.0.iso', b'an older Eclipse'))
        self.assertEqual(self.out.read_bytes(), ECLIPSE)
        self.assertEqual(self.maps, 0)

    def test_says_when_it_is_the_sunshine_disc(self):
        self.assert_fails('that is your Super Mario Sunshine disc',
                          None, eclipse_iso=self.mine_file('GMSE01.iso', SUNSHINE))
        self.assertFalse(self.install.exists())

    def test_says_when_the_sunshine_disc_is_eclipse(self):
        self.assert_fails('install it with --eclipse-iso instead', self.mine_file('Eclipse.iso', ECLIPSE))

    def test_missing_file(self):
        self.assert_fails('no such image', None, eclipse_iso=str(self.mine / 'nothing.iso'))

    # --- --archive ---------------------------------------------------------------

    def patch_with_fakes(self):
        """7-Zip and the patcher, standing in for the real ones."""
        self.get.seven_zip = lambda: '7z'
        unpacked = []

        def extract(archive, dest, members=()):
            unpacked.append(archive)
            os.makedirs(os.path.join(dest, 'patches'))
            for name in ('patches/v1.1.0.xdelta', 'changelog.md'):
                pathlib.Path(dest, name).write_bytes(b'from the archive')
        self.get.extract = extract

        def apply(source, patch, output, progress):
            self.assertEqual(pathlib.Path(source).read_bytes(), SUNSHINE)
            pathlib.Path(output).write_bytes(ECLIPSE)
        self.addCleanup(setattr, self.get.vcdiff, 'apply', self.get.vcdiff.apply)  # a module others share
        self.get.vcdiff.apply = apply
        return unpacked

    def test_uses_your_download_and_keeps_it(self):
        unpacked = self.patch_with_fakes()
        archive = self.mine_file('super_mario_eclipse_v110.7z', RELEASE)
        out = self.run_get(self.mine_file('GMSE01.iso', SUNSHINE), archive_arg=archive)
        self.assertIn('Checking your Eclipse download', out)
        self.assertEqual(unpacked, [archive])
        self.assertEqual(self.fetched, [])
        self.assertEqual(self.out.read_bytes(), ECLIPSE)
        self.assertEqual(pathlib.Path(archive).read_bytes(), RELEASE)  # yours, never deleted
        self.assertEqual(self.maps, 1)

    def test_rejects_another_archive_before_touching_the_install(self):
        self.patch_with_fakes()
        self.install.mkdir()
        self.out.write_bytes(ECLIPSE)
        self.assert_fails('that is not the release of Super Mario Eclipse v1.1.0',
                          self.mine_file('GMSE01.iso', SUNSHINE),
                          archive_arg=self.mine_file('other.7z', b'another release'))
        self.assertEqual(self.out.read_bytes(), ECLIPSE)

    def test_an_archive_inside_mods_eclipse_must_move_first(self):
        self.patch_with_fakes()
        self.install.mkdir()
        archive = self.install / 'super_mario_eclipse_v110.7z'
        archive.write_bytes(RELEASE)
        self.assert_fails('move', self.mine_file('GMSE01.iso', SUNSHINE), archive_arg=str(archive))
        self.assertTrue(archive.exists())

    def test_command_line(self):
        calls = []
        self.get.get_eclipse = lambda *args: calls.append(args)
        for argv in (['eclipse', '--eclipse-iso', 'E.iso'], ['eclipse', '--iso', 'S.iso', '--archive', 'A.7z']):
            self.get.sys.argv = ['get.py'] + argv
            self.get.main()
        self.assertEqual(calls, [(False, None, 'E.iso', None), (False, 'S.iso', None, 'A.7z')])


if __name__ == '__main__':
    unittest.main()
