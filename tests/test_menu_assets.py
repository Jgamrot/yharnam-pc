from paths import ROOT
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, str(ROOT / 'scripts'))
import menu_assets as m  # noqa: E402
from fromsoft import Bnd4, dcx_decompress  # noqa: E402

# BB_TEST_GAME_DIR=<CUSA03173>: also edit the real 1.09 movie and message bundles.
GAME = Path(os.environ['BB_TEST_GAME_DIR']) / 'dvdroot_ps4' if os.environ.get('BB_TEST_GAME_DIR') else None


def placed_names(movie, codes=(m.PLACE2, m.PLACE3)):
    out = []
    for code, _head, s, _e in m.tags(movie, m.body_start(movie), len(movie)):
        if code in codes:
            name = m.placement(movie, code, s)[3]
            if name:
                out.append(name)
    return out


class FormatTests(unittest.TestCase):
    def test_fmg_round_trip(self):
        ids = {5: 'five'.encode('utf-16-le'), 6: None, 7: 'seven'.encode('utf-16-le'),
               20: 'five'.encode('utf-16-le')}
        self.assertEqual(m.fmg_read(m.fmg_write(ids)), ids)

    def test_matrix_round_trip(self):
        matrix = m.Matrix()
        matrix.has_scale, matrix.sn, matrix.sa, matrix.sc = True, 17, 65536, 70000
        matrix.tx, matrix.ty = -1234, 5678
        again, _end = m.read_matrix(m.write_matrix(matrix), 0)
        self.assertEqual((again.sa, again.sc, again.tx, again.ty), (65536, 70000, -1234, 5678))

    def test_messages_are_menu_or_event_text(self):
        rows = m.read_messages()
        for mid in (110006, 110013, 116020, 124020, 125000, 116004, 124001, 124003):
            self.assertTrue(rows[mid][0], mid)
        events = [mid for mid in rows if m.message_kind(mid) == 'event']
        # Rebirth; the 19 boss rematches (14000337 Cleric Beast to 14000355 Orphan of Kos)
        self.assertEqual(events, [14000332, 14000333, 14000334, 14000335, 14000336, *range(14000337, 14000356)])
        self.assertEqual([mid for mid in rows if m.message_kind(mid) == 'dialog'], [920001, 920002, 920003])
        self.assertEqual(rows[122000][0], 'Quit Game')

    def test_runtime_and_movie_agree_on_sections_rows_and_strings(self):
        import re
        runtime = (ROOT / 'src/pc_menu.c').read_text(encoding='utf-8')
        rows = m.read_messages()
        arrays = dict(re.findall(r'static Row (g_\w+)\[\] = \{(.*?)\n\};', runtime, re.S))
        sections = re.findall(r'\{"(\w+)", (\d+), (g_\w+), COUNT\(g_\w+\)\}', runtime)
        # Achievements is built by its own handler: six text rows (ACH_ROWS) from 128000.
        self.assertIn('"PCAchieve"', runtime)
        self.assertIn('ACH_ROWS = 6', runtime)
        self.assertIn(('PCAchieve', 128020, 6), m.SECTION_ROWS)
        for row_id in range(128000, 128006):
            self.assertIn(row_id, rows)
        self.assertEqual(sorted([n for n, *_ in sections] + ['PCAchieve']), sorted(n for n, *_ in m.SECTION_ROWS))
        native = re.search(r'NATIVE_SECTION_INDEX\[\] = \{([^}]+)\}', runtime)
        self.assertIsNotNone(native)
        indices = [int(index) for index in native.group(1).split(',')]
        self.assertEqual([sections[index][0] for index in indices],
                         [name for name, *_ in m.NATIVE_SECTIONS if name != 'PCAchieve'])
        self.assertIn(('PCAchieve', 128020, 6), m.NATIVE_SECTIONS)
        self.assertNotIn(('PCEnhance', 124020, 3), m.NATIVE_SECTIONS)
        self.assertIn('add_command(builder, ROW_ACHIEVEMENTS, (const void *)ach_row, 1)', runtime)
        self.assertEqual(len(m.NATIVE_SECTIONS), 6)  # in-game GFX capacity checked in play
        counts = {name: rows_n for name, _title, rows_n in m.SECTION_ROWS}
        for name, system, array in sections:
            ids = [int(i) for i in re.findall(r'_ROW\("\w+", (\d+)', arrays[array])]
            self.assertEqual(len(ids), counts[name], name)
            self.assertLessEqual(len(ids), 6, name)
            self.assertIn(int(system), rows, name)
            for row_id in ids:
                self.assertTrue(rows[row_id][1], f'{row_id} has no line help')
            # Pick lists below every toggle (their drop-down covers what is under them).
            kinds = re.findall(r'(TOGGLE|PICK)_ROW', arrays[array])
            self.assertEqual(kinds, sorted(kinds, key=lambda k: k == 'PICK'), name)
        options = ''.join(re.findall(r'static const Option \w+\[\] = \{(.*?)\};', runtime))
        self.assertTrue(options)
        for choice in re.findall(r'\{-?\d+, (\d+)\}', options):
            self.assertIn(int(choice), rows)
        for message in (110017, 920002, 122000, 920001):
            self.assertIn(message, rows)


