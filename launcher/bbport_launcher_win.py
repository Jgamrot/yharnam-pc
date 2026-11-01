#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bloodborne launcher for Windows (Qt Quick, with a classic Tk fallback).

Every setting of the port in one window: the game folder and saves, bbport.ini (upscaler,
preset, output, effects), start-up options passed to run.py as environment variables
(frame rate, presentation, HDR, ...), mods, third-party patches and the FSR 4 assets.
Launcher options live in %APPDATA%/bbport-launcher/settings.json.

Frozen with PyInstaller (packaging/windows/package.sh) the same Bloodborne.exe also runs the
game without the window (`--play`), run.py (`--run`) and the preparation scripts (`--script`),
so a packaged port needs no Python installation. A plain start launches the game directly once
setup is done; `--launcher` (or Shift/Ctrl held) opens the launcher and `--setup` the wizard.
"""
import ctypes
import http.client
import json
import os
from pathlib import Path
import queue
import re
import runpy
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import webbrowser
import zipfile

FROZEN = getattr(sys, 'frozen', False)
PORT_DIR = Path(sys.executable).resolve().parent if FROZEN else Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PORT_DIR / 'scripts'))
sys.path.append(str(PORT_DIR))  # run.py's START_SCRIPTS; frozen, the copy built into the launcher wins
DATA_DIR = Path(os.environ.get('BB_DATA_DIR', PORT_DIR))
# Co-op test build: its own settings, and no updates (a public release would replace it).
COOP_TEST = False
CONFIG_DIR = Path(os.environ.get('APPDATA', Path.home())) / ('bbport-launcher-coop' if COOP_TEST else 'bbport-launcher')
CONFIG_FILE = CONFIG_DIR / 'settings.json'
PATCH_VERSION = '01.09'
MAX_LOG_LINES = 6000
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
# This fork's build; the release tag includes this version.
VERSION = '2.3.2'
CURRENT_RELEASE_TAG = 'v2.3.2'
HERO_ART = PORT_DIR / 'launcher' / 'art' / 'hero.png'
STRIP_ART = PORT_DIR / 'launcher' / 'art' / 'strip.png'
# Co-op: the community server that answers the game's own server requests.
COMMUNITY_SERVER = 'https://thehuntersdream.com'
# Its co-op service (bbhost's), as run.py's COMMUNITY_API_PORT sets the game's BB_THD_SERVER.
COMMUNITY_API_PORT = 18671
RELEASES_API = 'https://api.github.com/repos/Jgamrot/yharnam-pc/releases/latest'
RELEASES_PAGE = 'https://github.com/Jgamrot/yharnam-pc/releases/latest'
RELEASES_LIST_API = 'https://api.github.com/repos/Jgamrot/yharnam-pc/releases?per_page=100'
UPDATE_DIR = Path(tempfile.gettempdir()) / 'bbport-update'
# The last answer from GitHub, so a direct launch (no launcher window) can check quietly and the
# launcher shows the result the next time it opens. Off with check_updates like every check.
UPDATE_CACHE = CONFIG_DIR / 'update-check.json'
QUIET_CHECK_DELAY = 20  # seconds after a direct launch starts the game
QUIET_CHECK_INTERVAL = 6 * 3600  # at most this often from direct launches
# Never copied over an installation by an update (the package does not hold them either).
USER_FILES = ('user', 'out', 'mods', 'bbport.ini', 'mods.json', 'patches.json', 'last_run.log')
# The running launcher: in use, and already this version. A repair leaves them alone.
LAUNCHER_FILES = ('Bloodborne.exe', '_internal')


# ---------------------------------------------------------------------------------------------
# Command line roles of the frozen executable.

def attach_stdio():
    """A windowed executable starts without sys.stdout; inherited pipes or a console still exist."""
    import msvcrt
    for name, std in (('stdout', -11), ('stderr', -12)):
        if getattr(sys, name) is not None:
            continue
        stream = None
        handle = ctypes.windll.kernel32.GetStdHandle(std)
        if handle and handle != ctypes.c_void_p(-1).value:
            try:
                stream = open(msvcrt.open_osfhandle(handle, os.O_WRONLY), 'w', encoding='utf-8',
                              errors='replace', buffering=1)
            except OSError:
                stream = None
        setattr(sys, name, stream or open(os.devnull, 'w'))


def run_role(argv):
    """--run [args]: run.py; --script <file> [args]: a preparation script. Returns an exit code."""
    attach_stdio()
    for stream in (sys.stdout, sys.stderr):  # keep messages in order with the game's output
        try:
            stream.reconfigure(line_buffering=True)
        except (AttributeError, ValueError):
            pass
    # The launcher may close (close on play) while run.py and its scripts still print.
    sys.path.insert(0, str(PORT_DIR / 'scripts'))
    import quiet_output
    quiet_output.guard()
    if argv[0] == '--script':
        path, sys.argv = argv[1], argv[1:]
    else:
        path = str(PORT_DIR / 'run.py')
        sys.argv = [path, *argv[1:]]
    try:
        runpy.run_path(path, run_name='__main__')
    except SystemExit as stop:
        return exit_status(stop.code if isinstance(stop.code, int) else (0 if stop.code is None else 1))
    return 0


# Windows status codes a process can end with before it prints anything (run.py passes them on).
EXIT_HINTS = {
    0xC0000005: 'it crashed (access violation)',
    0xC000007B: 'a DLL in bin is damaged or the wrong kind: extract the release into a new, empty folder',
    0xC0000135: 'a DLL is missing from bin (antivirus?): extract the release into a new, empty folder',
    0xC0000409: 'it stopped on a fatal error',
}


def exit_status(code):
    """`code` as sys.exit and a Qt int take it: 32 bits, signed (0xC000007B is -1073741701)."""
    code &= 0xFFFFFFFF
    return code - (1 << 32) if code & 0x80000000 else code


def exit_code_text(code):
    """An exit code as the launcher shows it: Windows status codes in hex, with what they mean."""
    code &= 0xFFFFFFFF
    if not code & 0x80000000:
        return str(code)
    hint = EXIT_HINTS.get(code)
    return f'0x{code:08X}' + (f', {hint}' if hint else '')


def https_context():
    """Windows' trusted roots plus Mozilla's (certifi, shipped with the launcher). Python reads
    only the roots already in the Windows store, which may lack newer ones such as Let's
    Encrypt's (The Hunter's Dream); Windows itself fetches those on demand."""
    import ssl
    context = ssl.create_default_context()
    try:
        import certifi
        context.load_verify_locations(certifi.where())
    except (ImportError, OSError, ssl.SSLError):
        pass
    return context


urllib.request.install_opener(urllib.request.build_opener(urllib.request.HTTPSHandler(context=https_context())))


def connection_error_text(error):
    """The error, with a hint when a certificate was refused."""
    text = str(error)
    if 'CERTIFICATE_VERIFY_FAILED' in text:
        text += (". Check that the PC's date and time are right; HTTPS scanning in an "
                 'antivirus or a proxy can also cause this')
    return text


def run_command():
    """The command that starts run.py: this executable when frozen, else Python."""
    return [sys.executable, '--run'] if FROZEN else [sys.executable, str(PORT_DIR / 'run.py')]


# ---------------------------------------------------------------------------------------------
# Languages: every text is written in English with the Russian next to it; the other
# languages are in bbport_lang.py, keyed by the English text.

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bbport_lang  # noqa: E402

LANG = 'en'


WARN = '  ⚠'  # marks a risky choice; the text before it is translated as usual


def _(en, ru=None):
    if en.endswith(WARN):
        return _(en[:-len(WARN)], ru and ru.removesuffix(WARN)) + WARN
    if LANG == 'ru':
        return ru or bbport_lang.table('ru').get(en) or en
    return bbport_lang.table(LANG).get(en) or en


def windows_language():
    try:
        primary = ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3ff
    except (AttributeError, OSError):
        return 'en'
    return bbport_lang.WINDOWS_LANGUAGES.get(primary, 'en')


# ---------------------------------------------------------------------------------------------
# Settings. bbport.ini keys (the game reads them, the in-game menu edits them) and the
# launcher's own settings.json (passed to run.py as environment variables).

EFFECTS = [
    ('effect_chromatic_aberration', ('Chromatic aberration', 'Хроматическая аберрация'), True),
    ('effect_dof', ('Depth of field', 'Глубина резкости (DoF)'), True),
    ('effect_motion_blur', ('Motion blur', 'Размытие в движении'), True),
    ('effect_ssao', ('Ambient occlusion (SSAO)', 'Затенение SSAO'), True),
    ('effect_game_aa', ("The game's own anti-aliasing", 'Собственное сглаживание игры'), True),
    ('effect_dynamic_shadows', ('Shadows of dynamic lights', 'Тени от динамических источников'), True),
    ('effect_ssr', ('Screen-space reflections (not in the original)', 'Отражения SSR (не было в игре)'), False),
]
EXTRAS = [
    ('skip_intro', ('Skip the intro logos and movie', 'Пропуск заставок при запуске'), False),
    ('debug_camera', ('Free camera (hold Cross + L3; keyboard Space + Z)', 'Свободная камера (Cross + L3 / Space + Z)'), False),
    ('debug_menu', ('Game debug menu (left touchpad / Tab; needs the debug fonts)',
                    'Debug menu (левый touchpad / Tab; нужны шрифты)'), False),
]
# Cheats and gameplay tweaks: game patches for 1.09 (patches.py EFFECTS), off by default.
CHEATS = [
    ('cheat_no_death', ('Never die (health stops at 1 HP)', 'Бессмертие (здоровье не ниже 1 HP)'), False),
    ('cheat_stealth', ('Enemies do not see you (unless attacked)', 'Враги не видят вас (пока не атакованы)'), False),
    ('cheat_silent', ('Enemies do not hear you', 'Враги не слышат вас'), False),
    ('cheat_rally_no_decay', ('Rally never fades', 'Rally не угасает'), False),
    ('cheat_enemy_control', ('Control the targeted enemy (R3; L3 to go back; not with the free camera)',
                             'Управление выбранным врагом (R3; L3 — назад; не вместе со свободной камерой)'), False),
    ('cheat_no_stamina', ('No stamina use', 'Без расхода выносливости'), False),
    ('cheat_stamina_out_of_combat', ('No stamina use out of combat (8 s after your last attack or hit)',
                                     'Без расхода выносливости вне боя (8 с после атаки или удара)'), False),
]
TWEAKS = [
    ('tweak_no_rally', ('No Rally (hits do not give health back)', 'Без Rally (удары не возвращают здоровье)'), False),
    ('tweak_camera_distance', ('Camera further from the character', 'Камера дальше от персонажа'), False),
    ('tweak_no_camera_rotation', ('No camera auto-rotation while moving', 'Без автоповорота камеры при движении'), False),
    ('tweak_easy_run', ('Run with less stick tilt (about 70%)', 'Бег при меньшем наклоне стика (около 70%)'), False),
    ('tweak_circle_confirm', ('Japanese/Asian buttons: Circle confirms and interacts, Cross dodges',
                              'Японская раскладка: Круг — подтверждение и действие, Крест — уклонение'), False),
    ('tweak_ragdoll', ('Dark Souls-style ragdoll physics (corpses fly further)',
                       'Физика тел как в Dark Souls (тела отлетают дальше)'), False),
]
# PC enhancements (scripts/enhancements.py, after bbhost): game files made at start; off is the
# game as it shipped. BB_PC_MIRROR / BB_PC_TWO_INVADERS / BB_PC_REBIRTH / BB_PC_LAMPS /
# BB_PC_REMATCHES override them.
# Hunter's Atlas (MikeTat1988/HuntersAtlas, scripts/atlas.py): the full-screen map, its maps made
# once from the player's game (Mods & Patches > Prepare maps).
ATLAS = [
    ('atlas_enabled', ("Hunter's Atlas: full-screen map (Map in the game's quick menu)",
                       "Hunter's Atlas: полноэкранная карта (пункт «Карта» в быстром меню)"), False),
    ('atlas_travel', ('Fast travel to discovered lamps from the map', 'Быстрое перемещение к открытым фонарям'), False),
    ('atlas_fog', ("Reveal an area's map only after its boss", 'Открывать карту области только после босса'), False),
    ('atlas_items', ('Pickup markers (vials, Insight, equipment, Coldblood)',
                     'Метки предметов (флаконы, озарение, снаряжение, отголоски)'), False),
]
ATLAS_NPC_MODES = [('all', ('All NPCs', 'Все NPC')), ('new_dialogue', ('Only NPCs with new dialogue', 'Только с новыми репликами')),
                   ('hidden', ('Hidden', 'Скрыты'))]
ATLAS_DIR = DATA_DIR / 'atlas'
ENHANCEMENTS = [
    ('pc_mirror_appearance', ("Hunter's Dream mirror: change appearance",
                              'Зеркало в Сне охотника: смена внешности'), False),
    ('pc_two_invaders', ('Two invaders in the late areas (every player needs it)',
                         'Два захватчика в поздних локациях (нужно всем игрокам)'), False),
    ('pc_rebirth', ('Rebirth in the Nightmare at the Altar of Despair',
                    'Перерождение у Алтаря отчаяния'), False),
    ('pc_enhanced_lamps', ('Lamps: Level Up and Storage at every lit lamp',
                           'Фонари: повышение уровня и хранилище у каждого фонаря'), False),
    ('pc_boss_rematches', ('Boss rematches at their lamps (experimental; back up your saves)',
                           'Повторные бои с боссами у их фонарей (эксперимент; сохраните копию сохранений)'), False),
]
INI_FLAGS = {'sharpen', 'object_motion', 'show_fps', 'mouse_camera', 'mouse_invert_y', 'mouse_menu', 'mouse_hide_cursor', 'frame_gen_fit_display',
             'overlay_pad_shortcut',
             *(k for k, _t, _o in EFFECTS + EXTRAS + CHEATS + TWEAKS + ENHANCEMENTS + ATLAS)}
INI_DEFAULTS = {'upscaler': 'dlss', 'preset': '2', 'sharpen': '1', 'sharpness': '0.50',
                'object_motion': '1', 'show_fps': '1', 'output_res': '2560x1440', 'hud_position': '100', 'model_lod': '0',
                'live_resolution': 'auto', 'mouse_camera': '1', 'mouse_sensitivity': '10', 'mouse_menu': '1',
                'mouse_invert_y': '0', 'mouse_hide_cursor': '0', 'overlay_pad_shortcut': '1', 'pad_back': 'gestures', 'move_smoothing': '60', 'frame_gen': 'off', 'frame_gen_multiplier': '2', 'frame_gen_fit_display': '0',
                'atlas_npc_mode': 'all',
                **{key: '1' if on else '0' for key, _t, on in EFFECTS + EXTRAS + CHEATS + TWEAKS + ENHANCEMENTS + ATLAS}}
APP_DEFAULTS = {'ui_language': '', 'game_dir': str(PORT_DIR.parent / 'CUSA03173'), 'user_dir': '', 'dlc_dir': '',
                'mods_dir': '', 'mods_enabled': True, 'patches_dir': '', 'language': '1',
                'voice_language': 'auto', 'audio_channels': '',
                'player_name': '', 'fullscreen': False, 'fullscreen_mode': 'borderless', 'display': '',
                'hdr': False, 'present_mode': 'Mailbox',
                'fps_mode': '120', 'frame_cap': '', 'draw_pipe': '', 'readbacks': '',
                'frames_ahead': '', 'vram_budget_mb': '', 'frame_stats': False, 'gpu_profile': False,
                'vk_validation': False, 'extra_env': '', 'close_on_play': False,
                'check_updates': True, 'online': False, 'online_server': 'srv.shadps4.net:31313',
                'online_webapi': '', 'online_npid': '', 'online_password': '', 'online_upnp': True,
                'coop_name': '', 'coop_password': '', 'coop_invite': '',
                'online_community': COMMUNITY_SERVER,
                # The co-op pool: 'shadnet' (the server above) or 'huntersdream' (shown only with
                # BB_HUNTERS_DREAM=0 hides it), and The Hunter's Dream account from its Discord sign-in.
                'online_pool': 'shadnet', 'thd_online_id': '', 'thd_token': '', 'play_mode': '',
                # First-run setup (bbport_startup): setupVersion is the wizard version last
                # finished or skipped; launchDirectly starts the game without the launcher.
                'setupCompleted': False, 'setupSkipped': False, 'setupVersion': 1,
                'launchDirectly': True}



def app_settings(saved):
    """The launcher settings: APP_DEFAULTS under the saved ones. Settings saved before 120 FPS was
    the default and without a frame rate of their own ran unlocked: they keep that."""
    settings = {**APP_DEFAULTS, **saved}
    if saved and 'fps_mode' not in saved:
        settings['fps_mode'] = 'uncap'
    return settings


UPSCALERS = [('dlss', ('DLSS (NVIDIA GeForce RTX)',)),
             ('fsr4', ('FSR 4 (best quality)', 'FSR 4 (лучшее качество)')),
             ('fsr411', ('FSR 4.1.1 (needs fsr4_411 assets)', 'FSR 4.1.1 (нужны ассеты fsr4_411)')),
             ('fsr3', ('FSR 3.1 (every GPU)', 'FSR 3.1 (любая видеокарта)')),
             ('taa', ('TAA (native resolution anti-aliasing)', 'TAA (нативное сглаживание)')),
             ('off', ('Off', 'Выключен'))]
# Output / render size per upscaling preset (scripts/patches.py PRESET_SCALES).
PRESET_SCALE_VALUES = [1.0, 1.5, 1.7, 2.0, 3.0]
# Frames generated between two of the game's (vk_frame_generation.cpp).
FRAME_GENERATION = [('off', ('Off', 'Выключена')),
                    ('fsr3', ('AMD FSR 3.1 Frame Generation (any GPU)', 'AMD FSR 3.1 (любая видеокарта)')),
                    ('dlss', ('NVIDIA DLSS Frame Generation (GeForce RTX 40/50)',
                              'NVIDIA DLSS (GeForce RTX 40/50)'))]
FRAME_GEN_MULTIPLIERS = [('2', ('2× (one generated frame)', '2× (один кадр)')),
                         ('3', ('3× (DLSS on GeForce RTX 50)', '3× (DLSS на GeForce RTX 50)')),
                         ('4', ('4× (DLSS on GeForce RTX 50)', '4× (DLSS на GeForce RTX 50)'))]
PRESETS = [('0', ('Native AA (×1.0)',)), ('1', ('Quality (×1.5)',)), ('2', ('Balanced (×1.7)',)),
           ('3', ('Performance (×2)',)), ('4', ('Ultra Performance (×3)',))]
OUTPUTS = [('1280x720', ('1280 × 720 (Steam Deck)',)), ('1366x768', ('1366 × 768',)),
           ('1920x1080', ('1920 × 1080',)),
           ('2560x1440', ('2560 × 1440',)), ('3840x2160', ('3840 × 2160 (4K)',)),
           ('1280x800', ('1280 x 800 (16:10)',)), ('1920x1200', ('1920 × 1200 (16:10)',)),
           ('2560x1080', ('2560 x 1080 (Ultrawide)',)),
           ('3440x1440', ('3440 x 1440 (Ultrawide)',)), ('5120x1440', ('5120 x 1440 (32:9)',)),
           ('5120x2160', ('5120 x 2160 (Ultrawide)',)), ('3840x1600', ('3840 x 1600 (Ultrawide)',)),
           ('3840x1080', ('3840 x 1080 (32:9)',)), ('1080x1920', ('1080 x 1920 (Portrait)',)),
           # 4:3 for CRT monitors (issue #34): narrower than 16:9 the camera keeps its vertical
           # view and the HUD stays 16:9, centred (docs/aspect_ratio.md). In game: the custom slot.
           ('1024x768', ('1024 x 768 (4:3)',)), ('1280x960', ('1280 x 960 (4:3)',)),
           ('1440x1080', ('1440 x 1080 (4:3)',)), ('1600x1200', ('1600 x 1200 (4:3)',)),
           ('1920x1440', ('1920 x 1440 (4:3)',))]
# Ultrawide outputs: where the HUD's corner groups go (scripts/hud_layout.py at start).
HUD_POSITIONS = [('100', ('Screen edges', 'У краёв экрана')), ('75', ('75%',)), ('50', ('Halfway', 'Посередине')),
                 ('25', ('25%',)), ('0', ('Centred (16:9)', 'По центру (16:9)'))]

LIVE = [('auto', ('Auto (by graphics card)', 'Авто (по видеокарте)')), ('0', ('Off (faster)', 'Выключена (быстрее)')),
        ('1', ('On (change without restarting)', 'Включена (без перезапуска)'))]
LODS = [('0', ('As in the game', 'Как в игре')), ('-2', ('Highest (−2)', 'Максимальная (−2)')),
        ('-1', ('Higher (−1)', 'Высокая (−1)')),
        ('1', ('Lower (1)', 'Ниже (1)')), ('2', ('Lowest (2)', 'Минимальная (2)'))]
# 120 and 144: 90 FPS++ with its values for that rate (scripts/patches.py SCALED_FPS). Unlocked froze
# at the end of Vicar Amelia's intro; settings saved before 120 was the default keep it
# (app_settings).
FPS_MODES = [('120', ('120 FPS (recommended)', '120 FPS (рекомендуется)')), ('144', ('144 FPS',)),
             ('90', ('90 FPS',)), ('60', ('60 FPS',)),
             ('uncap', ("Unlocked (frame-time patch; can freeze at the end of Vicar Amelia's intro)",
                        'Без ограничения (патч; может зависнуть в конце заставки Викарии Амелии)')),
             ('30', ('30 FPS (as on PS4)', '30 FPS (как на PS4)'))]
# Fullscreen (BB_FULLSCREEN_MODE; window.cpp, after RafaelHGOliveira's Supermedo PR #68): a
# borderless window over the desktop, or exclusive (the display switched to its desktop mode).
FULLSCREEN_MODES = [('borderless', ('Borderless (default)', 'Без рамки (по умолчанию)')),
                    ('exclusive', ('Exclusive', 'Эксклюзивный'))]
# The monitor the game opens on (BB_DISPLAY; window.cpp, after parsifal295's Supermedo PR #66):
# numbered in the order the game's log lists them ("Display 1: ...").
DISPLAYS = [('', ('Primary monitor', 'Основной монитор'))] + \
    [(str(n), (f'Monitor {n}', f'Монитор {n}')) for n in range(1, 5)]
PRESENT_MODES = [('Mailbox', ('Mailbox (low latency, no tearing)', 'Mailbox (без разрывов)')),
                 ('Fifo', ('FIFO (VSync)',)), ('FifoRelaxed', ('FIFO Relaxed',)),
                 ('Immediate', ('Immediate (tearing)', 'Immediate (с разрывами)'))]
LANGUAGES = [('1', ('English', 'Английский')), ('8', ('Russian', 'Русский')), ('0', ('Japanese', 'Японский')),
             ('2', ('French', 'Французский')), ('3', ('Spanish', 'Испанский')), ('4', ('German', 'Немецкий')),
             ('5', ('Italian', 'Итальянский')),
             # The other languages the game ships (dvdroot_ps4/msg), by PS4 system language id.
             ('17', ('Portuguese (Brazil)', 'Португальский (Бразилия)')),
             ('7', ('Portuguese (Portugal)', 'Португальский (Португалия)')),
             ('20', ('Spanish (Latin America)', 'Испанский (Латинская Америка)')),
             ('18', ('English (UK)', 'Английский (Великобритания)')), ('6', ('Dutch', 'Нидерландский')),
             ('16', ('Polish', 'Польский')), ('19', ('Turkish', 'Турецкий')), ('12', ('Finnish', 'Финский')),
             ('13', ('Swedish', 'Шведский')), ('14', ('Danish', 'Датский')), ('15', ('Norwegian', 'Норвежский')),
             ('9', ('Korean', 'Корейский')), ('10', ('Chinese (Traditional)', 'Китайский (традиционный)')),
             ('11', ('Chinese (Simplified)', 'Китайский (упрощённый)'))]
# Voice tracks apart from the text language (BB_VOICE_LANGUAGE, scripts/language_files.py; after
# Berleis, Supermedo/bloodborne_pc PR #20): the game's sound/*_<code>.fsb files.
VOICE_LANGUAGES = [('auto', ('As in the game', 'Как в игре')), ('eng', ('English',)),
                   ('ptb', ('Português (Brasil)',)), ('ded', ('Deutsch',)),
                   ('esa', ('Español (América)',)), ('ese', ('Español (España)',)),
                   ('frf', ('Français',)), ('iti', ('Italiano',)), ('jaj', ('日本語',))]
# Speakers (BB_AUDIO_CHANNELS): the channels the game's 7.1 mix is folded to. A TV or headset can
# take 7.1 from Windows and play only the front pair: no dialogue (GitHub issue #195).
AUDIO_CHANNELS = [('', ('As in Windows', 'Как в Windows')),
                  ('2', ('Stereo (headphones, TV speakers)', 'Стерео (наушники, динамики ТВ)')),
                  ('6', ('5.1',)), ('8', ('7.1',))]
DRAW_PIPE = [('', ('Auto (8+ threads)', 'Авто (8+ потоков)')), ('1', ('On', 'Включён')),
             ('0', ('Off (more stable)', 'Выключен (стабильнее)'))]
READBACKS = [('', ('Relaxed (default)', 'Relaxed (по умолчанию)')), ('0', ('Off', 'Выключены')),
             ('2', ('Precise',))]
# Frame cap of the unlocked mode (BB_FPS_LIMIT). '' leaves the port's own: the display refresh,
# at most 120, because the game's movement timing breaks above about 120 FPS.
FRAME_CAPS = [('', ('Auto: display refresh, max 120 (recommended)', 'Авто: частота монитора, макс. 120 (рекомендуется)')),
              ('60', ('60',)), ('90', ('90',)), ('120', ('120',)), ('144', ('144  ⚠',)), ('165', ('165  ⚠',)),
              ('240', ('240  ⚠',)), ('0', ('No limit  ⚠', 'Без ограничения  ⚠'))]
# Mouse camera sensitivity in tenths (bbport.ini mouse_sensitivity; 10 = 900 counts per radian).
MOUSE_SPEEDS = [(str(v), (f'{v / 10:.1f}x',)) for v in (5, 7, 10, 13, 16, 20, 30)]
# Keyboard movement smoothing (bbport.ini move_smoothing, issue #4): how far the stick swings
# towards a new WASD direction per 1/60 s, in percent of a full push; 100 is off.
MOVE_SMOOTHING = [('100', ('Off', 'Выкл.')), ('80', ('Light', 'Слабое')),
                  ('60', ('Medium (default)', 'Среднее (по умолчанию)')), ('35', ('Strong', 'Сильное'))]
# Xbox View / Back alone (bbport.ini pad_back, #228): the touchpad half it clicks; View + d-pad
# left or right picks either one.
PAD_BACK = [('gestures', ('Gestures (default)', 'Жесты (по умолчанию)')),
            ('effects', ('Personal Effects', 'Личные вещи'))]
FRAMES_AHEAD = [('', ('1 (default)', '1 (по умолчанию)')), ('2', ('2',)), ('0', ('Unbounded', 'Без ограничения'))]
VRAM_BUDGETS = [('', ('Automatic (recommended)', 'Автоматически (рекомендуется)')),
                ('2560', ('2.5 GB',)), ('3072', ('3 GB',)), ('4096', ('4 GB',)),
                ('5120', ('5 GB',)), ('6144', ('6 GB',)), ('8192', ('8 GB',))]

# Graphics presets, after Lantern Launcher's idea (Low to Ultra, recommended from the GPU) but
# with this port's own settings: only the ones that cost GPU time. The upscaler (DLSS needs an
# RTX card, FSR 4 its assets), the output resolution (the display's) and the look-only effects
# (chromatic aberration, depth of field, motion blur, the game's AA) stay as the player set them.
# Balanced is the shipped defaults. Any other combination shows as Custom; nothing changes
# until a preset is chosen. All of it applies when the game next starts (the upscaling preset
# live as well when live resolution is on).
GRAPHICS_PRESETS = {
    'low': {'preset': '3', 'model_lod': '1', 'effect_ssao': '0', 'effect_dynamic_shadows': '0',
            'effect_ssr': '0', 'fps_mode': '60'},
    'balanced': {'preset': '2', 'model_lod': '0', 'effect_ssao': '1', 'effect_dynamic_shadows': '1',
                 'effect_ssr': '0', 'fps_mode': '120'},
    'high': {'preset': '1', 'model_lod': '0', 'effect_ssao': '1', 'effect_dynamic_shadows': '1',
             'effect_ssr': '0', 'fps_mode': '120'},
    'ultra': {'preset': '0', 'model_lod': '-2', 'effect_ssao': '1', 'effect_dynamic_shadows': '1',
              'effect_ssr': '1', 'fps_mode': '120'},
}
GRAPHICS_PRESET_INFO = {
    'low': ('Low', 'Performance upscaling (×2), lower model detail, no SSAO or dynamic-light shadows, 60 FPS. '
                   'For integrated graphics, the Steam Deck and cards with under 4 GB.'),
    'balanced': ('Balanced', "Balanced upscaling (×1.7) and the game's own detail, 120 FPS. "
                             'The default; for cards with 4-8 GB.'),
    'high': ('High', "Quality upscaling (×1.5) and the game's own detail, 120 FPS. "
                     'For cards with 8-12 GB.'),
    'ultra': ('Ultra', 'Native resolution, highest model detail and screen-space reflections (not in the '
                       'original game), 120 FPS. For cards with 12 GB or more.'),
    'custom': ('Custom', 'Your own combination. Choosing a preset changes only upscaling, model detail, SSAO, '
                         'dynamic-light shadows, reflections and the frame rate.'),
}


def graphics_preset_of(ini, app):
    """The preset the current settings match, or 'custom'."""
    for name, values in GRAPHICS_PRESETS.items():
        if all(str((app if key in APP_DEFAULTS else ini).get(key, '')) == value for key, value in values.items()):
            return name
    return 'custom'


def apply_graphics_preset(name, ini, app):
    for key, value in GRAPHICS_PRESETS[name].items():
        (app if key in APP_DEFAULTS else ini)[key] = value


def recommended_graphics_preset(gpu):
    """From bb-gpu-capabilities' "NAME, discrete, 8192 MiB" (the launcher's GPU status); None when
    the card is unknown. Video memory stands in for speed: a rough guide, said so in the UI."""
    match = re.search(r', (discrete|integrated or other), (\d+) MiB', gpu or '')
    if not match:
        return None
    memory = int(match.group(2))
    if match.group(1) != 'discrete' or memory < 3500:
        return 'low'
    return 'balanced' if memory < 7500 else 'high' if memory < 11500 else 'ultra'
UI_LANGUAGES = bbport_lang.LANGUAGE_NAMES

FSR4_COMMIT = 'ae8d628fae208813172446d1e49ed94150b04658'
FSR4_BASE = f'https://raw.githubusercontent.com/FireBurn/Q2RTX/{FSR4_COMMIT}/baseq2/fsr4_shaders'
# Output tiers of the v07 model: up to 1920x1080, up to 3840x2160, and wider or taller outputs
# (5120x1440, 7680x4320). Each tier's SPIR-V bakes its tensor pitch, so no tier stands in for another.
FSR4_TIERS = ('1080', '2160', '4320')


def fsr4_files():
    """The FSR 4 v07 asset set of tools/fetch_fsr4_assets.sh (every tier in FSR4_TIERS)."""
    files = ['LICENSE-FSR4-v07.txt', 'rcas.spv', 'spd_auto_exposure.spv']
    for model in ('native', 'quality', 'balanced', 'performance', 'ultraperf', 'drs'):
        files += [f'fsr4_model_v07_i8_{model}_initializers.bin', f'fsr4_model_v07_i8_{model}_pre_weights.bin',
                  f'fsr4_model_v07_i8_{model}_shader_manifest.json']
        for tier in FSR4_TIERS:
            files += [f'fsr4_model_v07_i8_{model}_{tier}_pre.spv', f'fsr4_model_v07_i8_{model}_{tier}_post.spv']
            files += [f'fsr4_model_v07_i8_{model}_{tier}_pass{n}.spv' for n in range(1, 13)]
    return files


def fsr4_missing():
    folder = PORT_DIR / 'fsr4_shaders'
    return [name for name in fsr4_files() if not (folder / name).is_file() or not (folder / name).stat().st_size]


def load_json(path, default):
    try:
        return json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return default


def ini_path():
    return Path(os.environ.get('BB_CONFIG', DATA_DIR / 'bbport.ini'))


def load_ini():
    values, lines = dict(INI_DEFAULTS), []
    try:
        lines = ini_path().read_text(encoding='utf-8').splitlines()
    except OSError:
        pass
    for line in lines:
        if '=' in line and not line.lstrip().startswith('#'):
            key, value = line.split('=', 1)
            values[key.strip()] = value.strip()
    return values, lines


def save_ini(values, lines):
    """Rewrites the edited keys in place, appends missing ones, keeps comments and other keys.
    A repeated key is written once: an edited one at its first line, another at its last."""
    keys = [line.split('=', 1)[0].strip() if '=' in line and not line.lstrip().startswith('#') else None
            for line in lines]
    last = {key: i for i, key in enumerate(keys)}
    written, out = set(), []
    for i, (line, key) in enumerate(zip(lines, keys)):
        if key in values:
            if key not in written:
                out.append(f'{key}={values[key]}')
                written.add(key)
            continue
        if key is not None and last[key] != i:
            continue  # a later line sets it, as load_ini reads it
        out.append(line)
    if not lines:
        out.append('# bbport settings (in-game menu: Insert / L3+R3)')
    out += [f'{key}={value}' for key, value in values.items() if key not in written]
    ini_path().write_text('\n'.join(out) + '\n', encoding='utf-8')


def game_info(folder):
    """(title, app version) of a game folder, or None without eboot.bin."""
    folder = Path(folder or '.')
    if not (folder / 'eboot.bin').is_file():
        return None
    try:
        from prepare import sfo
        from patches import game_app_version
        values = sfo((folder / 'sce_sys/param.sfo').read_bytes())
        # game_app_version: Game of the Year dumps say APP_VER 01.00 over the 1.09 code.
        version = game_app_version(folder) or values.get('APP_VER', '?')
        return values.get('TITLE', 'Bloodborne').replace('™', '').strip(), version
    except (OSError, ValueError, ImportError):
        return 'Bloodborne', '?'


def webapi_address(server):
    """The shadNet WebAPI of a server address host:port (the server's default port 31315)."""
    host = server.rsplit(':', 1)[0] if server.count(':') == 1 else server
    return f'http://{host}:31315'


def community_address(value):
    """The game server (messages, bloodstains, ghosts) as https://host, The Hunter's Dream by default."""
    value = str(value).strip() or COMMUNITY_SERVER
    scheme, _sep, rest = value.rpartition('://')
    host = rest.split('/')[0].split(':')[0]  # the game picks the ports (20443, 18671)
    return f'{scheme or "https"}://{host}'


def webapi_setting(s):
    """The WebAPI field, unless it holds the game server's address (a common mix-up): then the
    co-op server's own WebAPI is used."""
    webapi = str(s['online_webapi']).strip()
    community_host = community_address(s['online_community']).split('://', 1)[1].split(':')[0]
    if webapi and community_host in webapi:
        webapi = ''
    return webapi or webapi_address(str(s['online_server']).strip() or APP_DEFAULTS['online_server'])


def server_check(server, webapi, community):
    """A line about each online address: reachable, or why not (runs off the Tk thread)."""
    import socket
    lines = []
    try:
        with urllib.request.urlopen(community + ':20443/bb-eu/ss.info', timeout=6) as reply:
            reply.read(64)
        lines.append(_('Game server {0}: reachable').format(community))
    except (OSError, ValueError) as error:
        lines.append(_('Game server {0}: not reachable ({1})').format(community, error))
    host, _sep, port = server.rpartition(':')
    try:
        socket.create_connection((host or server, int(port or 31313)), timeout=4).close()
        lines.append(_('Server {0}: reachable').format(server))
    except (OSError, ValueError) as error:
        lines.append(_('Server {0}: not reachable ({1})').format(server, error))
    try:
        with urllib.request.urlopen(webapi.rstrip('/') + '/status', timeout=4) as reply:
            ok = b'"ok":true' in reply.read(512).replace(b' ', b'')
        lines.append(_('WebAPI {0}: reachable').format(webapi) if ok else
                     _('WebAPI {0}: answered, but it is not a shadNet server').format(webapi))
    except (OSError, ValueError) as error:
        lines.append(_('WebAPI {0}: not reachable ({1})').format(webapi, error))
    return '\n'.join(lines)


# The Hunter's Dream pool: the engine's own backend for it (BB_NET_BACKEND=thd, bbhost's
# protocol, Awaken's port as Supermedo took it in): the pool choice and the Discord sign-in on
# the Online page, as Supermedo shows them since its 1.8.1. Off for now: the server refuses this
# port's client (#10); BB_HUNTERS_DREAM=1 shows them again.
HUNTERS_DREAM_POOL = os.environ.get('BB_HUNTERS_DREAM') == '1'
ONLINE_POOLS = [('shadnet', ('shadNet server (address below)',)),
                ('huntersdream', ("The Hunter's Dream (Discord sign-in)",))]
# Which build a server sees: shadNet's client version and The Hunter's Dream's client field
# ("yharnam-pc/<version>"). This fork's own name, never an official shadPS4 one.
CLIENT_VERSION = f'yharnam-pc {VERSION}'


# How the launcher's PLAY starts the game: 'offline', 'huntersdream' (online on The Hunter's
# Dream, Discord sign-in) or 'coop' (seamless co-op on a server one of the players hosts).
PLAY_MODES = ('offline', 'huntersdream', 'coop')


def play_mode(s):
    """The play mode; older settings (Play online with The Hunter's Dream pool) map to it."""
    mode = s.get('play_mode')
    if mode == 'huntersdream' and not HUNTERS_DREAM_POOL:
        return 'offline'
    if mode in PLAY_MODES:
        return mode
    return 'huntersdream' if s.get('online') and s.get('online_pool') == 'huntersdream' else 'offline'


def hunters_dream_selected(s):
    return HUNTERS_DREAM_POOL and s.get('online_pool') == 'huntersdream'


def thd_call(url, body):
    """A call to The Hunter's Dream's account service (bbhost's): its reply, or ValueError with
    the server's reason."""
    request = urllib.request.Request(url, data=json.dumps(body).encode(), method='POST',
                                     headers={'Content-Type': 'application/json'})
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            text = response.read()
    except urllib.error.HTTPError as error:
        text = error.read()
    try:
        reply = json.loads(text)
    except ValueError:
        reply = None
    if not isinstance(reply, dict) or 'ResKind' not in reply:
        raise ValueError(f'{url} has no account service')
    if reply['ResKind'] != 0:
        raise ValueError(reply.get('Message') or 'refused')
    return reply


def thd_sign_in(game_server, on_code, cancelled=lambda: False):
    """Discord sign-in the way bbhost does it: a code to approve on the website. on_code(page,
    code) shows it. Returns (online id, token); raises ValueError."""
    if thd_refused():
        raise ValueError(_(*THD_REFUSED_NOTE))
    auth = community_address(game_server)
    start = thd_call(auth + '/auth/device/start',
                     {'DeviceName': f'yharnam-pc on {os.environ.get("COMPUTERNAME", "PC")}'})
    device, user_code = start.get('DeviceCode', ''), start.get('UserCode', '')
    if not device or not user_code:
        raise ValueError('no code in the reply')
    interval = max(1, int(start.get('Interval') or 5))
    deadline = time.monotonic() + int(start.get('ExpiresIn') or 600)
    # As bbhost opens it: the website's /link page with the code in its address, so after the
    # Discord sign-in there the page asks to approve this code. (Its "code from the game" box is
    # for the other direction: signing in to the website from an account made in the game.)
    page = start.get('VerificationUri') or auth + '/link'
    on_code(page + ('&' if '?' in page else '?') + 'code=' + urllib.parse.quote(user_code), user_code)
    while time.monotonic() < deadline and not cancelled():
        time.sleep(interval)
        poll = thd_call(auth + '/auth/device/poll', {'DeviceCode': device})
        status = poll.get('Status')
        if status == 'approved':
            if not poll.get('OnlineId') or not poll.get('Token'):
                raise ValueError('no token in the reply')
            return poll['OnlineId'], poll['Token']
        if status == 'slow_down':
            interval += 5
        elif status == 'denied':
            raise ValueError('the sign-in was denied')
        elif status == 'expired':
            break
    raise ValueError('the sign-in was cancelled' if cancelled() else 'the code expired; try again')


def server_check_hunters_dream(community):
    """Whether The Hunter's Dream's game server and bbhost service answer (off the UI thread)."""
    lines = []
    for title, url in ((_('Game server {0}'), community + ':20443/bb-eu/ss.info'),
                       (_('Co-op service {0}'), f'{community}:{COMMUNITY_API_PORT}/')):
        name = title.format(url.split('://', 1)[1].split('/')[0])
        try:
            urllib.request.urlopen(url, timeout=6).close()
            lines.append(_('{0}: reachable').format(name))
        except urllib.error.HTTPError:
            lines.append(_('{0}: reachable').format(name))   # it answered
        except (OSError, ValueError) as error:
            lines.append(_('{0}: not reachable ({1})').format(name, error))
    return '\n'.join(lines)


def thd_client_refused(reason):
    """Whether a 401's reason is the server refusing this build ("This client is not supported"),
    which a new sign-in cannot change; gpu/bbnet/thd/thd_session.cpp tells the two apart alike."""
    return 'client is not supported' in reason.casefold()


# The CLIENT_VERSION The Hunter's Dream refused. While it is this launcher's, nothing here signs in
# to it or starts the game on it (PLAY starts offline); a new version tries again once.
THD_REFUSAL = CONFIG_DIR / 'thd-refusal.json'
# The game's line for that refusal (gpu/bbnet/thd/thd_session.cpp MarkRefused).
THD_REFUSED_LINE = 'THD: the server refused this client ('
THD_REFUSED_NOTE = ("The Hunter's Dream doesn't accept this build; offline play and seamless co-op still work.",
                    "The Hunter's Dream не принимает эту сборку; офлайн-игра и бесшовный кооператив работают.")


def thd_refused():
    saved = load_json(THD_REFUSAL, {})
    return isinstance(saved, dict) and saved.get('client') == CLIENT_VERSION


def remember_thd_refusal(reason=''):
    try:
        CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        THD_REFUSAL.write_text(json.dumps({'client': CLIENT_VERSION, 'reason': reason, 'when': time.time()}),
                               encoding='utf-8')
    except OSError:
        pass


def note_game_output(line):
    """A line of the game's output; True when it is The Hunter's Dream refusing this build."""
    if THD_REFUSED_LINE not in line:
        return False
    remember_thd_refusal(line.strip())
    return True


def thd_account_check(community, online_id, token):
    """Whether The Hunter's Dream accepts this sign-in from this client: the game's own event
    poll (read-only, no ack), with the headers gpu/bbnet/thd/thd_session.cpp sends. A saved
    token alone says nothing: the server can still refuse the account or this build (HTTP 401)."""
    if thd_refused():
        return _(*THD_REFUSED_NOTE)
    if not online_id or not token:
        return _('Not signed in: use "Sign in with Discord".')
    # The game sends at most 16 characters of it (thd_session.cpp, BB_THD_ONLINE_ID).
    body = {'OnlineId': online_id[:16], 'Cursor': 0, 'Categories': ['matching2']}
    request = urllib.request.Request(f'{community}:{COMMUNITY_API_PORT}/np/events/poll',
                                     data=json.dumps(body).encode(),
                                     method='POST',
                                     headers={'Content-Type': 'application/json', 'Accept': 'application/json',
                                              'User-Agent': CLIENT_VERSION.replace(' ', '/'),
                                              'X-BBHost-Ruleset': os.environ.get('BB_THD_RULESET') or 'vanilla',
                                              'X-BB-Token': token})
    try:
        with urllib.request.urlopen(request, timeout=8) as response:
            reply = json.loads(response.read())
    except urllib.error.HTTPError as error:
        if error.code != 401:
            return _('Sign-in as {0}: not checked (HTTP {1})').format(online_id, error.code)
        try:
            reason = json.loads(error.read()).get('Message') or ''
        except (OSError, ValueError, AttributeError, http.client.HTTPException):
            reason = ''
        line = _('Sign-in as {0}: refused by the server ({1})').format(online_id, reason or 'HTTP 401')
        if thd_client_refused(reason):
            remember_thd_refusal(reason)
            line += '\n' + _('The server refuses this build, not the account: signing in again does not help.')
        return line
    except ValueError:
        # Answered, but not with the poll's JSON: a proxy, a captive portal or another service.
        return _("Sign-in as {0}: not checked (the reply is not from The Hunter's Dream)").format(online_id)
    except (OSError, http.client.HTTPException) as error:
        return _('Sign-in as {0}: not checked ({1})').format(online_id, connection_error_text(error))
    if not isinstance(reply, dict):
        return _("Sign-in as {0}: not checked (the reply is not from The Hunter's Dream)").format(online_id)
    return _('Signed in as {0}: accepted by the server').format(online_id)


# Button prompt packs: Kenney's Input Prompts (CC0) written into the player's own
# menu/common.tpf.dcx (scripts/prompt_icons.py, launcher/assets/prompts) and added to the mods
# folder (install_prompt_pack). The keyboard images show KEYBOARD_PROMPT_LAYOUT, so choosing them
# also binds those keys (bbport.ini key_*); the engine names keys in text (src/key_prompts.c) when
# the bindings differ from the images.
KEYBOARD_PROMPTS = 'Keyboard Prompts (Kenney)'
XBOX_PROMPTS = 'Xbox Prompts (Kenney)'
# choice: (mods folder name, launcher/assets/prompts file)
PROMPT_PACKS = {'Xbox': (XBOX_PROMPTS, 'xbox.json'), 'Keyboard': (KEYBOARD_PROMPTS, 'keyboard.json')}
# Prompt mods earlier launchers added (the player's Nexus download, or shipped with 1.9.x); still
# prompt mods, so a choice here turns them off.
LEGACY_KEYBOARD_PROMPTS = 'Keyboard Prompts (PCBButtons)'
LEGACY_PROMPT_MODS = (LEGACY_KEYBOARD_PROMPTS, 'Xbox Prompts (Dommo)', 'Xbox 360 Prompts (Elden Ring)')
KEYBOARD_PROMPT_LAYOUT = {
    'key_interact': 'E', 'key_roll': 'Left Shift', 'key_blood_vial': 'R', 'key_use_item': 'Q',
    'key_transform': 'F', 'key_attack': 'Mouse1', 'key_firearm': 'Mouse2',
    'key_strong_attack': 'none', 'key_strong_modifier': 'Left Ctrl', 'key_l3': 'Space',
    'key_lock_on': 'Mouse3', 'key_gestures': 'G', 'key_personal_effects': 'H', 'key_menu': 'Escape',
}


def install_prompt_pack(choice, game_dir, mods_dir):
    """Writes the pack's images into the game's menu/common.tpf.dcx as
    <mods>/<pack>/menu/common.tpf.dcx and returns that mod folder. ValueError when the game folder
    has no such file or it is not the 1.09 one."""
    import prompt_icons
    from fromsoft import FormatError
    name, data = PROMPT_PACKS[choice]
    source = Path(game_dir or '') / 'dvdroot_ps4' / 'menu' / 'common.tpf.dcx'
    if not source.is_file():
        raise ValueError(f'{choice} prompts are made from the game\'s menu/common.tpf.dcx: '
                         'choose the game folder first.')
    assets = PORT_DIR / 'launcher' / 'assets' / 'prompts'
    try:
        texture = prompt_icons.apply(source.read_bytes(), prompt_icons.load(assets / data))
    except FormatError as error:
        raise ValueError(f'{choice} prompts: {error}') from None
    target = Path(mods_dir) / name
    (target / 'menu').mkdir(parents=True, exist_ok=True)
    (target / 'menu' / 'common.tpf.dcx').write_bytes(texture)
    (target / 'SOURCE.txt').write_text(
        "Input Prompts by Kenney (www.kenney.nl), CC0.\n"
        'Written by the launcher into your own menu/common.tpf.dcx.\n', encoding='utf-8')
    return target


def game_environment(s):
    env = dict(os.environ)
    env['BB_PORT_VERSION'] = VERSION
    env['BB_CLIENT_VERSION'] = CLIENT_VERSION  # what shadNet and The Hunter's Dream see
    env['BB_GAME_DIR'] = s['game_dir']
    if s['user_dir']:
        env['BB_USER_DIR'] = s['user_dir']
    if s['dlc_dir']:
        env['BB_DLC_DIR'] = s['dlc_dir']
    env['BB_MODS_DIR'] = s['mods_dir'] or str(DATA_DIR / 'mods')
    env['BB_MODS_CONFIG'] = str(DATA_DIR / 'mods.json')
    env['BB_MODS_ENABLED'] = '1' if s['mods_enabled'] else '0'
    env['BB_PATCHES_DIR'] = s['patches_dir'] or str(DATA_DIR / 'patches')
    env['BB_PATCHES_CONFIG'] = str(DATA_DIR / 'patches.json')
    env['BB_LANGUAGE'] = s['language']
    env['BB_VOICE_LANGUAGE'] = s.get('voice_language') or 'auto'
    if s.get('audio_channels'):
        env['BB_AUDIO_CHANNELS'] = str(s['audio_channels'])
    if str(s['player_name']).strip():
        env['BB_USER_NAME'] = str(s['player_name']).strip()
    env['BB_FULLSCREEN'] = '1' if s['fullscreen'] else '0'
    env['BB_FULLSCREEN_MODE'] = s.get('fullscreen_mode') or 'borderless'
    if s.get('display'):
        env['BB_DISPLAY'] = str(s['display'])
    env['BB_PRESENT_MODE'] = s['present_mode']
    if s['hdr']:
        env['BB_HDR'] = '1'
    env['BB_FPS'] = s['fps_mode']
    if s.get('frame_cap', ''):
        env['BB_FPS_LIMIT'] = s['frame_cap']
    if str(s.get('vram_budget_mb', '')) in {value for value, _ in VRAM_BUDGETS if value}:
        env['BB_GC_BUDGET_MB'] = str(s['vram_budget_mb'])
    for key, name in (('draw_pipe', 'BB_DRAW_PIPE'), ('readbacks', 'BB_READBACKS'), ('frames_ahead', 'BB_FRAMES_AHEAD')):
        if s[key]:
            env[name] = s[key]
    for key, name in (('frame_stats', 'BB_FRAME_STATS'), ('gpu_profile', 'BB_GPU_PROFILE'),
                      ('vk_validation', 'BB_VK_VALIDATION')):
        if s[key]:
            env[name] = '1'
    for item in str(s['extra_env']).split():
        if '=' in item:
            key, value = item.split('=', 1)
            env[key] = value
    # Online only in The Hunter's Dream play mode, unless it refused this build; seamless co-op sets
    # up its own launch (bbport_coop.co_op_environment), and the shadNet-server pool is no longer offered.
    if s.get('online') and play_mode(s) == 'huntersdream' and not thd_refused():
        server = str(s['online_server']).strip() or APP_DEFAULTS['online_server']
        env['BB_ONLINE'] = '1'
        env['BB_SHADNET_SERVER'] = server
        env['BB_SHADNET_WEBAPI'] = webapi_setting(s)
        env['BB_SHADNET_NPID'] = str(s['online_npid']).strip()
        env['BB_SHADNET_PASSWORD'] = str(s['online_password'])
        env['BB_UPNP'] = '1' if s['online_upnp'] else '0'
        env['BB_COMMUNITY_SERVER'] = community_address(s['online_community'])
        if hunters_dream_selected(s):
            # The engine's Hunter's Dream backend with the Discord-linked account (run.py sets
            # the game server and the backend's addresses).
            env['BB_NET_BACKEND'] = 'thd'
            env['BB_THD_ONLINE_ID'] = str(s.get('thd_online_id', ''))
            env['BB_THD_TOKEN'] = str(s.get('thd_token', ''))
            for key in ('BB_SHADNET_SERVER', 'BB_SHADNET_WEBAPI', 'BB_SHADNET_NPID', 'BB_SHADNET_PASSWORD'):
                env.pop(key, None)
    env['PYTHONUNBUFFERED'] = '1'
    env['PYTHONIOENCODING'] = 'utf-8'
    return env


# ---------------------------------------------------------------------------------------------
# The window.

BG, PANEL, CARD, LINE = '#090d12', '#131920', '#1c232b', '#33404a'
TEXT, MUTED, GOLD, BLOOD, BLOOD_HI = '#f3f1eb', '#a3aeb7', '#d2bb85', '#8a2635', '#ba3446'
NAV_HOVER, NAV_ACTIVE, SUCCESS = '#242b32', '#48252c', '#8dcc93'


def round_rect(canvas, x1, y1, x2, y2, radius, fill, outline='', width=1, tags=()):
    """A rounded Canvas surface, shared by the guide-like sidebar and dashboard."""
    r = min(radius, (x2 - x1) / 2, (y2 - y1) / 2)
    points = [x1+r,y1, x2-r,y1, x2,y1, x2,y1+r,
              x2,y2-r, x2,y2, x2-r,y2, x1+r,y2,
              x1,y2, x1,y2-r, x1,y1+r, x1,y1]
    return canvas.create_polygon(points, smooth=True, splinesteps=16,
                                 fill=fill, outline=outline, width=width, tags=tags)


class Launcher:
    def __init__(self, root, tk, ttk, filedialog, messagebox):
        self.tk, self.ttk, self.filedialog, self.messagebox = tk, ttk, filedialog, messagebox
        self.root = root
        self.app = app_settings(load_json(CONFIG_FILE, {}))
        self.ini, self.ini_lines = load_ini()
        self.vars = {}
        self.process = self.job = None
        self.downloading = self.repairing = False
        self.output = queue.Queue()
        self.gpu_text = _('• Checking the graphics card…', '• Проверка видеокарты…')
        self.banner_source = self.banner_image = None
        self.hero_cache_key = None
        self.footer_cache_key = None
        self.wallpaper_cache_key = None
        self.glass_images = []
        self.check_texts = {}
        self.play_enabled = False
        self.ui_calls = queue.Queue()  # work for the Tk thread from helper threads
        self.mod_order, self.mod_vars, self.patch_vars = [], {}, {}
        self.mod_profile_mtime = None
        self.mod_dirty = False
        root.title('Bloodborne')
        root.configure(bg=BG)
        try:
            hwnd = ctypes.windll.user32.GetParent(root.winfo_id())
            dark = ctypes.c_int(1)
            ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, 20, ctypes.byref(dark), 4)
        except (AttributeError, OSError):
            pass
        self.dpi = root.winfo_fpixels('1i') / 96.0
        screen_w, screen_h = root.winfo_screenwidth(), root.winfo_screenheight()
        root.geometry(f'{min(self.px(1280), screen_w - self.px(36))}x'
                      f'{min(self.px(820), screen_h - self.px(80))}')
        root.minsize(min(self.px(1040), screen_w - self.px(36)),
                     min(self.px(700), screen_h - self.px(80)))
        self.set_icon()
        self.style()
        self.build()
        self.show('play')
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.after(100, self.drain_output)
        threading.Thread(target=self.detect_gpu, daemon=True).start()
        if self.app.get('check_updates', True) and not COOP_TEST:
            if remembered := remembered_update():  # found by a direct launch's quiet check
                self.offer_update(*remembered)
            threading.Thread(target=self.check_update, daemon=True).start()
        if missing := missing_port_files():
            root.after(300, lambda: self.offer_repair(missing))

    def px(self, size):
        return int(size * self.dpi)

    def set_icon(self):
        try:
            self.root.iconbitmap(default=str(PORT_DIR / 'launcher' / 'bloodborne.ico'))
            return
        except self.tk.TclError:
            pass
        for icon in (PORT_DIR / 'launcher' / 'bloodborne.png', Path(self.app['game_dir']) / 'sce_sys' / 'icon0.png'):
            try:
                self.icon = self.tk.PhotoImage(file=str(icon))
                if self.icon.width() > 128:
                    self.icon = self.icon.subsample(self.icon.width() // 64)
                self.root.iconphoto(True, self.icon)
                return
            except (self.tk.TclError, OSError):
                continue

    # ---- look --------------------------------------------------------------------------------
    def check_images(self):
        """16 px (DPI-scaled) check box images: the clam theme draws a cross."""
        tk = self.tk
        n = max(14, int(16 * self.dpi))
        images = []
        for checked in (False, True):
            image = tk.PhotoImage(width=n + self.px(8), height=n)  # unset pixels stay transparent
            fill = GOLD if checked else CARD
            image.put(GOLD if checked else '#62636e', to=(0, 0, n, n))
            image.put(fill, to=(1, 1, n - 1, n - 1))
            if checked:
                # Tick: down from (0.22n, 0.52n) to (0.42n, 0.72n), up to (0.78n, 0.30n).
                pts = []
                x0, y0, x1, y1, x2, y2 = .22 * n, .52 * n, .42 * n, .72 * n, .78 * n, .28 * n
                for t in range(40):
                    f = t / 39
                    pts.append((x0 + (x1 - x0) * f, y0 + (y1 - y0) * f))
                    pts.append((x1 + (x2 - x1) * f, y1 + (y2 - y1) * f))
                width = max(1, round(n / 9))
                for x, y in pts:
                    image.put('#ffffff', to=(int(x), int(y), int(x) + width, int(y) + width))
            images.append(image)
        self.check_off, self.check_on = images

    def style(self):
        ttk = self.ttk
        s = ttk.Style(self.root)
        s.theme_use('clam')
        self.check_images()
        s.element_create('Bb.indicator', 'image', self.check_off, ('selected', self.check_on), sticky='')
        s.layout('TCheckbutton', [('Checkbutton.padding', {'sticky': 'nswe', 'children': [
            ('Bb.indicator', {'side': 'left', 'sticky': ''}),
            ('Checkbutton.focus', {'side': 'left', 'sticky': 'w', 'children': [
                ('Checkbutton.label', {'sticky': 'nswe'})]})]})])
        base = ('Segoe UI', 10)
        self.root.option_add('*TCombobox*Listbox.background', CARD)
        self.root.option_add('*TCombobox*Listbox.foreground', TEXT)
        self.root.option_add('*TCombobox*Listbox.selectBackground', NAV_ACTIVE)
        self.root.option_add('*TCombobox*Listbox.font', base)
        s.configure('.', background=PANEL, foreground=TEXT, fieldbackground=CARD, bordercolor=LINE,
                    lightcolor=LINE, darkcolor=LINE, troughcolor=CARD, focuscolor=GOLD, font=base)
        s.configure('TFrame', background=PANEL)
        s.configure('TLabel', background=PANEL, foreground=TEXT)
        s.configure('Muted.TLabel', background=PANEL, foreground=MUTED, font=('Segoe UI', 9))
        s.configure('Section.TLabel', background=PANEL, foreground=GOLD,
                    font=('Segoe UI Semibold', 13))
        s.configure('Card.TFrame', background=CARD)
        s.configure('Card.TLabel', background=CARD, foreground=TEXT)
        s.configure('CardMuted.TLabel', background=CARD, foreground=MUTED, font=('Segoe UI', 9))
        s.configure('CardHeader.TLabel', background=CARD, foreground=TEXT,
                    font=('Segoe UI Semibold', 13))
        s.configure('Card.TCheckbutton', background=CARD, foreground=TEXT, padding=(0, 3))
        s.map('Card.TCheckbutton', background=[('active', CARD)], foreground=[('disabled', MUTED)])
        s.configure('TCheckbutton', background=PANEL, foreground=TEXT, padding=(0, 3))
        s.map('TCheckbutton', background=[('active', PANEL)], foreground=[('disabled', MUTED)])
        s.configure('Warning.TLabel', background='#2a1d12', foreground='#e3b25a', padding=(10, 6))
        s.configure('TCombobox', arrowcolor=GOLD, foreground=TEXT, padding=4)
        s.map('TCombobox', fieldbackground=[('readonly', CARD)], foreground=[('readonly', TEXT)],
              selectbackground=[('readonly', CARD)], selectforeground=[('readonly', TEXT)])
        s.configure('TEntry', foreground=TEXT, insertcolor=TEXT, padding=4)
        s.configure('TSpinbox', foreground=TEXT, arrowcolor=GOLD, insertcolor=TEXT, padding=4)
        s.configure('TButton', background=CARD, foreground=TEXT, padding=(14, 8), borderwidth=1)
        s.map('TButton', background=[('active', LINE), ('disabled', PANEL)], foreground=[('disabled', MUTED)])
        s.configure('Play.TButton', background=BLOOD, foreground='#ffffff', font=('Segoe UI Semibold', 15),
                    padding=(36, 10), borderwidth=0)
        s.map('Play.TButton', background=[('active', BLOOD_HI), ('disabled', '#3b3036')],
              foreground=[('disabled', '#97949a')])
        s.configure('Horizontal.TScale', background=BLOOD, troughcolor=CARD, bordercolor=LINE)
        s.configure('Horizontal.TProgressbar', background=BLOOD_HI, troughcolor=CARD, bordercolor=LINE)
        s.configure('Vertical.TScrollbar', background=CARD, arrowcolor=GOLD, troughcolor=PANEL, bordercolor=PANEL)

    # ---- widget helpers ----------------------------------------------------------------------
    def var(self, key, store):
        """The Tk variable of a setting (one per setting, shared by every widget that shows it)."""
        tk = self.tk
        if key not in self.vars:
            if store == 'ini':
                value = self.ini.get(key, INI_DEFAULTS[key])
                if key in INI_FLAGS:
                    v = tk.BooleanVar(value=value == '1')
                elif key == 'sharpness':
                    v = tk.DoubleVar(value=float(value or 0.5))
                else:
                    v = tk.StringVar(value=value)
            else:
                value = self.app.get(key, APP_DEFAULTS[key])
                if isinstance(APP_DEFAULTS[key], bool):
                    v = tk.BooleanVar(value=bool(value))
                else:
                    v = tk.StringVar(value=str(value))
            v.store = store
            self.vars[key] = v
        return self.vars[key]

    def choice(self, parent, key, store, options, width=38):
        """A combobox over (value, (english, russian)) pairs, kept in sync with its variable."""
        var = self.var(key, store)
        values = [v for v, _t in options]
        box = self.ttk.Combobox(parent, values=[_(*text) for _v, text in options],
                               state='normal' if key == 'output_res' else 'readonly', width=width)
        if var.get() not in values and key != 'output_res':
            var.set(values[0])

        def show(*_args):
            current = var.get()
            if current in values:
                box.current(values.index(current))
            else:
                box.set(current)
        show()
        box.bind('<<ComboboxSelected>>', lambda _e: var.set(values[box.current()]))
        if key == 'output_res':
            def custom(_event):
                if box.current() >= 0:
                    return
                text = box.get().strip().lower().replace(' ', '')
                try:
                    w, h = map(int, text.split('x'))
                    if 256 <= w <= 7680 and 144 <= h <= 4320 and not (w % 2 or h % 2):
                        var.set(f'{w}x{h}')
                    else:
                        show()
                except ValueError:
                    show()
            box.bind('<FocusOut>', custom)
            box.bind('<Return>', custom)
        var.trace_add('write', show)
        return box

    def next_row(self, parent):
        return parent.grid_size()[1]

    def row(self, parent, title, widget, hint=None):
        ttk = self.ttk
        r = self.next_row(parent)
        label_style = 'Card.TLabel' if parent.cget('style') == 'Card.TFrame' else 'TLabel'
        ttk.Label(parent, text=title, style=label_style).grid(
            row=r, column=0, sticky='nw', padx=(0, 22), pady=(12, 0))
        widget.grid(row=r, column=1, sticky='w', pady=(9, 0))
        if hint:
            hint_style = 'CardMuted.TLabel' if label_style == 'Card.TLabel' else 'Muted.TLabel'
            ttk.Label(parent, text=hint, style=hint_style, wraplength=self.px(560), justify='left').grid(
                row=r + 1, column=1, sticky='w', pady=(2, 2))
        return widget

    def check(self, parent, key, store, title, hint=None):
        ttk = self.ttk
        r = self.next_row(parent)
        ttk.Checkbutton(parent, text=title, variable=self.var(key, store)).grid(
            row=r, column=0, columnspan=2, sticky='w', pady=(8, 0))
        if hint:
            ttk.Label(parent, text=hint, style='Muted.TLabel', wraplength=self.px(640), justify='left').grid(
                row=r + 1, column=0, columnspan=2, sticky='w', padx=(26, 0))

    def note(self, parent, text, top=12):
        self.ttk.Label(parent, text=text, style='Muted.TLabel', wraplength=self.px(680), justify='left').grid(
            row=self.next_row(parent), column=0, columnspan=2, sticky='w', pady=(top, 0))

    def section(self, parent, title, top=18):
        if self.next_row(parent):
            self.ttk.Separator(parent,orient='horizontal').grid(
                row=self.next_row(parent),column=0,columnspan=2,sticky='ew',pady=(top,0))
            top=8
        self.ttk.Label(parent, text=title, style='Section.TLabel').grid(
            row=self.next_row(parent), column=0, columnspan=2, sticky='w', pady=(top, 2))

    def folder(self, parent, key, title, prompt, hint=None, on_change=None):
        ttk = self.ttk
        var = self.var(key, 'app')
        holder = ttk.Frame(parent)
        ttk.Entry(holder, textvariable=var, width=54).pack(side='left')

        def browse():
            chosen = self.filedialog.askdirectory(title=prompt, initialdir=var.get() or str(PORT_DIR))
            if chosen:
                var.set(str(Path(chosen)))
        ttk.Button(holder, text=_('Browse…', 'Обзор…'), command=browse).pack(side='left', padx=(6, 0))
        ttk.Button(holder, text=_('Open', 'Открыть'), command=lambda: self.open_path(var.get(), key)).pack(
            side='left', padx=(6, 0))
        if on_change:
            var.trace_add('write', lambda *_a: on_change())
        return self.row(parent, title, holder, hint)

    def scrolled_page(self, name, title, subtitle):
        """A settings page: a heading and content that scrolls vertically."""
        tk, ttk = self.tk, self.ttk
        outer = ttk.Frame(self.content)
        head = tk.Canvas(outer,height=self.px(132),bg=BG,highlightthickness=0,bd=0)
        head.pack(fill='x')
        def draw_header(_event=None):
            head.delete('all')
            w=head.winfo_width();s=self.dpi
            head.create_rectangle(0,0,w,self.px(132),fill='#101a23',outline='')
            for i in range(8):
                head.create_rectangle(w-i*self.px(65),0,w-(i-1)*self.px(65),self.px(132),
                                      fill=('#17232b' if i%2 else '#142028'),outline='')
            head.create_rectangle(0,self.px(128),w,self.px(132),fill=BLOOD,outline='')
            head.create_text(self.px(30),self.px(22),text='BLOODBORNE  /  '+_('OPTIONS','НАСТРОЙКИ'),
                             anchor='w',fill=GOLD,font=('Segoe UI Semibold',9))
            head.create_text(self.px(30),self.px(60),text=title,anchor='w',fill=TEXT,
                             font=('Segoe UI Semibold',24))
            head.create_text(self.px(30),self.px(103),text=subtitle,anchor='w',fill=MUTED,
                             font=('Segoe UI',10),width=max(self.px(500),w-self.px(65)))
        head.bind('<Configure>',draw_header)
        canvas = tk.Canvas(outer, bg=PANEL, highlightthickness=0, bd=0)
        bar = ttk.Scrollbar(outer, orient='vertical', command=canvas.yview)
        inner = ttk.Frame(canvas, padding=(30, 18, 30, 30))
        inner.columnconfigure(1, weight=1)
        window = canvas.create_window(0, 0, window=inner, anchor='nw')
        inner.bind('<Configure>', lambda _e: canvas.configure(scrollregion=canvas.bbox('all')))
        canvas.bind('<Configure>', lambda e: canvas.itemconfigure(window, width=e.width))
        canvas.configure(yscrollcommand=bar.set)
        bar.pack(side='right', fill='y')
        canvas.pack(side='left', fill='both', expand=True)
        outer.scroll = lambda units: canvas.yview_scroll(units, 'units') if inner.winfo_height() > canvas.winfo_height() else None
        self.pages[name] = outer
        return inner

    # ---- layout ------------------------------------------------------------------------------
    def build(self):
        tk, ttk = self.tk, self.ttk
        self.nav_items = (('play', '▶', _('Play', 'Играть')),
                          ('graphics', '▣', _('Graphics', 'Графика')),
                          ('display', '▤', _('Display & FPS', 'Экран и FPS')),
                          ('game', '◈', _('Game & effects', 'Игра и эффекты')),
                          ('cheats', '✦', _('Cheats', 'Читы')),
                          ('online', '◎', _('Online co-op', 'Онлайн-кооператив')),
                          ('mods', '▦', _('Mods & patches', 'Моды и патчи')),
                          ('advanced', '⚙', _('Advanced', 'Дополнительно')),
                          ('log', '≡', _('Log', 'Журнал')))
        self.current_page = None
        self.nav_hovered = None
        self.side = tk.Canvas(self.root, bg=BG, width=self.px(254), highlightthickness=0)
        self.side.pack(side='left', fill='y')
        self.side.bind('<Configure>', lambda _e: self.draw_sidebar())
        self.side.bind('<Key-Up>', lambda _e: self.move_nav(-1))
        self.side.bind('<Key-Down>', lambda _e: self.move_nav(1))
        self.side.bind('<Key-Return>', lambda _e: self.show(self.nav_items[self.nav_focus][0]))
        self.nav_focus = 0
        self.update_box = None
        right = tk.Frame(self.root, bg=PANEL)
        right.pack(side='left', fill='both', expand=True)
        bar = tk.Frame(right, bg=BG, height=self.px(48))
        bar.pack(fill='x', side='bottom')
        bar.pack_propagate(False)
        self.status = tk.Label(bar, text='', bg=BG, fg=MUTED, font=('Segoe UI', 9),
                               anchor='w', justify='left')
        self.status.pack(side='left', padx=self.px(22))
        self.stop_button = ttk.Button(bar, text=_('Stop game', 'Остановить игру'), command=self.stop, state='disabled')
        self.stop_button.pack(side='right', padx=self.px(22), pady=self.px(5))
        self.content = tk.Frame(right, bg=PANEL)
        self.content.pack(fill='both', expand=True)

        self.pages = {}
        self.build_play()
        self.build_graphics()
        self.build_display()
        self.build_game()
        self.build_cheats()
        self.build_online()
        self.build_mods()
        self.build_advanced()
        self.build_log()
        self.root.bind_all('<MouseWheel>', self.wheel)
        self.root.bind_all('<Control-Tab>', lambda _e: self.move_nav(1))
        self.root.bind_all('<Control-Shift-Tab>', lambda _e: self.move_nav(-1))
        self.refresh_status()

    def move_nav(self, step):
        self.nav_focus = (self.nav_focus + step) % len(self.nav_items)
        self.show(self.nav_items[self.nav_focus][0])
        self.side.focus_set()

    def wallpaper(self):
        """One cover-cropped scene shared by the sidebar and dashboard canvases."""
        width=max(self.root.winfo_width(),self.px(800))
        height=max(self.root.winfo_height(),self.px(600))
        source=HERO_ART if HERO_ART.is_file() else Path(self.var('game_dir','app').get() or '.')/'sce_sys'/'pic1.png'
        key=(str(source),width,height)
        if self.wallpaper_cache_key==key:
            return self.wallpaper_image
        self.wallpaper_cache_key,self.wallpaper_image=key,None
        try:
            from PIL import Image
            with Image.open(source) as image:
                image=image.convert('RGB')
                scale=max(width/image.width,height/image.height)
                image=image.resize((round(image.width*scale),round(image.height*scale)),Image.Resampling.LANCZOS)
                left=round((image.width-width)*.46)
                top=round((image.height-height)*.42)
                self.wallpaper_image=image.crop((left,top,left+width,top+height))
        except (ImportError,OSError,ValueError):
            pass
        return self.wallpaper_image

    def glass_surface(self,canvas,x1,y1,x2,y2,radius,tint=(13,20,28,205)):
        """Blur and tint only the pixels beneath this card; content is drawn separately."""
        if self.dashboard_backdrop is None:
            round_rect(canvas,x1,y1,x2,y2,radius,CARD,LINE)
            return
        try:
            from PIL import Image, ImageDraw, ImageFilter, ImageTk
            box=(round(x1),round(y1),round(x2),round(y2))
            area=self.dashboard_backdrop.crop(box).filter(ImageFilter.GaussianBlur(6)).convert('RGBA')
            area=Image.alpha_composite(area,Image.new('RGBA',area.size,tint))
            mask=Image.new('L',area.size)
            ImageDraw.Draw(mask).rounded_rectangle((0,0,area.width-1,area.height-1),
                                                     radius=round(radius),fill=255)
            glass=Image.new('RGBA',area.size)
            glass.paste(area,(0,0),mask)
            photo=ImageTk.PhotoImage(glass)
            self.glass_images.append(photo)
            canvas.create_image(x1,y1,image=photo,anchor='nw')
            round_rect(canvas,x1,y1,x2,y2,radius,'','#63707a')
            canvas.create_line(x1+radius,y1+1,x2-radius,y1+1,fill='#687780')
        except (ImportError,OSError,ValueError):
            round_rect(canvas,x1,y1,x2,y2,radius,CARD,LINE)

    def nav_icon(self,canvas,name,x,y,color,tag):
        """Consistent thin-line icons instead of platform-dependent Unicode glyphs."""
        u=self.dpi
        line=lambda *p:canvas.create_line(*p,fill=color,width=max(1,1.6*u),capstyle='round',tags=(tag,))
        rect=lambda a,b,c,d:canvas.create_rectangle(a,b,c,d,outline=color,width=max(1,1.5*u),tags=(tag,))
        oval=lambda a,b,c,d:canvas.create_oval(a,b,c,d,outline=color,width=max(1,1.5*u),tags=(tag,))
        if name=='play':
            canvas.create_polygon(x-5*u,y-8*u,x+8*u,y,x-5*u,y+8*u,fill=color,outline='',tags=(tag,))
        elif name=='graphics':
            rect(x-9*u,y-7*u,x+9*u,y+5*u);line(x,y+5*u,x,y+9*u);line(x-5*u,y+9*u,x+5*u,y+9*u)
        elif name=='display':
            rect(x-8*u,y-8*u,x+8*u,y+8*u);line(x-4*u,y+4*u,x+4*u,y+4*u)
        elif name=='game':
            for offset,k in ((-6,-3),(0,4),(6,-1)):
                line(x-9*u,y+offset*u,x+9*u,y+offset*u)
                oval(x+(k-2)*u,y+(offset-2)*u,x+(k+2)*u,y+(offset+2)*u)
        elif name=='cheats':
            canvas.create_polygon(x,y-10*u,x+2*u,y-2*u,x+9*u,y,x+2*u,y+2*u,
                                  x,y+10*u,x-2*u,y+2*u,x-9*u,y,x-2*u,y-2*u,
                                  outline=color,fill='',width=max(1,1.5*u),tags=(tag,))
        elif name=='online':
            oval(x-8*u,y-7*u,x-1*u,y);oval(x+2*u,y-7*u,x+9*u,y)
            line(x-10*u,y+7*u,x-8*u,y+3*u,x-1*u,y+2*u,x+2*u,y+5*u)
            line(x,y+5*u,x+3*u,y+2*u,x+9*u,y+3*u,x+11*u,y+7*u)
        elif name=='mods':
            for offset in (-5,0,5):
                line(x-8*u,y+(offset-3)*u,x,y+(offset+1)*u,x+8*u,y+(offset-3)*u)
        elif name=='advanced':
            oval(x-5*u,y-5*u,x+5*u,y+5*u);oval(x-2*u,y-2*u,x+2*u,y+2*u)
            for dx,dy in ((0,-9),(0,9),(-9,0),(9,0),(-6,-6),(6,6),(-6,6),(6,-6)):
                line(x+dx*.65,y+dy*.65,x+dx*u,y+dy*u)
        else:
            for offset in (-5,0,5):
                line(x-9*u,y+offset*u,x+9*u,y+offset*u)

    def draw_sidebar(self):
        c = self.side
        c.delete('all')
        self.nav_shapes = {}
        w, h = max(c.winfo_width(), self.px(254)), max(c.winfo_height(), self.px(650))
        s = self.dpi
        rr = lambda x1,y1,x2,y2,r,fill,outline='': round_rect(c,x1*s,y1*s,x2*s,y2*s,r*s,fill,outline)
        background=self.wallpaper()
        if background:
            try:
                from PIL import Image, ImageDraw, ImageFilter, ImageTk
                original=background.crop((0,0,w,h)).convert('RGBA')
                blurred=original.filter(ImageFilter.GaussianBlur(8))
                glass=Image.alpha_composite(blurred,Image.new('RGBA',blurred.size,(8,13,20,207)))
                mask=Image.new('L',(w,h))
                ImageDraw.Draw(mask).rounded_rectangle((self.px(10),self.px(12),w-self.px(10),h-self.px(12)),
                                                         radius=self.px(16),fill=255)
                original.paste(glass,(0,0),mask)
                draw=ImageDraw.Draw(original)
                draw.rounded_rectangle((self.px(10),self.px(12),w-self.px(10),h-self.px(12)),
                                       radius=self.px(16),outline=(116,128,138,115),width=1)
                self.side_background=ImageTk.PhotoImage(original)
                c.create_image(0,0,image=self.side_background,anchor='nw')
            except (ImportError,OSError,ValueError):
                rr(10,12,w/s-10,h/s-12,16,'#151b22',LINE)
        else:
            rr(10,12,w/s-10,h/s-12,16,'#151b22',LINE)
        try:
            if not hasattr(self, 'side_icon'):
                self.side_icon = self.tk.PhotoImage(file=str(PORT_DIR / 'launcher' / 'bloodborne.png')).subsample(2)
            c.create_image(28*s,32*s,image=self.side_icon,anchor='nw')
        except (self.tk.TclError, OSError):
            pass
        c.create_text(83*s,43*s,text='Bloodborne',anchor='w',fill=TEXT,font=('Georgia',14))
        c.create_text(83*s,68*s,text=_('WINDOWS EDITION','ВЕРСИЯ ДЛЯ WINDOWS'),anchor='w',fill=GOLD,
                      font=('Segoe UI Semibold',8))
        c.create_line(26*s,105*s,w-26*s,105*s,fill=LINE)
        for i,(name,icon,title) in enumerate(self.nav_items):
            y = (122+i*51)*s if i < 7 else (122+i*51+15)*s
            if i == 7:
                c.create_line(26*s,y-15*s,w-26*s,y-15*s,fill=LINE)
            active = name == self.current_page
            hover = name == self.nav_hovered
            fill = NAV_ACTIVE if active else NAV_HOVER if hover else '#151b22'
            tag = f'nav_{name}'
            self.nav_shapes[name] = round_rect(c,20*s,y,w-20*s,y+45*s,9*s,fill,tags=(tag,))
            if not active:
                c.itemconfigure(self.nav_shapes[name],stipple='gray50' if hover else 'gray12')
            if active:
                round_rect(c,21*s,y+7*s,24*s,y+38*s,2*s,BLOOD_HI,tags=(tag,))
            self.nav_icon(c,name,44*s,y+22*s,GOLD if active else MUTED,tag)
            c.create_text(66*s,y+22*s,text=title,anchor='w',fill=TEXT if active else '#cbd1d5',
                          font=('Segoe UI Semibold' if active else 'Segoe UI',10),tags=(tag,))
            c.tag_bind(tag,'<Button-1>',lambda _e,n=name: self.show(n))
            c.tag_bind(tag,'<Enter>',lambda _e,n=name: self.nav_hover(n,True))
            c.tag_bind(tag,'<Leave>',lambda _e,n=name: self.nav_hover(n,False))
        c.create_text(28*s,h-76*s,text=f'WINDOWS  ·  v{VERSION}',anchor='w',fill=GOLD,
                      font=('Segoe UI Semibold',9))
        c.create_text(28*s,h-52*s,text=_('Insert / L3+R3  ·  In-game guide',
                                         'Insert / L3+R3  ·  Игровое меню'),anchor='w',fill=MUTED,
                      font=('Segoe UI',8))

    def nav_hover(self, name, hover):
        self.nav_hovered = name if hover else None
        self.side.configure(cursor='hand2' if hover else '')
        if name != self.current_page and name in self.nav_shapes:
            self.side.itemconfigure(self.nav_shapes[name],fill=NAV_HOVER if hover else '#151b22',
                                    stipple='gray50' if hover else 'gray12')

    def wheel(self, event):
        page = self.pages.get(self.current_page)
        if hasattr(page, 'scroll') and not isinstance(event.widget, (self.tk.Text, self.ttk.Combobox)) \
                and 'popdown' not in str(event.widget):
            page.scroll(int(-event.delta / 120))

    def show(self, name):
        for page in self.pages.values():
            page.pack_forget()
        self.pages[name].pack(fill='both', expand=True)
        self.current_page = name
        self.nav_focus = next(i for i,item in enumerate(self.nav_items) if item[0] == name)
        self.draw_sidebar()
        if name == 'mods':
            self.refresh_lists()
        elif name == 'graphics':
            self.refresh_fsr4()
        elif name == 'play':
            self.refresh_status()

    def build_play(self):
        tk = self.tk
        page = tk.Frame(self.content, bg=BG)
        self.pages['play'] = page
        self.dashboard = tk.Canvas(page, bg=BG, highlightthickness=0, bd=0)
        self.dashboard.pack(fill='both', expand=True)
        self.dashboard.bind('<Configure>', lambda _e: self.draw_dashboard())
        for key,store in (('fps_mode','app'),('upscaler','ini'),('preset','ini'),
                          ('output_res','ini'),('fullscreen','app'),('online','app')):
            self.var(key,store).trace_add('write', lambda *_a: self.refresh_status())

    def draw_banner(self):
        self.hero_cache_key = None
        self.draw_dashboard()

    def hero_art(self, path, width, height):
        """Full-scene backdrop with a gentle reading gradient under the controls."""
        key = (str(HERO_ART if HERO_ART.is_file() else path), width, height,
               self.root.winfo_width(), self.root.winfo_height())
        if self.hero_cache_key == key:
            return self.banner_image
        self.hero_cache_key, self.banner_image, self.dashboard_backdrop = key, None, None
        try:
            from PIL import Image, ImageDraw, ImageTk
            wallpaper=self.wallpaper()
            left=self.side.winfo_width()
            source=wallpaper.crop((left,0,left+width,height)).convert('RGBA')
            shade=Image.new('RGBA',(width,height),(0,0,0,0))
            draw = ImageDraw.Draw(shade)
            for y in range(0,height,4):
                t=y/max(height-1,1)
                opacity=round(24+113*t*t)
                draw.rectangle((0,y,width,y+4),fill=(7,11,17,opacity))
            source=Image.alpha_composite(source,shade)
            left_shade=Image.new('RGBA',(width,height),(0,0,0,0))
            draw=ImageDraw.Draw(left_shade)
            for x in range(0,round(width*.65),6):
                opacity=round(70*(1-x/max(width*.65,1)))
                draw.rectangle((x,0,x+6,height),fill=(3,6,11,opacity))
            self.dashboard_backdrop=Image.alpha_composite(source,left_shade)
            self.banner_image=ImageTk.PhotoImage(self.dashboard_backdrop)
        except (ImportError, OSError, ValueError):
            try:
                original = self.tk.PhotoImage(file=str(path))
                self.banner_image = original.subsample(max(1,original.width()//width))
            except (self.tk.TclError, OSError):
                pass
        return self.banner_image

    def canvas_action(self, canvas, tag, command):
        canvas.tag_bind(tag,'<Button-1>',lambda _e: command())
        canvas.tag_bind(tag,'<Enter>',lambda _e: canvas.configure(cursor='hand2'))
        canvas.tag_bind(tag,'<Leave>',lambda _e: canvas.configure(cursor=''))

    def footer_art(self, path, width, height):
        source_path=STRIP_ART if STRIP_ART.is_file() else path
        key=(str(source_path),width,height)
        if self.footer_cache_key==key:
            return self.footer_image
        self.footer_cache_key,self.footer_image=key,None
        try:
            from PIL import Image, ImageDraw, ImageEnhance, ImageTk
            with Image.open(source_path) as source:
                source=source.convert('RGB')
                scale=max(width/source.width,height/source.height)
                source=source.resize((round(source.width*scale),round(source.height*scale)),Image.Resampling.LANCZOS)
                left=(source.width-width)//2
                top=min(max(0,round(source.height*.15)),source.height-height)
                source=ImageEnhance.Brightness(source.crop((left,top,left+width,top+height))).enhance(.70)
            shade=Image.new('RGBA',(width,height))
            draw=ImageDraw.Draw(shade)
            for x in range(0,width,5):
                draw.rectangle((x,0,x+5,height),fill=(9,13,18,round(215*(1-x/max(width,1))+35)))
            combined=Image.alpha_composite(source.convert('RGBA'),shade)
            mask=Image.new('L',(width,height))
            ImageDraw.Draw(mask).rounded_rectangle((0,0,width-1,height-1),radius=self.px(10),fill=255)
            finished=Image.new('RGBA',(width,height))
            finished.paste(combined,(0,0),mask)
            self.footer_image=ImageTk.PhotoImage(finished)
        except (ImportError,OSError,ValueError):
            pass
        return self.footer_image

    def controller_connected(self):
        try:
            dll = ctypes.windll.xinput1_4
            return any(dll.XInputGetState(i,ctypes.create_string_buffer(16)) == 0 for i in range(4))
        except (AttributeError, OSError):
            return False

    def draw_dashboard(self):
        c = self.dashboard
        c.delete('all')
        self.glass_images=[]
        w,h = c.winfo_width(),c.winfo_height()
        if w < 350 or h < 350:
            return
        s = self.dpi
        hero_h = min(max(round(h*.52),self.px(315)),self.px(440))
        art = Path(self.var('game_dir','app').get() or '.')/'sce_sys'/'pic1.png'
        photo = self.hero_art(art,w,h)
        if photo:
            c.create_image(0,0,image=photo,anchor='nw')
        else:
            c.create_rectangle(0,0,w,h,fill=BG,outline='')
        x = self.px(30)
        c.create_text(x+self.px(2),hero_h-self.px(220)+self.px(2),text='Bloodborne',anchor='w',
                      fill='#070a0e',font=('Georgia',48))
        c.create_text(x,hero_h-self.px(220),text='Bloodborne',anchor='w',
                      fill='#d8c38e',font=('Georgia',48))
        c.create_text(x,hero_h-self.px(137),text=_('RETURN TO YHARNAM','ВОЗВРАЩЕНИЕ В ЯРНАМ'),anchor='w',
                      fill=GOLD,font=('Segoe UI Semibold',9))
        c.create_text(x,hero_h-self.px(111),text=_('Face your nightmares once more.',
                                                   'Встретьтесь со своими кошмарами вновь.'),anchor='w',
                      fill=TEXT,font=('Segoe UI',13))
        play_y = hero_h-self.px(83)
        play_tag='dashboard_play'
        play_fill = BLOOD if self.play_enabled else '#3d3034'
        round_rect(c,x,play_y,x+self.px(205),play_y+self.px(49),8*s,play_fill,
                   '#c15160' if self.play_enabled else '#534349',tags=(play_tag,))
        c.create_text(x+self.px(25),play_y+self.px(25),text='▶',fill=TEXT,
                      font=('Segoe UI Symbol',14),tags=(play_tag,))
        c.create_text(x+self.px(106),play_y+self.px(25),text=_('PLAY','ИГРАТЬ'),fill=TEXT,
                      font=('Segoe UI Semibold',13),tags=(play_tag,))
        if self.play_enabled:
            self.canvas_action(c,play_tag,self.play)
        options_tag='dashboard_options'
        ox=x+self.px(218)
        round_rect(c,ox,play_y,ox+self.px(176),play_y+self.px(49),8*s,'#222b33','#53616d',tags=(options_tag,))
        c.create_text(ox+self.px(88),play_y+self.px(25),text=_('⚙   GAME OPTIONS','⚙   НАСТРОЙКИ'),
                      fill=TEXT,font=('Segoe UI Semibold',10),tags=(options_tag,))
        self.canvas_action(c,options_tag,lambda:self.show('graphics'))

        gap=self.px(13); margin=self.px(22)
        card_y=hero_h+self.px(12)
        card_h=min(self.px(215),max(self.px(164),h-card_y-self.px(103)))
        card_w=(w-2*margin-2*gap)/3
        cards=[]
        for index in range(3):
            left=margin+index*(card_w+gap)
            right=left+card_w
            shadow=round_rect(c,left+self.px(3),card_y+self.px(5),right+self.px(3),
                              card_y+card_h+self.px(5),12*s,'#080c11')
            c.itemconfigure(shadow,stipple='gray50')
            self.glass_surface(c,left,card_y,right,card_y+card_h,12*s)
            cards.append((left,right))
        def card_title(index,icon,title):
            left,right=cards[index]
            c.create_text(left+self.px(16),card_y+self.px(22),text=icon,anchor='w',fill=GOLD,
                          font=('Segoe UI Symbol',15))
            c.create_text(left+self.px(46),card_y+self.px(23),text=title,anchor='w',fill=TEXT,
                          font=('Segoe UI Semibold',11))
            c.create_line(left+self.px(16),card_y+self.px(45),right-self.px(16),card_y+self.px(45),fill=LINE)
        card_title(0,'◈',_('Game status','Состояние игры'))
        card_title(1,'▣',_('Current setup','Текущие настройки'))
        card_title(2,'♧',_('Controller & guide','Геймпад и меню'))
        def line(index,row,label,value=None,color=TEXT,action=None):
            left,right=cards[index]
            y=card_y+self.px(67+row*30)
            if value is None:
                c.create_text(left+self.px(17),y,text=label,anchor='w',fill=color,
                              font=('Segoe UI',9),width=card_w-self.px(34))
            else:
                c.create_text(left+self.px(17),y,text=label,anchor='w',fill=MUTED,font=('Segoe UI',9))
                c.create_text(right-self.px(17),y,text=value,anchor='e',fill=color,font=('Segoe UI Semibold',9),
                              width=card_w*.57)
            if action:
                tag=f'card_{index}_{row}'
                c.create_rectangle(left+self.px(12),y-self.px(13),right-self.px(12),y+self.px(15),
                                   fill='',outline='',tags=(tag,))
                self.canvas_action(c,tag,action)
        info=game_info(self.var('game_dir','app').get())
        line(0,0,_('Game ready','Игра готова') if info else _('Choose game folder','Выберите папку игры'),
             color=SUCCESS if info else '#e1ad65',action=lambda:self.show('game'))
        if info:
            line(0,1,_('Game version','Версия игры'),info[1],
                 SUCCESS if info[1]==PATCH_VERSION else '#e1ad65')
        line(0,2,self.check_texts.get('saves',_('Checking saves…','Проверка сохранений…')).lstrip('✓• '))
        missing=bool(fsr4_missing())
        line(0,3,_('FSR 4 assets missing  ·  Details →','Нет FSR 4  ·  Подробнее →') if missing
             else _('FSR 4 assets installed','FSR 4 установлены'),
             color='#e1ad65' if missing else SUCCESS,action=(lambda:self.show('graphics')) if missing else None)
        if card_h>self.px(195):
            line(0,4,_('All 1.09 patches available','Патчи 1.09 доступны') if info and info[1]==PATCH_VERSION
                 else _('Patch status  ·  Details →','Состояние патчей →'),
                 color=SUCCESS if info and info[1]==PATCH_VERSION else MUTED,
                 action=lambda:self.show('mods'))
        line(1,0,_('Resolution','Разрешение'),self.var('output_res','ini').get().replace('x',' × '))
        fps=dict(FPS_MODES).get(self.var('fps_mode','app').get(),('?',))[0]
        line(1,1,_('Frame rate','Частота кадров'),fps)
        upscaler=dict(UPSCALERS).get(self.var('upscaler','ini').get(),('?',))[0].split(' (')[0]
        line(1,2,_('Upscaling','Апскейлинг'),upscaler,action=lambda:self.show('graphics'))
        line(1,3,_('Display','Экран'),_('Fullscreen','Полный экран') if self.var('fullscreen','app').get()
             else _('Windowed','В окне'),action=lambda:self.show('display'))
        if card_h>self.px(195):
            gpu=self.gpu_text.lstrip('✓•✗ ').replace('Graphics card: ','')
            line(1,4,gpu[:41] + ('…' if len(gpu)>41 else ''),color=MUTED)
        line(2,0,_('Xbox controller connected','Геймпад Xbox подключен') if self.controller_connected()
             else _('No Xbox controller detected','Геймпад Xbox не найден'),
             color=SUCCESS if self.controller_connected() else MUTED)
        line(2,1,_('Insert or L3+R3 opens the guide','Insert или L3+R3 открывает меню'),color=MUTED)
        line(2,2,_('Mods & patches  →','Моды и патчи  →'),color=GOLD,action=lambda:self.show('mods'))
        line(2,3,_('Add to Steam  →','Добавить в Steam  →'),color=GOLD,action=self.add_to_steam)
        if card_h>self.px(195):
            line(2,4,_('Online co-op enabled','Кооператив включён') if self.var('online','app').get()
                 else _('Playing offline','Игра офлайн'),color=MUTED,action=lambda:self.show('online'))
        banner_y=card_y+card_h+self.px(12)
        if banner_y+self.px(55)<h:
            footer_height=h-self.px(10)-banner_y
            footer=self.footer_art(art,round(w-2*margin),round(footer_height))
            round_rect(c,margin,banner_y,w-margin,h-self.px(10),10*s,'#172027','#394751')
            if footer:
                c.create_image(margin,banner_y,image=footer,anchor='nw')
            round_rect(c,margin,banner_y,w-margin,h-self.px(10),10*s,'','#63707a')
            c.create_text(margin+self.px(19),banner_y+self.px(21),
                          text=_('THE HUNT CONTINUES','ОХОТА ПРОДОЛЖАЕТСЯ'),anchor='w',fill=GOLD,
                          font=('Segoe UI Semibold',9))
            c.create_text(margin+self.px(19),banner_y+self.px(43),
                          text=_('Return to Yharnam. Your saves stay with your game installation.',
                                 'Вернитесь в Ярнам. Сохранения остаются в установленной игре.'),
                          anchor='w',fill=TEXT,font=('Georgia',12))
            c.create_text(w-margin-self.px(20),banner_y+self.px(34),text='→',anchor='e',fill=GOLD,
                          font=('Segoe UI',19))

    def scaled_art(self, path, width):
        """The cover art scaled to `width`: Pillow (smooth) or Tk's integer subsampling."""
        try:
            from PIL import Image, ImageTk
            with Image.open(path) as image:
                height = round(image.height * width / image.width)
                return ImageTk.PhotoImage(image.convert('RGB').resize((width, height), Image.LANCZOS))
        except (ImportError, OSError):
            factor = max(1, round(self.banner_image.width() / max(width, 1)))
            return self.banner_image.subsample(factor) if factor > 1 else self.banner_image

    def build_graphics(self):
        ttk = self.ttk
        f = self.scrolled_page('graphics', _('Graphics', 'Графика'),
                               _('Stored in bbport.ini; the in-game menu (Insert or L3+R3) changes the same values.',
                                 'Хранится в bbport.ini; в игре меняется через меню (Insert или L3+R3).'))
        self.section(f, _('Upscaling', 'Апскейлинг'), top=4)
        self.row(f, _('Upscaler', 'Апскейлер'), self.choice(f, 'upscaler', 'ini', UPSCALERS),
                 _("Temporal upscaling with the game's own motion vectors. FSR 4 needs its assets (below) and "
                   'a GPU with INT8 dot products; otherwise the game falls back to FSR 3.1 by itself.',
                   'Временной апскейлинг с векторами движения игры. FSR 4 нужны ассеты (ниже) и GPU с INT8; '
                   'иначе игра сама переключится на FSR 3.1.'))
        self.row(f, _('Quality preset', 'Пресет'), self.choice(f, 'preset', 'ini', PRESETS),
                 _('Render scale per axis: Quality renders at 1/1.5 of the output size.',
                   'Масштаб рендера по каждой оси: Quality рисует в 1/1.5 размера вывода.'))
        self.row(f, _('Output resolution', 'Разрешение вывода'), self.choice(f, 'output_res', 'ini', OUTPUTS),
                 _('Choose a preset or type WxH (even dimensions, 256x144 through 7680x4320).',
                   'Что выдаёт апскейлер; интерфейс рисуется в этом же размере.'))
        self.row(f, _('HUD position (ultrawide)', 'Положение интерфейса (21:9)'),
                 self.choice(f, 'hud_position', 'ini', HUD_POSITIONS),
                 _('21:9 and 32:9 outputs: health, items and Blood Echoes at the screen edges, or in the '
                   'centred 16:9 area as on a 16:9 screen.',
                   'Для 21:9 и 32:9: здоровье, предметы и Отголоски крови у краёв экрана или в центральной '
                   'области 16:9, как на обычном экране.'))
        self.row(f, _('Live resolution changes', 'Смена разрешения на лету'), self.choice(f, 'live_resolution', 'ini', LIVE),
                 _('Off: outputs other than 1080p are set by a patch at start (fastest; changing them in the '
                   'game restarts it). On: change output and preset in the game without a restart, at a cost.',
                   'Выкл.: разрешения кроме 1080p задаются патчем при запуске (быстрее). Вкл.: менять в игре '
                   'без перезапуска, но медленнее.'))
        self.check(f, 'sharpen', 'ini', _('Sharpening (RCAS)', 'Резкость (RCAS)'))
        holder = ttk.Frame(f)
        ttk.Scale(holder, from_=0.0, to=2.0, variable=self.var('sharpness', 'ini'), length=300).pack(side='left')
        value = ttk.Label(holder, width=5)
        value.pack(side='left', padx=10)
        show = lambda *_a: value.configure(text=f'{self.vars["sharpness"].get():.2f}')
        self.vars['sharpness'].trace_add('write', show)
        show()
        self.row(f, _('Sharpness', 'Сила резкости'), holder)
        self.check(f, 'object_motion', 'ini', _('Object motion vectors', 'Векторы движения объектов'),
                   _('Less ghosting on characters, cloth and weapons; costs about 10% FPS.',
                     'Меньше гостинга на персонажах и одежде; стоит около 10% FPS.'))
        self.section(f, _('Frame generation', 'Генерация кадров'))
        self.row(f, _('Frame generation', 'Генерация кадров'),
                 self.choice(f, 'frame_gen', 'ini', FRAME_GENERATION),
                 _('Frames made between two of the game\'s: 60 FPS on screen becomes 120. Needs a display with '
                   'that refresh rate (120 Hz or more at 60 FPS); it pauses when the display cannot show the extra '
                   'frames. Adds a little input latency. Menus and movies are not generated. DLSS needs a GeForce '
                   'RTX 40 or newer, otherwise FSR 3.1 is used.',
                   'Кадры между двумя кадрами игры: 60 FPS на экране становятся 120. Нужен монитор с такой '
                   'частотой (120 Гц и выше при 60 FPS); иначе генерация приостанавливается. Немного добавляет '
                   'задержку ввода. Меню и ролики не генерируются. DLSS нужна GeForce RTX 40 или новее, иначе '
                   'используется FSR 3.1.'))
        self.row(f, _('Multiplier', 'Множитель'),
                 self.choice(f, 'frame_gen_multiplier', 'ini', FRAME_GEN_MULTIPLIERS),
                 _('FSR 3.1 makes one frame (2×); DLSS on a GeForce RTX 50 up to three (4×).',
                   'FSR 3.1 делает один кадр (2×); DLSS на GeForce RTX 50 — до трёх (4×).'))
        self.check(f, 'frame_gen_fit_display', 'ini', _('Fit to display refresh', 'Под частоту монитора'),
                   _('On: no more frames than the display shows (an uncapped game gets fewer generated frames, and '
                     'the GPU does less). Off: the multiplier as chosen.',
                     'Вкл.: не больше кадров, чем показывает монитор (при снятом ограничении FPS генерируется меньше, '
                     'видеокарта меньше нагружена). Выкл.: множитель как выбран.'))
        self.section(f, _('FSR 4 assets', 'Ассеты FSR 4'))
        self.fsr4_label = ttk.Label(f, text='', wraplength=self.px(640), justify='left')
        self.fsr4_label.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w')
        holder = ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(8, 0))
        self.fsr4_button = ttk.Button(holder, text=_('Download FSR 4 assets', 'Скачать ассеты FSR 4'),
                                      command=self.download_fsr4)
        self.fsr4_button.pack(side='left')
        self.fsr4_progress = ttk.Progressbar(holder, length=280, maximum=len(fsr4_files()))
        self.fsr4_progress.pack(side='left', padx=12)
        self.note(f, _("From FireBurn/Q2RTX on GitHub (built from AMD's MIT-licensed FidelityFX source), "
                       'about 45 MB, into the fsr4_shaders folder of the port.',
                       'С GitHub FireBurn/Q2RTX (собраны из MIT-исходников AMD FidelityFX), около 45 МБ, '
                       'в папку fsr4_shaders порта.'), top=6)
        self.section(f, _('Detail', 'Детализация'))
        self.row(f, _('Model detail (LOD)', 'Детализация моделей'), self.choice(f, 'model_lod', 'ini', LODS),
                 _('A game patch (game version 1.09).', 'Патч игры (версия 1.09).'))
        self.row(f, _('Video memory pressure target', 'Порог видеопамяти'),
                 self.choice(f, 'vram_budget_mb', 'app', VRAM_BUDGETS),
                 _('Starts clearing old textures and buffers before this amount. This is not a hard cap; '
                   'lower values can cause stutter. Restart the game to apply.',
                   'Начинает освобождать старые текстуры и буферы до этого объёма. Это не жёсткий предел; '
                   'низкие значения могут вызвать рывки. Перезапустите игру.'))
        self.check(f, 'show_fps', 'ini', _('Show the FPS counter', 'Показывать FPS'))

    def build_display(self):
        ttk = self.ttk
        f = self.scrolled_page('display', _('Display & FPS', 'Экран и FPS'),
                               _('Applied when the game starts.', 'Применяется при запуске игры.'))
        self.version_warning(f)
        self.section(f, _('Frame rate', 'Частота кадров'), top=4)
        self.row(f, _('Frame rate', 'Режим'), self.choice(f, 'fps_mode', 'app', FPS_MODES),
                 _("Community patches for game version 1.09. Unlocked makes the game use the real frame time. "
                   'Other game versions always run at 30 FPS (the patches would corrupt them).',
                   'Патчи сообщества для версии 1.09. «Без ограничения» — игра использует реальное время кадра. '
                   'Другие версии всегда работают в 30 FPS.'))
        self.row(f, _('Frame cap (unlocked mode)', 'Ограничение FPS (режим без ограничения)'),
                 self.choice(f, 'frame_cap', 'app', FRAME_CAPS),
                 _("Above about 120 FPS the game's movement timing breaks (running and rolling get slower, "
                   'physics and animations can glitch): higher caps are at your own risk.',
                   'Выше ~120 FPS ломается тайминг движения игры (бег и перекаты замедляются, возможны '
                   'сбои физики и анимаций): более высокие значения — на ваш риск.'))
        self.row(f, _('Frames ahead of the GPU', 'Кадров впереди GPU'), self.choice(f, 'frames_ahead', 'app', FRAMES_AHEAD),
                 _('1 keeps frame pacing even; more can raise FPS when the graphics card is the limit.',
                   '1 — ровная подача кадров; больше может поднять FPS, если упирается в видеокарту.'))
        self.section(f, _('Window', 'Окно'))
        self.check(f, 'fullscreen', 'app', _('Fullscreen', 'Полноэкранный режим'))
        self.row(f, _('Fullscreen type', 'Тип полного экрана'), self.choice(f, 'fullscreen_mode', 'app', FULLSCREEN_MODES),
                 _('Exclusive switches the display to its own mode: notifications and other windows do not show over the game.',
                   'Эксклюзивный переключает монитор в собственный режим: уведомления и другие окна не видны поверх игры.'))
        self.row(f, _('Monitor', 'Монитор'), self.choice(f, 'display', 'app', DISPLAYS),
                 _('The monitor the game window and fullscreen go to, in the order the game log lists them.',
                   'Монитор для окна и полного экрана игры, в порядке списка в журнале игры.'))
        self.row(f, _('Presentation', 'Режим показа кадров'), self.choice(f, 'present_mode', 'app', PRESENT_MODES))
        self.check(f, 'hdr', 'app', _('Allow HDR output', 'Разрешить HDR'),
                   _('When HDR is on in Windows and the display supports it.',
                     'Если HDR включён в Windows и монитор его поддерживает.'))

    def build_game(self):
        f = self.scrolled_page('game', _('Game & effects', 'Игра и эффекты'),
                               _('Your game dump, saves and the game patches.', 'Дамп игры, сохранения и патчи игры.'))
        self.section(f, _('Game', 'Игра'), top=4)
        self.folder(f, 'game_dir', _('Game folder', 'Папка игры'),
                    _('Choose the folder with eboot.bin', 'Выберите папку с eboot.bin'),
                    _('Your own dump of CUSA03173 (eboot.bin, sce_module, sce_sys, dvdroot_ps4); version 1.09 '
                      'for the community patches.', 'Ваш дамп CUSA03173 (eboot.bin, sce_module, sce_sys, '
                      'dvdroot_ps4); версия 1.09 для патчей сообщества.'), on_change=self.game_changed)
        self.folder(f, 'user_dir', _('Saves folder', 'Папка сохранений'),
                    _('Choose the saves folder', 'Выберите папку сохранений'),
                    _('Empty: {} (shader caches are kept there too).',
                      'Пусто: {} (там же кэш шейдеров).').format(DATA_DIR / 'user'), on_change=self.refresh_status)
        self.folder(f, 'dlc_dir', _('DLC folder'), _('Choose the DLC folder'),
                    _('Your dumped add-ons, one folder each named like SPEXPANSIONDLC03 (The Old Hunters). '
                      'Only needed with a base-game dump; the Game of the Year edition (CUSA03173) has it.'))
        self.row(f, _('Game language', 'Язык игры'), self.choice(f, 'language', 'app', LANGUAGES))
        self.row(f, _('Voice language', 'Язык озвучки'), self.choice(f, 'voice_language', 'app', VOICE_LANGUAGES),
                 _('As in the game uses the original voice setting; other choices use available tracks.',
                   '«Как в игре» оставляет исходную озвучку; другие варианты берут доступные дорожки.'))
        self.row(f, _('Speakers', 'Динамики'), self.choice(f, 'audio_channels', 'app', AUDIO_CHANNELS),
                 _('Stereo when dialogue is missing or hits sound muffled: some TVs and headsets take 7.1 '
                   'and play only the front pair.',
                   'Стерео, если не слышно диалогов или удары звучат глухо: некоторые ТВ и гарнитуры '
                   'принимают 7.1 и играют только два передних канала.'))
        self.row(f, _('Player name', 'Имя игрока'), self.ttk.Entry(f, textvariable=self.var('player_name', 'app'), width=30),
                 _('Where the game shows the PSN name; empty: the default.', 'Где игра показывает имя PSN; пусто — по умолчанию.'))
        self.section(f, _('Effects', 'Эффекты'))
        self.version_warning(f)
        for key, title, _on in EFFECTS:
            self.check(f, key, 'ini', _(*title))
        self.section(f, _('Extras', 'Дополнительно'))
        for key, title, _on in EXTRAS:
            self.check(f, key, 'ini', _(*title))
        self.note(f, _('Effects and extras are game patches for version 1.09, applied at start.',
                       'Эффекты и дополнения — патчи игры для версии 1.09, применяются при запуске.'))

    def build_cheats(self):
        f = self.scrolled_page('cheats', _('Cheats', 'Читы'),
                               _('Game patches for version 1.09, applied at start. Leave them off for a normal '
                                 'play-through.', 'Патчи игры для версии 1.09, применяются при запуске. Для обычного '
                                 'прохождения оставьте их выключенными.'))
        self.version_warning(f)
        self.section(f, _('Cheats', 'Читы'), top=4)
        for key, title, _on in CHEATS:
            self.check(f, key, 'ini', _(*title))
        self.section(f, _('Gameplay tweaks', 'Изменения игрового процесса'))
        for key, title, _on in TWEAKS:
            self.check(f, key, 'ini', _(*title))
        # Enemy control and the free camera share their buttons: one at a time.
        control, camera = self.var('cheat_enemy_control', 'ini'), self.var('debug_camera', 'ini')
        control.trace_add('write', lambda *_a: control.get() and camera.set(False))
        camera.trace_add('write', lambda *_a: camera.get() and control.set(False))
        self.note(f, _("Cheats, the free camera and the debug menu are off when playing online with The "
                       "Hunter's Dream; seamless co-op keeps them.", 'Читы, свободная камера и меню отладки '
                       "отключены при игре онлайн через The Hunter's Dream; в бесшовном кооперативе они остаются."))

    def build_online(self):
        ttk = self.ttk
        f = self.scrolled_page('online', _('Online co-op'),
                               _("Messages, bloodstains and ghosts come from The Hunter's Dream; bells and summons "
                                 'go through a shadNet server.'))
        self.check(f, 'online', 'app', _('Play online'),
                   _('Off: the game stays offline, as before.'))
        self.note(f, _('Heads-up: the public shadPS4 server (srv.shadps4.net) currently signs this port out '
                       '("doesn\'t accept your shadPS4 build"), since its 14 October 2026 update. To play together, '
                       'use seamless co-op on a server one of you runs, or a private shadNet server.',
                       'Внимание: публичный сервер shadPS4 (srv.shadps4.net) сейчас отключает этот порт '
                       '("doesn\'t accept your shadPS4 build") после обновления 14 октября 2026. Для совместной игры '
                       'используйте бесшовный кооператив на сервере одного из вас или частный сервер shadNet.'))
        self.var('online', 'app').trace_add('write', lambda *_a: self.refresh_status())
        if HUNTERS_DREAM_POOL:
            self.build_hunters_dream(f)
        self.section(f, _('Game server (messages, bloodstains, ghosts)'))
        community = self.var('online_community', 'app')
        self.row(f, _('Game server'), ttk.Entry(f, textvariable=community, width=40),
                 _('The Hunter\'s Dream by default. Only the name is needed; the game picks the ports.'))
        self.section(f, _('Co-op server (bells and summons)'))
        server = self.var('online_server', 'app')
        self.row(f, _('Server address'), ttk.Entry(f, textvariable=server, width=40),
                 _('srv.shadps4.net:31313 is the public shadPS4 server. For a private server, enter '
                   'host:port from the person who runs it.'))
        self.row(f, _('WebAPI address'), ttk.Entry(f, textvariable=self.var('online_webapi', 'app'), width=40),
                 _('Of the shadNet server, not the game server. Leave empty: the server address with '
                   'port 31315 is used.'))
        self.section(f, _('Account'))
        self.row(f, '', ttk.Button(f, text=_('Create shadNet account (no Discord)'),
                                   command=lambda: webbrowser.open('https://shadps4.net/shadnet/register/')))
        self.note(f, _('The Hunter\'s Dream website account is separate and optional for gameplay.'))
        self.row(f, _('Online ID (NPID)'), ttk.Entry(f, textvariable=self.var('online_npid', 'app'), width=40),
                 _('Your shadNet account name, not your email. Register one at shadps4.net/shadnet/register/, or ask the '
                   'owner of a private server.'))
        self.row(f, _('Password'), ttk.Entry(f, textvariable=self.var('online_password', 'app'), width=40, show='•'),
                 _('Saved on this PC with the other launcher settings.'))
        self.section(f, _('Connection'))
        self.check(f, 'online_upnp', 'app', _('UPnP (open a port on the router automatically)'),
                   _('Turn it off on Tailscale or another VPN.'))
        self.note(f, _('Test connection checks server reachability, not account sign-in.'))
        result = ttk.Label(f, text='', style='Muted.TLabel', justify='left')

        def check():
            values = {key: self.var(key, 'app').get()
                      for key in ('online_server', 'online_webapi', 'online_community')}
            address = values['online_server'].strip() or APP_DEFAULTS['online_server']
            webapi, game = webapi_setting(values), community_address(values['online_community'])
            online_on = self.var('online', 'app').get()
            result.configure(text=_('Checking…'))

            def work():
                text = server_check(address, webapi, game)
                if values['online_webapi'].strip() and values['online_webapi'].strip() != webapi:
                    text += '\n' + _('The WebAPI box holds the game server address; it is ignored.')
                if not online_on:
                    text += '\n' + _('"Play online" is off: the game starts offline.')
                self.ui_calls.put(lambda: result.configure(text=text))
            threading.Thread(target=work, daemon=True).start()
        self.row(f, '', ttk.Button(f, text=_('Test connection'), command=check))
        result.grid(row=self.next_row(f), column=1, sticky='w', pady=(6, 0))
        self.note(f, _('Everyone in a session needs the same game version (1.09) and the same server. '
                       'Cheats change the game for the other players too: turn them off when you play together.'))

    def build_hunters_dream(self, f):
        """The co-op pool choice and The Hunter's Dream's Discord sign-in (BB_HUNTERS_DREAM=0 hides them)."""
        ttk = self.ttk
        self.section(f, _("The Hunter's Dream (test build)"))
        if thd_refused():
            self.note(f, _(*THD_REFUSED_NOTE))
        self.row(f, _('Co-op pool'), self.choice(f, 'online_pool', 'app', ONLINE_POOLS),
                 _("The Hunter's Dream signs in with Discord below; the shadNet server uses the account "
                   'fields further down.'))
        thd_id, thd_token = self.var('thd_online_id', 'app'), self.var('thd_token', 'app')
        account = ttk.Label(f, text='', justify='left')

        def show_account():
            name = thd_id.get() if thd_token.get() else ''
            account.configure(text=_('Signed in as {}').format(name) if name else _('Not signed in'))
        show_account()
        self.row(f, _('Account'), account)
        signing = ttk.Label(f, text='', style='Muted.TLabel', justify='left', wraplength=self.px(560))
        code_label = ttk.Label(f, text='', font=('Segoe UI', 20, 'bold'))

        def sign_in():
            signing.configure(text=_('Starting the sign-in…'))
            code_label.configure(text='')
            game = self.var('online_community', 'app').get()

            def on_code(page, code):
                text = _('On the page that opened, sign in with Discord and approve this code (not in its "code from the game" box):')
                self.ui_calls.put(lambda: (signing.configure(text=text), code_label.configure(text=code)))
                webbrowser.open(page)

            def work():
                try:
                    name, token = thd_sign_in(game, on_code)
                except (OSError, ValueError) as error:
                    self.ui_calls.put(lambda e=error: (signing.configure(text=_('Sign-in failed: {}').format(connection_error_text(e))),
                                                       code_label.configure(text='')))
                    return
                self.ui_calls.put(lambda: (thd_id.set(name), thd_token.set(token), signing.configure(text=''),
                                           code_label.configure(text=''), show_account(), self.collect()))
            threading.Thread(target=work, daemon=True).start()
        self.row(f, '', ttk.Button(f, text=_('Sign in with Discord'), command=sign_in))
        signing.grid(row=self.next_row(f), column=1, sticky='w', pady=(6, 0))
        code_label.grid(row=self.next_row(f), column=1, sticky='w', pady=(4, 0))
        self.note(f, _("The connection to The Hunter's Dream is Awaken's port of bbhost's network code; "
                       "bbhost and The Hunter's Dream are by droogie. Seamless co-op does not use it."))

    def build_mods(self):
        f = self.scrolled_page('mods', _('Mods & patches', 'Моды и патчи'),
                               _('The game files are never changed: mods are layered over them at start.',
                                 'Файлы игры не меняются: моды накладываются при запуске.'))
        self.section(f, _('Mods', 'Моды'), top=4)
        self.check(f, 'mods_enabled', 'app', _('Load mods', 'Загружать моды'),
                   _('Loose-file mods, each in its own folder (with dvdroot_ps4, or chr\\, parts\\ … directly).',
                     'Моды из файлов, каждый в своей папке (с dvdroot_ps4 или chr\\, parts\\ … напрямую).'))
        self.folder(f, 'mods_dir', _('Mods folder', 'Папка модов'), _('Choose the mods folder', 'Выберите папку модов'),
                    _('Empty: {}', 'Пусто: {}').format(DATA_DIR / 'mods'), on_change=self.refresh_lists)
        self.mods_frame = self.ttk.Frame(f)
        self.mods_frame.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=(8, 0))
        self.section(f, _('PC gameplay enhancements'))
        for key, title, _on in ENHANCEMENTS:
            self.check(f, key, 'ini', _(*title))
        self.note(f, _('Made from your own game files at launch. Restart to apply; Load mods is not required.'))
        self.note(f, _('Boss rematches: a defeated boss\'s own lamp offers a rematch for one Insight. Tested in '
                       'play on the Cleric Beast; the other 18 are new.', 'Повторные бои: фонарь побеждённого босса '
                       'предлагает бой заново за одно озарение. В игре проверен Чудовище-священник; остальные 18 новые.'))
        self.section(f, _('Third-party patches', 'Сторонние патчи'))
        self.folder(f, 'patches_dir', _('Patches folder', 'Папка патчей'), _('Choose the patches folder', 'Выберите папку патчей'),
                    _('shadPS4/GoldHEN XML patch files for version 1.09. Empty: {}',
                      'XML-патчи shadPS4/GoldHEN для версии 1.09. Пусто: {}').format(DATA_DIR / 'patches'),
                    on_change=self.refresh_lists)
        self.patches_frame = self.ttk.Frame(f)
        self.patches_frame.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=(8, 0))
        self.ttk.Button(f, text=_('Refresh', 'Обновить'), command=self.refresh_lists).grid(
            row=self.next_row(f), column=0, sticky='w', pady=(14, 0))

    def build_advanced(self):
        ttk = self.ttk
        f = self.scrolled_page('advanced', _('Advanced', 'Дополнительно'),
                               _('Launcher options, performance switches and diagnostics.',
                                 'Настройки лаунчера, производительность и диагностика.'))
        self.section(f, _('Launcher', 'Лаунчер'), top=4)
        self.row(f, _('Launcher language', 'Язык лаунчера'), self.choice(f, 'ui_language', 'app', UI_LANGUAGES),
                 _('Applies when the launcher opens again.', 'Применится при следующем открытии лаунчера.'))
        self.check(f, 'close_on_play', 'app', _('Close the launcher when the game starts', 'Закрывать лаунчер при запуске игры'))
        self.check(f, 'check_updates', 'app', _('Check for updates when the launcher opens',
                                                'Проверять обновления при открытии лаунчера'))
        holder = ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(10, 0))
        ttk.Button(holder, text=_('Desktop shortcut', 'Ярлык на рабочем столе'), command=self.shortcut).pack(side='left')
        ttk.Button(holder, text=_('Add to Steam'), command=self.add_to_steam).pack(side='left', padx=(6, 0))
        ttk.Button(holder, text=_('Port folder', 'Папка порта'), command=lambda: self.open_path(DATA_DIR)).pack(side='left', padx=6)
        ttk.Button(holder, text='bbport.ini', command=lambda: self.open_path(ini_path())).pack(side='left')
        holder = ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(10, 0))
        ttk.Button(holder, text=_('Check for updates', 'Проверить обновления'),
                   command=lambda: threading.Thread(target=self.check_update, args=(True,), daemon=True).start()
                   ).pack(side='left')
        ttk.Label(holder, text=f'v{VERSION}', style='Muted.TLabel').pack(side='left', padx=10)
        holder = ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(10, 0))
        ttk.Button(holder, text=_('Clear shader cache', 'Очистить кэш шейдеров'), command=self.clear_cache).pack(side='left')
        ttk.Label(holder, text=_('If the game only shows a black screen, this usually helps.',
                                 'Если игра показывает только чёрный экран, обычно это помогает.'),
                  style='Muted.TLabel').pack(side='left', padx=10)
        self.section(f, _('Performance', 'Производительность'))
        self.row(f, _('Two-stage GPU pipeline', 'Двухстадийный конвейер GPU'), self.choice(f, 'draw_pipe', 'app', DRAW_PIPE),
                 _('20–30% faster; switch it off if the game is unstable.', 'Быстрее на 20–30%; при нестабильности выключите.'))
        self.row(f, _('GPU readbacks', 'Чтение данных GPU'), self.choice(f, 'readbacks', 'app', READBACKS),
                 _('How exactly data the GPU writes is copied back for the game.',
                   'Насколько точно данные, записанные GPU, возвращаются игре.'))
        self.section(f, _('Diagnostics', 'Для разработчика'))
        self.check(f, 'frame_stats', 'app', _('Frame statistics in the log (every 5 s)', 'Статистика кадров в журнале (раз в 5 с)'))
        self.check(f, 'gpu_profile', 'app', _('GPU time per pass in the log', 'Профиль GPU в журнале'))
        self.check(f, 'vk_validation', 'app', _('Vulkan validation layers (needs the Vulkan SDK; much slower)',
                                                'Слои валидации Vulkan (нужен Vulkan SDK; сильно замедляет)'))
        self.row(f, _('Extra variables', 'Доп. переменные'), ttk.Entry(f, textvariable=self.var('extra_env', 'app'), width=58),
                 _('NAME=value pairs separated by spaces (README lists them).', 'Пары ИМЯ=значение через пробел (список в README).'))

    def build_log(self):
        tk, ttk = self.tk, self.ttk
        page = ttk.Frame(self.content, padding=(20, 14, 20, 8))
        self.pages['log'] = page
        top = ttk.Frame(page)
        top.pack(fill='x', pady=(0, 8))
        ttk.Label(top, text=_('Log', 'Журнал'), font=('Georgia', 20)).pack(side='left')
        ttk.Button(top, text=_('Copy', 'Копировать'), command=self.copy_log).pack(side='right')
        ttk.Button(top, text=_('Clear', 'Очистить'), command=lambda: self.set_log('')).pack(side='right', padx=6)
        self.log = tk.Text(page, wrap='none', bg='#0a0908', fg='#cfc6b8', insertbackground=TEXT, relief='flat',
                           font=('Consolas', 9), padx=8, pady=6, state='disabled', highlightthickness=0)
        bar = ttk.Scrollbar(page, command=self.log.yview)
        self.log.configure(yscrollcommand=bar.set)
        bar.pack(side='right', fill='y')
        self.log.pack(fill='both', expand=True)

    def version_warning(self, parent):
        """A banner shown while the selected game is not version 1.09 (refresh_status)."""
        label = self.ttk.Label(parent, style='Warning.TLabel', wraplength=self.px(640), justify='left', text=_(
            'Your game is version {}: these options are patches for 1.09 and are not applied; the game runs '
            'at 30 FPS. Update the dump to 1.09 to use them.',
            'Ваша игра версии {}: эти настройки — патчи для 1.09 и не применяются; игра работает в 30 FPS. '
            'Обновите дамп до 1.09, чтобы их использовать.'))
        label.grid(row=self.next_row(parent), column=0, columnspan=2, sticky='we', pady=(6, 4))
        label.template = label.cget('text')
        self.warnings = getattr(self, 'warnings', []) + [label]

    # ---- state -------------------------------------------------------------------------------
    def game_changed(self):
        self.banner_source = None
        self.draw_banner()
        self.set_icon()
        self.refresh_status()

    def refresh_status(self):
        info = game_info(self.var('game_dir', 'app').get())
        if not info:
            game = _('✗ No eboot.bin in the game folder (Game & effects)', '✗ В папке игры нет eboot.bin («Игра и эффекты»)')
        elif info[1] == PATCH_VERSION:
            game = _('✓ Game version {}: every patch available', '✓ Версия игры {}: доступны все патчи').format(info[1])
        else:
            game = _('⚠ Game version {}: runs at 30 FPS without the community patches (they are for 1.09)',
                     '⚠ Версия игры {}: 30 FPS без патчей сообщества (они для 1.09)').format(info[1])
        user = Path(self.var('user_dir', 'app').get() or DATA_DIR / 'user')
        saves = list((user / 'savedata').glob('*/*/SPRJ*')) if (user / 'savedata').is_dir() else []
        save = (_('✓ Saves found', '✓ Сохранения найдены') if saves
                else _('• No saves yet: the game creates them when you play',
                       '• Сохранений пока нет: игра создаст их после запуска'))
        missing = fsr4_missing()
        fsr4 = (_('✓ FSR 4 assets installed', '✓ Ассеты FSR 4 установлены') if not missing else
                _('• FSR 4 assets missing (Graphics); FSR 3.1 is used meanwhile',
                  '• Нет ассетов FSR 4 («Графика»); пока используется FSR 3.1'))
        self.check_texts.update((('game', game), ('saves', save), ('gpu', self.gpu_text), ('fsr4', fsr4)))
        for label in getattr(self, 'warnings', []):
            if info and info[1] != PATCH_VERSION:
                label.configure(text=label.template.format(info[1]))
                label.grid()
            else:
                label.grid_remove()
        if not self.process:
            self.set_play_enabled(bool(info))
            self.status.configure(text=self.summary() if info else _('Choose the game folder first.',
                                                                     'Сначала выберите папку игры.'), fg=MUTED)

    def set_play_enabled(self, enabled):
        self.play_enabled = enabled
        if hasattr(self, 'dashboard'):
            self.draw_dashboard()

    def summary(self):
        fps = dict(FPS_MODES).get(self.var('fps_mode', 'app').get(), ('?',))
        info = game_info(self.var('game_dir', 'app').get())
        if info and info[1] != PATCH_VERSION:
            fps = ('30 FPS',)
        upscaler = dict(UPSCALERS).get(self.var('upscaler', 'ini').get(), ('?',))[0].split(' (')[0]
        output = self.var('output_res', 'ini').get().replace('x', ' × ')
        online = _('Online') if self.var('online', 'app').get() else _('Offline')
        return f'{_(*fps)}   ·   {upscaler}   ·   {output}   ·   {online}'

    def detect_gpu(self):
        exe = PORT_DIR / 'bin' / 'bb-gpu-capabilities.exe'
        if not exe.is_file():
            exe = PORT_DIR / 'out' / 'bb-gpu-capabilities.exe'
        env = dict(os.environ)
        if not (exe.parent / 'SDL3.dll').is_file():
            clang64 = Path(os.environ.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
            env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'
        text = _('• Graphics card: not checked', '• Видеокарта: не проверена')
        try:
            result = subprocess.run([str(exe), '--live-resolution'], capture_output=True, text=True, timeout=30,
                                    env=env, creationflags=NO_WINDOW)
            names = [line[5:].split(':')[0] for line in result.stderr.splitlines() if line.startswith('GPU: ')]
            if names:
                text = _('✓ Graphics card: {}', '✓ Видеокарта: {}').format(names[0])
            elif result.returncode:
                text = _('✗ No Vulkan 1.3 graphics card found (update the driver)',
                         '✗ Не найдена видеокарта с Vulkan 1.3 (обновите драйвер)')
        except (OSError, subprocess.TimeoutExpired):
            pass
        self.gpu_text = text
        self.ui_calls.put(self.refresh_status)

    def refresh_fsr4(self):
        total, missing = len(fsr4_files()), len(fsr4_missing())
        self.fsr4_progress.configure(value=total - missing)
        self.fsr4_label.configure(
            text=_('Installed: {} files in {}.', 'Установлены: {} файлов в {}.').format(total, PORT_DIR / 'fsr4_shaders')
            if not missing else _('{} of {} files missing in {}.', 'Нет {} из {} файлов в {}.').format(
                missing, total, PORT_DIR / 'fsr4_shaders'))
        self.fsr4_button.configure(state='normal' if missing and not self.downloading else 'disabled')

    def download_fsr4(self):
        self.downloading = True
        self.fsr4_button.configure(state='disabled')
        folder = PORT_DIR / 'fsr4_shaders'

        def work():
            error = None
            try:
                folder.mkdir(parents=True, exist_ok=True)
                for name in fsr4_missing():
                    part = folder / (name + '.part')
                    with urllib.request.urlopen(f'{FSR4_BASE}/{name}', timeout=60) as response:
                        part.write_bytes(response.read())
                    part.replace(folder / name)
                    self.ui_calls.put(self.refresh_fsr4)
            except OSError as failure:
                error = failure

            def done():
                self.downloading = False
                self.refresh_fsr4()
                self.refresh_status()
                if error:
                    self.messagebox.showerror('FSR 4', _('Download failed: {}', 'Не удалось скачать: {}').format(error))
            self.ui_calls.put(done)
        threading.Thread(target=work, daemon=True).start()

    def refresh_lists(self):
        if not hasattr(self, 'patches_frame'):
            return
        tk, ttk = self.tk, self.ttk
        from mods import discover, unrecognized
        from patches import external_patches
        for holder in (self.mods_frame, self.patches_frame):
            for widget in holder.winfo_children():
                widget.destroy()
        root = Path(self.var('mods_dir', 'app').get() or DATA_DIR / 'mods')
        mod_config = DATA_DIR / 'mods.json'
        profile = load_json(mod_config, {})
        mtime = mod_config.stat().st_mtime_ns if mod_config.is_file() else None
        externally_changed = self.mod_profile_mtime is not None and mtime != self.mod_profile_mtime
        if externally_changed:
            self.mod_dirty = False
            self.mod_order = []
        self.mod_profile_mtime = mtime
        available = discover(root)
        if set(self.mod_order) != set(available):
            known = [n for n in profile.get('order', []) if n in available]
            self.mod_order = known + [n for n in available if n not in known]
        old = {} if externally_changed else {n: v.get() for n, v in self.mod_vars.items()}
        self.mod_vars = {}
        if not available:
            ttk.Label(self.mods_frame, text=_('No mods in {} yet.', 'В {} пока нет модов.').format(root),
                      style='Muted.TLabel').pack(anchor='w')
        for index, name in enumerate(self.mod_order):
            line = ttk.Frame(self.mods_frame)
            line.pack(fill='x', pady=1)
            var = tk.BooleanVar(value=old.get(name, name not in profile.get('disabled', [])))
            var.trace_add('write', lambda *_a: setattr(self,'mod_dirty',True))
            self.mod_vars[name] = var
            ttk.Button(line, text='▲', width=3, command=lambda i=index: self.move_mod(i, -1)).pack(side='left')
            ttk.Button(line, text='▼', width=3, command=lambda i=index: self.move_mod(i, 1)).pack(side='left', padx=(2, 10))
            ttk.Checkbutton(line, text=f'{index + 1}.  {name}', variable=var).pack(side='left')
        if len(self.mod_order) > 1:
            ttk.Label(self.mods_frame, text=_('Lower in the list loads later and wins conflicts.',
                                              'Ниже в списке — загружается позже и перекрывает.'),
                      style='Muted.TLabel').pack(anchor='w', pady=(4, 0))
        # Folders that are not mods are named, with why, instead of left out silently.
        for name, problem in unrecognized(root):
            ttk.Label(self.mods_frame, text=_('Not loaded: {}: {}', 'Не загружен: {}: {}').format(name, problem),
                      style='Muted.TLabel', wraplength=self.px(620), justify='left').pack(anchor='w', pady=(4, 0))
        chosen = load_json(DATA_DIR / 'patches.json', {})
        found = external_patches(Path(self.var('patches_dir', 'app').get() or DATA_DIR / 'patches'), PATCH_VERSION)
        old = {n: v.get() for n, v in self.patch_vars.items()}
        self.patch_vars = {}
        if not found:
            ttk.Label(self.patches_frame, text=_('No patch files for version 1.09 yet.', 'Пока нет патчей для версии 1.09.'),
                      style='Muted.TLabel').pack(anchor='w')
        for key, _path, meta in found:
            on = key in chosen.get('enabled', []) or (key not in chosen.get('disabled', []) and
                                                      meta.get('isEnabled', 'false').lower() == 'true')
            var = tk.BooleanVar(value=old.get(key, on))
            self.patch_vars[key] = var
            author = meta.get('Author')
            ttk.Checkbutton(self.patches_frame, text=key + (f'  —  {author}' if author else ''), variable=var).pack(anchor='w')

    def move_mod(self, index, step):
        other = index + step
        if 0 <= other < len(self.mod_order):
            self.mod_order[index], self.mod_order[other] = self.mod_order[other], self.mod_order[index]
            self.mod_dirty = True
            self.refresh_lists()

    @staticmethod
    def ini_text(key, value):
        return ('1' if value else '0') if key in INI_FLAGS else \
            f'{float(value):.2f}' if key == 'sharpness' else str(value)

    def sync_ini(self):
        """Takes what the in-game menu wrote to bbport.ini while the launcher was open, except where
        the launcher has an edit of its own that is not saved yet (else Play wrote the launcher's
        older values over the menu's)."""
        disk, lines = load_ini()
        for key, var in self.vars.items():
            if var.store != 'ini' or key not in disk:
                continue
            try:
                shown = self.ini_text(key, var.get())
            except (self.tk.TclError, ValueError):  # an unfinished number in a spinbox
                continue
            if shown == self.ini.get(key) and shown != disk[key]:
                var.set(disk[key] == '1' if key in INI_FLAGS else disk[key])
        self.ini, self.ini_lines = disk, lines

    def collect(self):
        """Writes settings.json, bbport.ini, mods.json and patches.json."""
        self.sync_ini()
        for key, var in self.vars.items():
            try:
                value = var.get()
            except self.tk.TclError:  # an unfinished number in a spinbox
                value = APP_DEFAULTS.get(key, INI_DEFAULTS.get(key))
            if var.store == 'ini':
                self.ini[key] = self.ini_text(key, value)
            else:
                self.app[key] = value
        CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        CONFIG_FILE.write_text(json.dumps(self.app, indent=2, ensure_ascii=False), encoding='utf-8')
        save_ini({key: self.ini[key] for key in INI_DEFAULTS}, self.ini_lines)
        self.ini, self.ini_lines = load_ini()
        if self.mod_order:
            mod_config = DATA_DIR / 'mods.json'
            mtime = mod_config.stat().st_mtime_ns if mod_config.is_file() else None
            if mtime == self.mod_profile_mtime or self.mod_dirty:
                # An in-game prompt switch may update this while the launcher remains open.
                # Keep that newer choice unless this window itself changed the mod list.
                profile = load_json(mod_config, {})
                profile.update({'order': self.mod_order,
                                'disabled': [n for n, v in self.mod_vars.items() if not v.get()]})
                mod_config.write_text(json.dumps(profile, indent=2), encoding='utf-8')
                self.mod_profile_mtime = mod_config.stat().st_mtime_ns
                self.mod_dirty = False
        if self.patch_vars:
            chosen = load_json(DATA_DIR / 'patches.json', {})
            shown = set(self.patch_vars)
            enabled = {k for k in chosen.get('enabled', []) if k not in shown} | {k for k, v in self.patch_vars.items() if v.get()}
            disabled = {k for k in chosen.get('disabled', []) if k not in shown} | {k for k, v in self.patch_vars.items() if not v.get()}
            (DATA_DIR / 'patches.json').write_text(json.dumps(
                {'enabled': sorted(enabled), 'disabled': sorted(disabled)}, indent=2), encoding='utf-8')

    # ---- game process ------------------------------------------------------------------------
    def play(self):
        if self.process or self.repairing:
            return
        online, npid = self.var('online', 'app'), self.var('online_npid', 'app')
        if not online.get() and npid.get().strip():
            choice = self.messagebox.askyesnocancel(
                'Bloodborne', _('An online account is filled in, but "Play online" is off.\n\n'
                                'Yes: play online.   No: play offline.'))
            if choice is None:
                return
            online.set(bool(choice))
        refused = thd_refused()  # PLAY starts offline instead
        if (online.get() and HUNTERS_DREAM_POOL and self.var('online_pool', 'app').get() == 'huntersdream'
                and not self.var('thd_token', 'app').get() and not refused):
            self.messagebox.showwarning('Bloodborne', _("The Hunter's Dream: sign in with Discord on the "
                                                        'Online page first.'))
            self.show('online')
            return
        if online.get() and '@' in npid.get():
            self.messagebox.showwarning(
                'Bloodborne', _('The Online ID is the account name (NPID) you registered on the server, '
                                'not your email address.'))
            self.show('online')
            return
        self.collect()
        if not game_info(self.app['game_dir']):
            self.messagebox.showerror('Bloodborne', _('Choose the game folder with eboot.bin (CUSA03173).',
                                                      'Выберите папку игры с eboot.bin (CUSA03173).'))
            self.show('game')
            return
        if missing := missing_port_files():
            self.offer_repair(missing)
            return
        self.set_log('')
        if refused and online.get() and play_mode(self.app) == 'huntersdream':
            self.append(_(*THD_REFUSED_NOTE) + ' ' + _('Starting offline.', 'Запуск офлайн.') + '\n')
        try:
            self.process = subprocess.Popen(run_command(), cwd=PORT_DIR, env=game_environment(self.app),
                                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
        except OSError as error:
            self.append(_('Could not start: {}', 'Не удалось запустить: {}').format(error) + '\n')
            self.process = None
            return
        self.job = GameJob(self.process)
        threading.Thread(target=self.read_output, args=(self.process,), daemon=True).start()
        self.set_play_enabled(False)
        self.stop_button.configure(state='normal')
        self.status.configure(text=_('Preparing the game; it opens in its own window…',
                                     'Подготовка игры; она откроется в своём окне…'), fg=GOLD)
        if self.app.get('close_on_play'):
            self.root.after(5000, self.root.destroy)  # the game keeps running

    def read_output(self, process):
        """Helper thread: the game's output to the Log page and to <saves folder>/last_run.log."""
        log = None
        try:
            log = open_run_log(self.app.get('user_dir') or DATA_DIR / 'user')
        except OSError:
            pass
        warned = False
        for raw in iter(process.stdout.readline, b''):
            text = raw.decode('utf-8', errors='replace')
            self.output.put(text)
            note_game_output(text)
            if not warned and 'eboot.bin is not the 01.09 build' in text:
                warned = True
                self.ui_calls.put(lambda: self.messagebox.showwarning('Bloodborne', _(
                    "Your eboot.bin is not the clean 1.09 one (modified, or from another dump). The port "
                    "cannot patch it: the game runs at 30 FPS, the Game & effects options do nothing, and "
                    "some scenes (like the cutscene after Rom) can freeze.\n\nReplace eboot.bin with the one "
                    "from an unmodified 1.09 update.")))
            if log:
                log.write(text.replace('\r\n', '\n'))
        if log:
            log.close()
        self.output.put((process.wait(),))

    def drain_output(self):
        while not self.ui_calls.empty():
            self.ui_calls.get_nowait()()
        try:
            for _i in range(500):
                item = self.output.get_nowait()
                if isinstance(item, tuple):
                    self.append(_('\n— the game exited (code {}) —\n', '\n— игра завершилась (код {}) —\n').format(
                        exit_code_text(item[0])))
                    self.process = None
                    if self.job:
                        self.job.close()
                    self.sync_ini()  # the in-game menu's changes, shown at once
                    self.stop_button.configure(state='disabled')
                    self.refresh_status()
                    if item[0] and (missing := missing_port_files()):
                        self.offer_repair(missing)
                else:
                    if 'Entering original x86-64 code' in item:
                        self.status.configure(text=_('The game is running.', 'Игра запущена.'), fg=GOLD)
                    elif 'restarting through run.py' in item:
                        self.status.configure(text=_('Restarting with the new settings…', 'Перезапуск с новыми настройками…'), fg=GOLD)
                    self.append(item)
        except queue.Empty:
            pass
        self.root.after(100, self.drain_output)

    def stop(self):
        if self.job:
            self.job.terminate()

    def append(self, text):
        self.log.configure(state='normal')
        self.log.insert('end', text)
        lines = int(self.log.index('end-1c').split('.')[0])
        if lines > MAX_LOG_LINES:
            self.log.delete('1.0', f'{lines - MAX_LOG_LINES}.0')
        self.log.see('end')
        self.log.configure(state='disabled')

    def set_log(self, text):
        self.log.configure(state='normal')
        self.log.delete('1.0', 'end')
        self.log.insert('end', text)
        self.log.configure(state='disabled')

    def copy_log(self):
        self.root.clipboard_clear()
        self.root.clipboard_append(self.log.get('1.0', 'end'))

    def open_path(self, path, key=None):
        if key == 'user_dir' and not path:
            path = DATA_DIR / 'user'
        elif key == 'mods_dir' and not path:
            path = DATA_DIR / 'mods'
        elif key == 'patches_dir' and not path:
            path = DATA_DIR / 'patches'
        path = Path(path or DATA_DIR)
        if path.suffix == '.ini':
            self.collect()
        elif not path.exists():
            path.mkdir(parents=True, exist_ok=True)
        os.startfile(str(path))

    # ---- updates -------------------------------------------------------------------------------
    def check_update(self, manual=False):
        """Helper thread: asks GitHub for the newest release and offers it when it is newer."""
        try:
            version, url, page = latest_release()
        except (OSError, ValueError, KeyError) as failure:
            if manual:
                self.ui_calls.put(lambda error=failure: self.messagebox.showerror(
                    'Bloodborne', _('Could not check for updates: {}', 'Не удалось проверить обновления: {}').format(error)))
            return

        def show():
            if release_is_newer(version):
                self.offer_update(version, url, page)
            elif manual:
                self.messagebox.showinfo('Bloodborne', _('You have the latest version ({}).',
                                                         'У вас последняя версия ({}).').format(VERSION))
        self.ui_calls.put(show)

    def offer_update(self, version, url, page):
        if self.update_box:
            return
        tk, ttk = self.tk, self.ttk
        text = _('Version {} is available.', 'Доступна версия {}.').format(version)
        box = tk.Frame(self.side, bg=CARD, highlightthickness=1, highlightbackground=GOLD)
        self.update_label = tk.Label(box, text=text, bg=CARD, fg=GOLD, font=('Segoe UI', 10, 'bold'),
                                     wraplength=self.px(160), justify='left')
        self.update_label.pack(anchor='w', padx=10, pady=(8, 6))
        buttons = tk.Frame(box, bg=CARD)
        buttons.pack(fill='x', padx=10, pady=(0, 10))
        self.update_button = ttk.Button(buttons, text=_('Update', 'Обновить'),
                                        command=lambda: self.install_update(version, url, page))
        self.update_button.pack(fill='x')
        ttk.Button(buttons, text=_("What's new", 'Что нового'), command=lambda: webbrowser.open(page)).pack(
            fill='x', pady=(4, 0))
        box.pack(fill='x', padx=(22, 18), pady=(0, 14), before=self.side_note)
        self.update_box = box
        if not self.process:
            self.status.configure(text=text, fg=GOLD)

    def install_update(self, version, url, page):
        if self.process:
            self.messagebox.showinfo('Bloodborne', _('Close the game before updating.', 'Закройте игру перед обновлением.'))
            return
        if not FROZEN or not url:  # a source tree updates with git
            webbrowser.open(page)
            return
        if not self.messagebox.askyesno('Bloodborne', _(
                'Install version {} now? The launcher closes, installs it and opens again. Saves and settings are kept.',
                'Установить версию {} сейчас? Лаунчер закроется, установит её и откроется снова. Сохранения и '
                'настройки останутся.').format(version)):
            return
        self.update_button.configure(state='disabled')

        def progress(percent):
            self.update_label.configure(text=_('Downloading version {}… {}%', 'Загрузка версии {}… {}%').format(
                version, percent))

        def work():
            try:
                if not remove_update_folder():
                    raise OSError('an update is still being installed; try again in a minute')
                UPDATE_DIR.mkdir(parents=True, exist_ok=True)
                archive = UPDATE_DIR / 'update.zip'
                request = urllib.request.Request(url, headers={'User-Agent': 'bbport-launcher'})
                with urllib.request.urlopen(request, timeout=60) as response, open(archive, 'wb') as out:
                    total, done, shown = int(response.headers.get('Content-Length') or 0), 0, -1
                    while chunk := response.read(1 << 20):
                        out.write(chunk)
                        done += len(chunk)
                        percent = done * 100 // total if total else 0
                        if percent != shown:
                            shown = percent
                            self.ui_calls.put(lambda p=percent: progress(p))
                extract_update(archive, UPDATE_DIR / 'new')
                archive.unlink()
                new = next((p.parent for p in (UPDATE_DIR / 'new').rglob('Bloodborne.exe')), None)
                if not new:
                    raise OSError('Bloodborne.exe is missing from the download')
                # The new launcher copies itself over this installation once this one has closed.
                subprocess.Popen([str(new / 'Bloodborne.exe'), '--install-update', str(PORT_DIR), str(os.getpid())],
                                 cwd=str(new), stdin=subprocess.DEVNULL, creationflags=NO_WINDOW)
                self.ui_calls.put(self.root.destroy)
            except (OSError, zipfile.BadZipFile) as failure:
                def failed(error=failure):
                    self.update_button.configure(state='normal')
                    self.update_label.configure(text=_('Version {} is available.', 'Доступна версия {}.').format(version))
                    self.messagebox.showerror('Bloodborne', _('Update failed: {}', 'Не удалось обновить: {}').format(error))
                self.ui_calls.put(failed)
        threading.Thread(target=work, daemon=True).start()

    # ---- repair --------------------------------------------------------------------------------
    def offer_repair(self, missing):
        """A port folder without some of its files (#171): offers to put this version's back."""
        if self.repairing:
            return
        if not FROZEN or COOP_TEST:
            self.repair_failed(missing)
            return
        more = _(' and {} more', ' и ещё {}').format(len(missing) - 1) if len(missing) > 1 else ''
        if self.messagebox.askyesno('Bloodborne', _(
                'This copy of the port is incomplete: {} missing.\n\nRepair it? The launcher downloads version {} '
                'from GitHub and puts its files back. Saves, settings and mods are kept.',
                'Эта копия порта неполная: нет {}.\n\nВосстановить? Лаунчер скачает версию {} с GitHub и вернёт '
                'её файлы. Сохранения, настройки и моды останутся.').format(missing[0] + more, VERSION)):
            self.repair()

    def repair(self):
        if self.process:
            self.messagebox.showinfo('Bloodborne', _('Close the game before repairing.', 'Закройте игру перед восстановлением.'))
            return
        self.repairing = True

        def progress(percent):
            self.status.configure(text=_('Downloading version {}… {}%', 'Загрузка версии {}… {}%').format(
                VERSION, percent), fg=GOLD)

        def work():
            try:
                missing, failed = repair_port(lambda percent: self.ui_calls.put(lambda: progress(percent)))
            except OSError as failure:
                self.ui_calls.put(lambda error=failure: self.repair_done(missing_port_files(), [], error))
            else:
                self.ui_calls.put(lambda: self.repair_done(missing, failed))
        threading.Thread(target=work, daemon=True).start()

    def repair_done(self, missing, failed, error=None):
        self.repairing = False
        self.status.configure(text='', fg=MUTED)
        if error or missing:
            self.repair_failed(missing, error)
        elif failed:
            self.messagebox.showwarning('Bloodborne', _(
                'The port is repaired, but some files were in use and were not replaced: {}. Close the '
                'programs using them and repair again if the game does not start.',
                'Порт восстановлен, но некоторые файлы были заняты и не заменены: {}. Закройте программы, '
                'которые их используют, и повторите, если игра не запускается.').format(
                    ', '.join(map(str, failed[:3]))))
        else:
            self.messagebox.showinfo('Bloodborne', _('The port is repaired: the files of version {} are back.',
                                                     'Порт восстановлен: файлы версии {} на месте.').format(VERSION))

    def repair_failed(self, missing, error=None):
        """What run.py says about an incomplete copy, with the releases page opened."""
        text = ''
        if missing:
            more = _(' and {} more', ' и ещё {}').format(len(missing) - 1) if len(missing) > 1 else ''
            text = _('This copy of the port is incomplete: {} missing from {}. Download the port again from {} '
                     'and unpack the whole zip; copies from other sites often leave files out.',
                     'Эта копия порта неполная: в {1} нет {0}. Скачайте порт заново с {2} и распакуйте весь '
                     'архив; в копиях с других сайтов часто не хватает файлов.').format(
                         missing[0] + more, PORT_DIR, RELEASES_PAGE)
        if error:
            text = (_('Repair failed: {}', 'Не удалось восстановить: {}').format(error) + '\n\n' + text).strip()
        self.messagebox.showerror('Bloodborne', text)
        webbrowser.open(RELEASES_PAGE)

    def clear_cache(self):
        """Shader and pipeline caches; they are rebuilt while playing."""
        if self.process:
            return
        import shutil
        cache = Path(self.app['user_dir'] or DATA_DIR / 'user') / 'cache'
        shutil.rmtree(cache, ignore_errors=True)
        self.messagebox.showinfo('Bloodborne', _('Shader cache cleared. The next start stutters for a few minutes while '
                                                 'it is rebuilt.', 'Кэш шейдеров очищен. Следующий запуск несколько '
                                                 'минут будет подтормаживать, пока кэш собирается заново.'))

    def shortcut(self):
        """Bloodborne.lnk on the desktop."""
        if FROZEN:
            target, arguments, icon = sys.executable, '', f'{sys.executable},0'
        else:  # a source tree: the launcher script with the windowless Python
            pythonw = Path(sys.executable).with_name('pythonw.exe')
            target = str(pythonw if pythonw.exists() else sys.executable)
            arguments, icon = Path(__file__).resolve(), PORT_DIR / 'launcher' / 'bloodborne.ico'
        script = ('$s=(New-Object -ComObject WScript.Shell).CreateShortcut([Environment]::GetFolderPath("Desktop")'
                  '+"\\Bloodborne.lnk");'
                  f'$s.TargetPath="{target}";$s.WorkingDirectory="{PORT_DIR}";$s.IconLocation="{icon}";'
                  + (f"$s.Arguments='\"{arguments}\"';" if arguments else '') + '$s.Save()')
        result = subprocess.run(['powershell', '-NoProfile', '-Command', script], capture_output=True,
                                creationflags=NO_WINDOW)
        if result.returncode == 0:
            self.messagebox.showinfo('Bloodborne', _('Shortcut created on the desktop.', 'Ярлык создан на рабочем столе.'))
        else:
            self.messagebox.showerror('Bloodborne', result.stderr.decode(errors='replace')[:400])

    def add_to_steam(self):
        """Bloodborne in the Steam library (a non-Steam game that starts Play Bloodborne.exe)."""
        import bbport_steam
        steam = bbport_steam.steam_folder()
        configs = bbport_steam.user_configs(steam) if steam else []
        if not configs:
            self.messagebox.showerror('Bloodborne', _('Steam was not found on this PC (or nobody has signed in to it yet).'))
            return
        if FROZEN:
            exe, options = PORT_DIR / 'Play Bloodborne.exe', ''
        else:
            pythonw = Path(sys.executable).with_name('pythonw.exe')
            exe, options = (pythonw if pythonw.exists() else Path(sys.executable)), f'"{Path(__file__).resolve()}" --play'
        self.collect()  # Play Bloodborne.exe starts the game with the saved settings
        restart = False
        if bbport_steam.steam_running():
            if not self.messagebox.askyesno('Bloodborne', _('Steam has to close while the game is added; it starts '
                                                            'again afterwards. Close Steam now?')):
                return
            subprocess.Popen([str(steam / 'steam.exe'), '-shutdown'], creationflags=NO_WINDOW)
            for _i in range(60):
                time.sleep(0.5)
                if not bbport_steam.steam_running():
                    break
            else:
                self.messagebox.showerror('Bloodborne', _('Steam did not close. Close it yourself and try again.'))
                return
            time.sleep(1)  # Steam writes its files as it exits
            restart = True
        try:
            for config in configs:
                bbport_steam.add_shortcut(config / 'shortcuts.vdf', 'Bloodborne', str(exe), str(PORT_DIR),
                                          str(exe), options)
        except (OSError, ValueError) as error:
            self.messagebox.showerror('Bloodborne', _('Could not add the game to Steam: {}').format(error))
            return
        if restart:
            os.startfile('steam://open/games')
        self.messagebox.showinfo('Bloodborne', _('Bloodborne is in your Steam library. It starts with the settings '
                                                 'saved in this launcher.'))

    def close(self):
        try:
            self.collect()
        except Exception:  # never keep the window open over a settings problem
            pass
        self.root.destroy()


class GameJob:
    """A Windows job holding run.py and everything it starts: bb-probe.exe and the launches made
    by the in-game restart (no longer descendants of the first process)."""

    def __init__(self, process):
        kernel32 = ctypes.windll.kernel32
        kernel32.CreateJobObjectW.restype = ctypes.c_void_p
        self.handle = kernel32.CreateJobObjectW(None, None)
        if self.handle:
            kernel32.AssignProcessToJobObject(ctypes.c_void_p(self.handle), ctypes.c_void_p(int(process._handle)))

    def terminate(self):
        if self.handle:
            ctypes.windll.kernel32.TerminateJobObject(ctypes.c_void_p(self.handle), 1)

    def close(self):
        if self.handle:
            ctypes.windll.kernel32.CloseHandle(ctypes.c_void_p(self.handle))
            self.handle = None



RUN_LOGS_KEPT = 3


def open_run_log(log_dir):
    """<saves folder>/last_run.log for a new run. The previous runs' logs move to
    last_run.1.log, last_run.2.log ... (RUN_LOGS_KEPT in all): starting again after a crash
    no longer loses the log of the run that crashed."""
    log_dir = Path(log_dir)
    log_dir.mkdir(parents=True, exist_ok=True)
    names = ['last_run.log'] + [f'last_run.{n}.log' for n in range(1, RUN_LOGS_KEPT)]
    for older, newer in zip(reversed(names[1:]), reversed(names[:-1])):
        try:
            (log_dir / newer).replace(log_dir / older)
        except OSError:
            pass
    return open(log_dir / 'last_run.log', 'w', encoding='utf-8', buffering=1)


def play_without_window(settings):
    """--play: the game with the saved settings (shortcuts, Steam). Output goes to the console
    when there is one and to <saves folder>/last_run.log."""
    attach_stdio()
    start_quiet_update_check(settings)
    with open_run_log(settings.get('user_dir') or DATA_DIR / 'user') as log:
        process = subprocess.Popen(run_command(), cwd=PORT_DIR, env=game_environment(settings),
                                   stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
        for raw in iter(process.stdout.readline, b''):
            text = raw.decode('utf-8', errors='replace')
            log.write(text)
            note_game_output(text)
            try:
                sys.stdout.write(text)
            except (OSError, ValueError):
                pass
        return exit_status(process.wait())


def version_tuple(text):
    return tuple(int(number) for number in re.findall(r'\d+', text or ''))


def release_is_newer(tag):
    """Accept the next QML-series bridge or a newer numbered stable release."""
    if tag.startswith('qml-launcher-preview-'):
        try:
            return int(tag.removeprefix('qml-launcher-preview-')) > int(CURRENT_RELEASE_TAG.removeprefix('qml-launcher-preview-'))
        except ValueError:
            return False
    return version_tuple(tag) > version_tuple(VERSION)


def latest_release():
    """(version, zip URL, page URL) of the newest GitHub release."""
    request = urllib.request.Request(RELEASES_API, headers={'Accept': 'application/vnd.github+json',
                                                            'User-Agent': 'bbport-launcher'})
    with urllib.request.urlopen(request, timeout=15) as response:
        release = json.load(response)
    version = release['tag_name']
    url = next((asset['browser_download_url'] for asset in release.get('assets', [])
                if asset.get('name', '').lower().endswith('.zip')), None)
    page = release.get('html_url') or RELEASES_PAGE
    try:
        CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        UPDATE_CACHE.write_text(json.dumps({'checked': time.time(), 'version': version, 'url': url,
                                            'page': page}), encoding='utf-8')
    except OSError:
        pass
    return version, url, page


def remembered_update():
    """(version, zip URL, page URL) of the last release GitHub reported, if it is newer than
    this launcher; None otherwise (also once this version is installed)."""
    cached = load_json(UPDATE_CACHE, {})
    version = cached.get('version') if isinstance(cached, dict) else None
    if not isinstance(version, str) or not release_is_newer(version):
        return None
    return version, cached.get('url') or None, cached.get('page') or RELEASES_PAGE


def quiet_update_check(settings, now=None):
    """Direct launches: asks GitHub at most every QUIET_CHECK_INTERVAL and only remembers the
    answer (UPDATE_CACHE); nothing is shown while the game runs."""
    if not settings.get('check_updates', True) or COOP_TEST:
        return False
    cached = load_json(UPDATE_CACHE, {})
    checked = cached.get('checked') if isinstance(cached, dict) else None
    now = time.time() if now is None else now
    if isinstance(checked, (int, float)) and 0 <= now - checked < QUIET_CHECK_INTERVAL:
        return False
    try:
        latest_release()
    except (OSError, ValueError, KeyError):
        return False
    return True


def start_quiet_update_check(settings):
    timer = threading.Timer(QUIET_CHECK_DELAY, quiet_update_check, args=(settings,))
    timer.daemon = True
    timer.start()
    return timer


def extract_update(archive, folder):
    """Unpacks the downloaded release into folder. Entry names may use either separator:
    releases up to 1.9.7 were zipped by Windows PowerShell, which writes backslashes (#75)."""
    folder = Path(folder)
    with zipfile.ZipFile(archive) as package:
        for entry in package.infolist():
            parts = [part for part in entry.filename.replace('\\', '/').split('/') if part not in ('', '.')]
            if not parts or '..' in parts or ':' in parts[0]:
                continue  # nothing outside the folder
            path = folder.joinpath(*parts)
            if entry.filename.endswith(('/', '\\')):
                path.mkdir(parents=True, exist_ok=True)
                continue
            path.parent.mkdir(parents=True, exist_ok=True)
            with package.open(entry) as source, open(path, 'wb') as out:
                shutil.copyfileobj(source, out, 1 << 20)


def update_installing_mark():
    return UPDATE_DIR / 'installing'


def remove_update_folder():
    """Removes UPDATE_DIR unless an update is being installed from it: the installer keeps
    update_installing_mark() open, so it cannot be deleted. A launcher opened during the
    install used to delete the files not copied yet, leaving an installation without its new
    files ("No module named 'game_check'", #38). False when the folder is in use."""
    try:
        update_installing_mark().unlink(missing_ok=True)
    except OSError:
        return False
    shutil.rmtree(UPDATE_DIR, ignore_errors=True)
    return True


def copy_release(source, target, files=None):
    """Copies the release in source over target, leaving the player's USER_FILES at the top
    alone; `files` limits it to those relative paths. Returns the files not copied."""
    source, target = Path(source), Path(target)
    if files is None:
        files = [path.relative_to(source) for path in sorted(source.rglob('*'))
                 if path.is_file() and path.relative_to(source).parts[0] not in USER_FILES]
    failed = []
    for relative in files:
        try:
            (target / relative).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source / relative, target / relative)
        except OSError:
            failed.append(relative)
    return failed


def install_update(target, wait_pid):
    """--install-update TARGET PID, run by the downloaded version from its temporary folder:
    waits for the old launcher to close, copies this version over TARGET (never the saves,
    settings or mods) and starts it."""
    target = Path(target)
    try:
        mark = open(update_installing_mark(), 'w')  # held open until done: remove_update_folder
    except OSError:
        mark = None
    kernel = ctypes.windll.kernel32
    handle = kernel.OpenProcess(0x00100000, False, int(wait_pid))  # SYNCHRONIZE
    if handle:
        kernel.WaitForSingleObject(handle, 60000)
        kernel.CloseHandle(handle)
    pending = None
    for attempt in range(30):
        # Files still in use (the old launcher is closing) are tried again; every file of the
        # release must arrive, new ones included.
        pending = copy_release(PORT_DIR, target, pending)
        if not pending:
            break
        time.sleep(1)
    if mark:
        mark.close()
    if pending:
        ctypes.windll.user32.MessageBoxW(None, _(
            'Could not install the update. Download it from the releases page.',
            'Не удалось установить обновление. Скачайте его со страницы релизов.'), 'Bloodborne', 0x10)
        webbrowser.open(RELEASES_PAGE)
        return 1
    subprocess.Popen([str(target / 'Bloodborne.exe')], cwd=str(target))
    return 0


def missing_port_files(port=None):
    """run.py and the scripts it needs that the port folder lacks, by run.py's START_SCRIPTS
    (frozen: this launcher's own copy of run.py, so this version's list)."""
    port = Path(port or PORT_DIR)
    missing = [] if (port / 'run.py').is_file() else ['run.py']
    try:
        import run
        return missing + run.missing_port_files(port)
    except (ImportError, AttributeError):  # no run.py to read the list from, or an old one
        return missing or ['run.py']


def own_release(releases, version=None, tag=None):
    """(zip URL, page URL) of this launcher's release in a GitHub releases list: the one tagged
    CURRENT_RELEASE_TAG, else the one named "Bloodborne Windows Launcher <VERSION> (...)"."""
    version, tag = version or VERSION, tag or CURRENT_RELEASE_TAG

    def zip_url(release):
        assets = [asset for asset in release.get('assets') or []
                  if str(asset.get('name', '')).lower().endswith('.zip')]
        assets.sort(key=lambda asset: asset.get('name') != 'bbport-windows.zip')
        return next((asset['browser_download_url'] for asset in assets if asset.get('browser_download_url')), None)

    def named(release):
        match = re.match(r'Bloodborne Windows Launcher (\S+)', release.get('name') or '')
        return bool(match) and match.group(1) == version

    for wanted in (lambda release: release.get('tag_name') == tag, named):
        for release in releases:
            if isinstance(release, dict) and not release.get('draft') and wanted(release):
                if url := zip_url(release):
                    return url, release.get('html_url') or RELEASES_PAGE
    return None


def download(url, path, progress=None):
    """A release zip to path; progress(percent) each time the percentage changes."""
    request = urllib.request.Request(url, headers={'User-Agent': 'bbport-launcher'})
    with urllib.request.urlopen(request, timeout=60) as response, open(path, 'wb') as out:
        total, done, shown = int(response.headers.get('Content-Length') or 0), 0, -1
        while chunk := response.read(1 << 20):
            out.write(chunk)
            done += len(chunk)
            percent = done * 100 // total if total else 0
            if progress and percent != shown:
                shown = percent
                progress(percent)


def repair_copy(source, target):
    """Every file of the release in source over target but USER_FILES and LAUNCHER_FILES.
    Returns the files not copied (in use)."""
    source = Path(source)
    files = [path.relative_to(source) for path in sorted(source.rglob('*'))
             if path.is_file() and path.relative_to(source).parts[0] not in USER_FILES + LAUNCHER_FILES]
    return copy_release(source, target, files)


def repair_port(progress=None, target=None):
    """Downloads this version's release and copies it over the port folder (repair_copy), so a
    folder with files missing or left from another version matches this launcher again.
    Returns (files still missing, files not copied); raises OSError when it cannot."""
    target = Path(target or PORT_DIR)
    if not FROZEN or COOP_TEST:
        raise OSError('only a packaged launcher repairs itself')
    request = urllib.request.Request(RELEASES_LIST_API, headers={'Accept': 'application/vnd.github+json',
                                                                 'User-Agent': 'bbport-launcher'})
    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            releases = json.load(response)
    except ValueError as error:
        raise OSError(f'unexpected answer from GitHub: {error}') from error
    found = own_release(releases if isinstance(releases, list) else [])
    if not found:
        raise OSError(f'version {VERSION} was not found on GitHub')
    if not remove_update_folder():
        raise OSError('an update is still being installed; try again in a minute')
    try:
        UPDATE_DIR.mkdir(parents=True, exist_ok=True)
        archive = UPDATE_DIR / 'repair.zip'
        download(found[0], archive, progress)
        try:
            extract_update(archive, UPDATE_DIR / 'new')
        except zipfile.BadZipFile as error:
            raise OSError(f'the download is damaged: {error}') from error
        source = next((p.parent for p in (UPDATE_DIR / 'new').rglob('Bloodborne.exe')), None)
        if not source:
            raise OSError('Bloodborne.exe is missing from the download')
        failed = repair_copy(source, target)
    finally:
        remove_update_folder()
    return missing_port_files(target), failed


def main():
    global LANG
    args = sys.argv[1:]
    if args and args[0] in ('--run', '--script'):
        sys.exit(run_role(args))
    import bbport_startup
    keys_held = bbport_startup.launcher_keys_held()  # read first: the user may let go soon
    saved = load_json(CONFIG_FILE, {})
    settings = app_settings(saved)
    if bbport_startup.migrate(saved, settings):
        try:
            CONFIG_FILE.write_text(json.dumps(saved, indent=2, ensure_ascii=False), encoding='utf-8')
        except OSError:
            pass
        settings = app_settings(saved)
    LANG = settings.get('ui_language') or windows_language()
    if args[:1] == ['--install-update'] and len(args) == 3:
        sys.exit(install_update(args[1], args[2]))
    if FROZEN and UPDATE_DIR not in PORT_DIR.parents:
        remove_update_folder()  # what a finished update left behind
    # Without a usable game folder there is nothing to play yet, and an incomplete port folder
    # needs Repair: open the launcher instead.
    if ('--play' in args and (Path(settings['game_dir'] or '.') / 'eboot.bin').is_file()
            and not missing_port_files()):
        sys.exit(play_without_window(settings))
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)  # sharp text on scaled displays
    except (AttributeError, OSError):
        pass
    if '--classic-launcher' not in args:
        try:
            from bbport_launcher_qml import run_ui
        except ImportError as error:
            import traceback
            try:
                CONFIG_DIR.mkdir(parents=True, exist_ok=True)
                (CONFIG_DIR / 'launcher-startup.log').write_text(traceback.format_exc(), encoding='utf-8')
            except OSError:
                pass
            print(f'Qt Quick unavailable; opening classic launcher: {error}', file=sys.stderr)
        else:
            sys.exit(run_ui(bbport_startup.route(settings, args, DATA_DIR, keys_held)))
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    root = tk.Tk()
    Launcher(root, tk, ttk, filedialog, messagebox)
    root.mainloop()


if __name__ == '__main__':
    main()