@unittest.skipUnless(GAME, 'BB_TEST_GAME_DIR not set')
class RealGameTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.files = m.make(GAME, log=lambda line: None)

    def test_movie_gains_both_sections_and_the_scroll_bar(self):
        source = (GAME / m.MOVIE).read_bytes()
        movie = self.files[m.MOVIE]
        self.assertEqual(int.from_bytes(movie[4:8], 'little'), len(movie))
        names = placed_names(movie)
        for name, title, *_ in m.SECTIONS:
            self.assertEqual(names.count(name), 1)
            self.assertIn(f'StaticText_{title}'.encode(), movie)
        sprites = [s for code, _h, s, _e in m.tags(movie, m.body_start(movie), len(movie)) if code == m.DEFINE_SPRITE]
        before = [s for code, _h, s, _e in m.tags(source, m.body_start(source), len(source)) if code == m.DEFINE_SPRITE]
        self.assertEqual(len(sprites), len(before) + 2 * len(m.SECTIONS))  # each: the section and its title
        self.assertEqual(len(m.symbol_classes(movie)), len(m.symbol_classes(source)) + len(m.SECTIONS))
        # The System list ("Top") has its scroll bar now.
        top = next(m.placement(movie, code, s) for code, _h, s, _e in m.tags(movie, m.body_start(movie), len(movie))
                   if code in (m.PLACE2, m.PLACE3) and m.placement(movie, code, s)[3] == 'Top')
        head, s, e = m.find_sprite(movie, top[1])
        inside = [m.placement(movie, code, s2)[3] for code, _h, s2, _e in m.tags(movie, s + 4, e)
                  if code in (m.PLACE2, m.PLACE3)]
        self.assertIn('ScrollBarV', inside)

    def test_title_lists_gain_a_line_for_quit_game(self):
        for relative in m.TITLE_MOVIES:
            source, movie = (GAME / relative).read_bytes(), self.files[relative]
            self.assertEqual(int.from_bytes(movie[4:8], 'little'), len(movie))
            self.assertNotIn('Item_5_0', placed_names(source, (m.PLACE2,)) + self.list_rows(source))
            self.assertIn('Item_5_0', self.list_rows(movie))

    @staticmethod
    def list_rows(movie):
        rows = []
        for code, _h, s, e in m.tags(movie, m.body_start(movie), len(movie)):
            if code == m.DEFINE_SPRITE:
                rows += [m.placement(movie, c, s2)[3] for c, _h2, s2, _e2 in m.tags(movie, s + 4, e)
                         if c in (m.PLACE2, m.PLACE3)]
        return [r for r in rows if r]

    def test_every_language_gains_the_strings_and_keeps_its_own(self):
        rows = m.read_messages()
        languages = [k for k in self.files if k.name == m.MESSAGE_FILE]
        self.assertGreater(len(languages), 1)
        for relative in languages:
            before = Bnd4(dcx_decompress((GAME / relative).read_bytes()))
            after = Bnd4(dcx_decompress(self.files[relative]))
            self.assertEqual(after.names, before.names)
            for group in m.GROUP_KIND:
                old = m.fmg_read(before.files[before.ids.index(group)])
                new = m.fmg_read(after.files[after.ids.index(group)])
                self.assertEqual({k: v for k, v in new.items() if k in old}, old)  # nothing changed
            menu = m.fmg_read(after.files[after.ids.index(m.MENU_TEXT)])
            self.assertEqual(menu[110006].decode('utf-16-le'), rows[110006][0])
            events = m.fmg_read(after.files[after.ids.index(m.EVENT_TEXT)])
            self.assertEqual(events[14000335].decode('utf-16-le'), 'Accept Rebirth')
            for group in (m.DIALOG_TEXT, m.SCREEN_DIALOG_TEXT):
                dialog = m.fmg_read(after.files[after.ids.index(group)])
                self.assertEqual(dialog[920001].decode('utf-16-le'), rows[920001][0])

    def test_a_damaged_message_bundle_turns_the_menu_off(self):
        # Issue #8: a cut-short menu.msgbnd.dcx stopped the game instead.
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / m.MOVIE).parent.mkdir(parents=True)
            shutil.copyfile(GAME / m.MOVIE, root / m.MOVIE)
            relative = Path('msg') / 'engus' / m.MESSAGE_FILE
            (root / relative).parent.mkdir(parents=True)
            (root / relative).write_bytes((GAME / relative).read_bytes()[:-100])
            lines = []
            self.assertEqual(m.make(root, log=lines.append), {})
            self.assertEqual(len(lines), 1)
            self.assertIn('PC menu: off - msg/engus/menu.msgbnd.dcx: DCX file is truncated', lines[0])


if __name__ == '__main__':
    unittest.main()
