"""Compile selected shadPS4/GoldHEN XML patches into out/patches.bin for the loader.

Patch addresses are PS4 virtual addresses (eboot base 0x400000); the loader's
image places eboot vaddr 0 at image offset 0. Only literal writes are supported
(bytes, bytes16/32/64, float32/64, utf8, utf16); pattern ("mask") patches are rejected.
"""
import argparse
import functools
import hashlib
import json
import os
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0,str(Path(__file__).resolve().parent))
from cpu_compat import site_check  # noqa: E402

EBOOT_BASE=0x400000
# BB_FPS presets: patch names from patches/Bloodborne.xml (app version 01.09).
FPS_PRESETS={'30':[],'60':['60 FPS++'],'90':['90 FPS++'],'uncap':['Uncap FPS++']}
# 120 and 144 (Awakenchan/BloodBorneButPc, GPL): Kyo's current 90 FPS++ list ("90 FPS++ (rate
# base)") with its frame-rate values for that rate. Uncap FPS++ (another design) deadlocked the
# game's model jobs at the end of Vicar Amelia's intro; the 60/90 FPS++ design did not. The values
# follow the rate from the game's own 30: frame times 1/fps, frame counts fps/6 and fps/3, timings
# 3/fps, a thread's sleep 990000/fps us, the limiter's longest sleep just under a frame, and the
# frame time the character update steps its stamina by, 1.125/fps (kept at 90's 0.0125, the
# character ran 1.5 and 1.8 times as fast at 120 and 144).
SCALED_FPS={'120':120,'144':144}
SCALED_BASE='90 FPS++ (rate base)'
SCALED_LIMITER_SLEEP={90:10000,120:8000,144:6500}  # us, the frame limiter's longest sleep
# The base's lines whose values follow the rate: address -> [(byte position in the line, struct
# format, value for a rate)]. Each line must hold the value for 90 there (rate_scaled checks).
SCALED_VALUES={
    0x0F383CA:[(0,'<f',lambda r:1/r)],                              # physics step
    0x1CBDB42:[(6,'<f',lambda r:0.0125*90/r)],                      # stamina frame time
    0x1929676:[(0,'<i',lambda r:r//6)],
    0x192C405:[(0,'<i',lambda r:r//3)],
    **{a:[(5,'<f',lambda r:1/r)] for a in (0x2434840,0x243485A,0x243487E,0x24348B3)},
    0x25629F0:[(1,'<I',lambda r:990000//r)],                        # thread sleep, us
    0x2C012CC:[(2,'<I',SCALED_LIMITER_SLEEP.get),(9,'<I',SCALED_LIMITER_SLEEP.get)],
    0x4CF6420:[(i,'<f',lambda r:3/r) for i in (0,4,8,12)],
    0x4D27FD4:[(0,'<f',lambda r:-3/r)],
    0x4D27FE0:[(0,'<f',lambda r:3/r)],
    0x4D29170:[(0,'<f',lambda r:1/r)],
}
# The 1.09 eboot's bytes at every site the base writes (patch_fingerprint of SCALED_BASE alone,
# a hash): another build there gets no 120/144 preset (30 FPS).
SCALED_FINGERPRINT='216bafcef13f00e358b8d8245ca3b8bccf7ad47f4ff2984295b06ed2fd776697'


def fps_preset(fps,elf,xml,app_version):
    """The patch names of an FPS preset; none for 120/144 on an eboot whose code at the base's
    sites is not the 1.09 build's."""
    if fps not in SCALED_FPS: return list(FPS_PRESETS[fps])
    if patch_fingerprint(elf,xml,app_version,[SCALED_BASE])!=SCALED_FINGERPRINT:
        print(f'Patches: {fps} FPS left out: this eboot.bin has other code where {SCALED_BASE} writes '
              '(30 FPS)')
        return []
    return [SCALED_BASE]


def preset_writes(xml,preset,fps,app_version,segments,elf=None,settings=None):
    """The FPS preset's writes: for 120/144 the base with its values for that rate, then the
    high-FPS corrections (rate_corrections)."""
    writes=compile_patches(xml,preset,app_version,segments)
    if not preset: return writes
    if fps in SCALED_FPS:
        writes=rate_scaled(writes,SCALED_FPS[fps])
        print(f'Patches: {fps} FPS: {SCALED_BASE} with its frame-rate values for {fps}')
    else:
        writes+=cloth_writes(xml,app_version,segments,elf,PRESET_RATES.get(fps),settings or {})
    writes=rate_corrections(writes,PRESET_RATES.get(fps),elf,settings or {})
    return frame_steps(writes,PRESET_RATES.get(fps),elf,settings or {})


# Corrections to the community frame-rate lists (Kyo's 1.0 lists), from Awakenchan/BloodBorneButPc
# (after bbhost's frame_rate.cpp and follow_camera.cpp, Kyo's 1.3 lists and yogesh1239's AI/Camera
# fix). They change only sites a list writes (and the camera's own eases, the wind's loads): the
# 60, 90 and uncapped lists of this file write none of the effect or cloth sites, so those
# corrections are for the 120/144 base.
#
# Effects keep the data's rate (effect_rate_fix=0 / BB_EFFECT_RATE_FIX=0: off). The effect
# manager's 30.0 (0x26FB39F) and the GPU particle update's (0x4CAD7FC) turn effect data, written in
# 30 fps frames, into seconds; the lists set both to 60, which halved each emitter's interval and
# lifetime and ran GPU particles twice as fast (the motes rising from a lamp's travel and a
# boss's blood rain went missing).
EFFECT_RATE_SITES=(0x26FB39F,0x4CAD7FC)
# Event scripts keep the data's waits (event_frames_fix=0 / BB_EVENT_FRAMES_FIX=0: off). The
# EMEVD time conditions (bank 1, 0x1BC8630) and waits (bank 1001, 0x1BC8A90) turn their frame
# counts, written in 30 fps frames, into seconds by the game's 1/30 at 0x4D27284 and 0x4D2728C
# and wait those seconds. The 60, 90 and uncapped lists set 0x4D2728C to 0.04 and
# the 60 list 0x4D27284 to 1/15, so every scripted wait in frames ran 1.2 times (a condition 2
# times) as long; Kyo's rate base writes neither. The Doll's level-up (m21_00_00_00 event
# 12100113) plays her kneel, waits 89 frames and loops the kneeling pose: 3.56 s instead of
# 2.97 s let the kneel end and she stood for a moment before the loop pulled her down (#60).
EVENT_FRAME_SITES=(0x4D27284,0x4D2728C)
# The cloth above 60, as Kyo's 1.3 (cloth_fix=0 / BB_CLOTH_FIX=0: off): one substep a frame runs
# the solver at the frame rate against the 60 its part files are tuned for, and the six
# constraint caves scale its stiffness by the scalars at 0x4D3BD00: (60/fps)^2. The 1.0 lists
# scale it by 60/fps and also point the damping exponent's load (0xB6FBD4) at it and scale the
# integrator's dt*dt (a hook at 0xB73F84, its cave at 0xB73FA8, the factor at 0x4D3BD10), which 1.3
# leaves as the game has them: the cloth jumped and ran fast at 90 and above.
CLOTH_STIFFNESS=0x4D3BD00
CLOTH_LEFT_OUT=((0xB6FBD4,0xB6FBD8),(0xB73F84,0xB73F8D),(0xB73FA8,0xB73FC1),(0x4D3BD10,0x4D3BD20))
# Kyo's 60, 90 and uncapped lists write none of it, so their cloth ran every substep: the base's
# substep count (0xB6E67A) and constraint caves (0xB20000-0xB43000) are added for them
# (cloth_writes). Uncapped the stiffness is 1 until src/cloth_rate.c points the caves at a cell
# it sets each frame from the frame time (cloth_uncapped=0 / BB_CLOTH_UNCAPPED=0: none of it); it
# does so above 60 at a fixed preset too, over the (60/fps)^2 written here (BB_FRAME_TIME_PRESETS=0: not).
CLOTH_SUBSTEPS=0xB6E67A
CLOTH_CAVES=(0xB20000,0xB43000)
# Grass and tree wind at 30's pace and bend (wind_fix=0 / BB_WIND_FIX=0: off; after
# droogie/bbhost). The wind query (0x2713870) steps the gusts' and pushes' times and lives by its
# load at 0x2713D8D a frame, the game's 1/60 (0x4D29E70): 1/60 s of wind each 1/30 s at 30, fps/30
# times that with the lists (60, 90, uncapped), twice with the base's real-dt slot (0x2713D91 on).
# Its load at 0x27149EA weighs the force (the bend) and a push's ramp-in by the same step. The
# step load reads 0x4D29E74 at 1/(2 fps) instead (the step of the device's +0x100 != 1 branch,
# only read here) and the force load the game's 1/60; src/wind_pace.c points the step loads at a
# cell it sets each frame (at a fixed preset over this 1/(2 fps), unless BB_FRAME_TIME_PRESETS=0).
WIND_STEP=0x4D29E70
WIND_STEP_LOAD=0x2713D8D       # vmovss xmm4, [rip+disp32]
WIND_PACE=0x4D29E74
WIND_FORCE_LOAD=0x27149EA      # vmovdqa xmm14, [rbp-0x170]: vmovss xmm14, [rip+disp32] instead
WIND_GUARDS=((WIND_STEP_LOAD-EBOOT_BASE,8,'5f5dbe16226b9950'),
             (WIND_FORCE_LOAD-EBOOT_BASE,8,'b6397d3e89d71a8c'))
# The follow camera (ChrExFollowCam) eases by weights a frame (camera_fix=1 / BB_CAMERA_FIX=1: on;
# off by default, as Awakenchan ships it: players found it trailing too far behind when running).
# The lists store Kyo's 60 fps values for them at every rate - its constructor's immediates (field
# by address below) and two runs of stores that replace its parameter reads (mov dword
# [r13+field], value) - so at 90, 120 and 144 it chased, turned and settled 1.5, 2 and 2.4 times as
# fast. They are converted from a 60 fps frame: a weight w to 1-(1-w)^(60/fps), the pitch return's
# kept share k (+0x1a8) to k^(60/fps); the direct pitch angle (+0x238) is an angle. Three weights no
# list tunes are the game's own for a 30 fps frame, converted from it at every rate: the
# LockCamParam blend (+0x330, an immediate no list writes), the distance ease (0x4D25E68, 0.1) and
# the chase-rate ease (0x4D25E94, 1/30).
CAMERA_FIELDS={0x183a4c3:0x19c,0x183a4cd:0x1a0,0x183a4d7:0x1a4,0x183a4e1:0x1a8,0x183a4f5:0x1b0,
               0x183a4ff:0x1b4,0x183a509:0x1b8,0x183a527:0x1c4,0x183a531:0x1c8,0x183a53b:0x1cc,
               0x183a649:0x238,0x183a653:0x23c,0x183a65d:0x240,0x183a8c1:0x19c,0x183a8cb:0x1a0,
               0x183a8d5:0x1a4,0x183a8df:0x1a8,0x183a8f3:0x1b0,0x183a8fd:0x1b4,0x183a907:0x1b8,
               0x183a925:0x1c4,0x183a92f:0x1c8,0x183a939:0x1cc,0x183aa47:0x238,0x183aa51:0x23c,
               0x183aa5b:0x240}
CAMERA_STORES=((0x183FF29,0x183FF6B),(0x1840799,0x18407E6))
CAMERA_STORE=bytes.fromhex('41c785')  # the lists' mov dword [r13+disp32], imm32
CAMERA_ANGLE,CAMERA_KEPT=0x238,0x1a8
# The game's own value at each of these (a float the eboot must hold there, else left alone).
CAMERA_EASES={0x4D25E68:0.1,0x4D25E94:1/30,0x183A7DE:0.05}  # the last: mov dword [rbx+0x330], 0.05
PRESET_RATES={'60':60,'90':90,**SCALED_FPS}  # uncapped: the rate is not known here


# Fixed time steps (frame_step_fix=0 / BB_FRAME_STEP_FIX=0: off). The game advances physics,
# falling objects and other systems by a fixed step a frame, 1/30 s. The 60 FPS++ list halves
# them to 1/60 and the 90 FPS++ lists keep that 1/60 (78 sites in the rate base, 95 in the older
# 90 FPS++), so above 60 FPS they ran fps/60 times as fast: gravity 1.5x at 90, 2x at 120, 2.4x
# at 144. Each 4-byte line that writes 1/60 where the eboot has the game's 1/30 gets 1/fps.
# Awakenchan/BloodBorneButPc 0.9.32 beta 6 made the same change (released without source); these
# sites are found again from the lists and the eboot, not taken from that build. Below the target
# src/frame_time.c makes them follow the frame time (tools/frame_time_sites.py; BB_FRAME_TIME_PRESETS=0: not).
FRAME_STEP_GAME=struct.pack('<f',1/30)
FRAME_STEP_LIST=struct.pack('<f',1/60)


def frame_steps(writes, fps, elf, settings):
    """The preset's writes with its fixed 1/60 steps at 1/fps, above 60 FPS."""
    if not fps or fps<=60 or elf is None or not setting_on(settings,'frame_step_fix','BB_FRAME_STEP_FIX','1'):
        return writes
    step=struct.pack('<f',1/fps)
    out=[]
    count=0
    for offset,data in writes:
        if data==FRAME_STEP_LIST and eboot_bytes(elf,offset,offset+4)==FRAME_STEP_GAME:
            data=step
            count+=1
        out.append((offset,data))
    print(f'Patches: {count} fixed frame steps at 1/{fps} s (frame_step_fix=0: 1/60)')
    return out


def setting_on(settings,key,env,default):
    return os.environ.get(env,settings.get(key,default))!='0'


def camera_value(field, value, frames):
    """A camera field's value for a frame `frames` of the frames it was tuned for."""
    if field==CAMERA_ANGLE or not 0<value<1: return value
    return value**frames if field==CAMERA_KEPT else 1-(1-value)**frames


def cloth_writes(xml, app_version, segments, elf, fps, settings):
    """The base's cloth writes for a 60, 90 or uncapped list, at a stiffness of (60/fps)^2
    (uncapped 1, which src/cloth_rate.c follows the frame time from); none with cloth_fix=0 (or
    uncapped cloth_uncapped=0), or when the eboot has other code where the base writes."""
    if not setting_on(settings,'cloth_fix','BB_CLOTH_FIX','1'): return []
    if not fps and not setting_on(settings,'cloth_uncapped','BB_CLOTH_UNCAPPED','1'): return []
    if elf is None or patch_fingerprint(elf,xml,app_version,[SCALED_BASE])!=SCALED_FINGERPRINT:
        print('Patches: cloth left as the list has it: this eboot.bin has other code where '
              f'{SCALED_BASE} writes')
        return []
    stiffness=(60/fps)**2 if fps else 1.0
    out=[]
    for offset,data in compile_patches(xml,[SCALED_BASE],app_version,segments):
        a=offset+EBOOT_BASE
        if a==CLOTH_SUBSTEPS or CLOTH_CAVES[0]<=a<CLOTH_CAVES[1]:
            out.append((offset,data))
        elif a==CLOTH_STIFFNESS:
            out.append((offset,struct.pack('<f',stiffness)*4))
    print(f'Patches: cloth at {fps or "uncapped"} FPS: one solver substep a frame, stiffness '
          + (f'{stiffness:.3f}' if fps else '(60 x frame time)^2 each frame (src/cloth_rate.c)'))
    return out


def wind_pace(writes, fps, elf):
    """`writes` with the wind's step at 1/(2 fps) and its force at the game's 1/60 (uncapped: the
    force only); None, with nothing changed, when the eboot or the list has other bytes there."""
    f=lambda v: struct.pack('<f',v)
    rel=lambda insn,target: struct.pack('<i',target-(insn+8))
    step,pace,force=WIND_STEP-EBOOT_BASE,WIND_PACE-EBOOT_BASE,WIND_FORCE_LOAD-EBOOT_BASE
    load=WIND_STEP_LOAD-EBOOT_BASE
    # The list may write the step load's displacement (the base) and the pace cell, nothing else.
    own={load+4:4,pace:4}
    clash=[o for o,d in writes if own.get(o)!=len(d) and any(lo<o+len(d) and o<lo+n for lo,n in
           ((step,4),(load,8),(force,8),(pace,4)))]
    if (clash or not all(site_matches(elf,g) for g in WIND_GUARDS)
            or eboot_bytes(elf,step,step+4)!=f(1/60) or eboot_bytes(elf,pace,pace+4)!=f(1/30)):
        print(f'Patches: wind left as the list has it (unexpected code at {WIND_STEP_LOAD:#x}, '
              f'{WIND_FORCE_LOAD:#x} or {WIND_STEP:#x})')
        return None
    ours={force:bytes.fromhex('c57a1035')+rel(WIND_FORCE_LOAD,WIND_STEP)}
    if fps:
        ours[load+4]=rel(WIND_STEP_LOAD,WIND_PACE)
        ours[pace]=f(1/(2*fps))
    out=[(o,ours.pop(o) if o in ours else d) for o,d in writes]
    return out+sorted(ours.items())


def rate_corrections(writes, fps, elf, settings):
    """The preset's writes with the corrections above for `fps` (None: uncapped; the effects'
    rate is put back and the wind's force weighed as at 30)."""
    f=lambda v: struct.pack('<f',v)
    effects=setting_on(settings,'effect_rate_fix','BB_EFFECT_RATE_FIX','1')
    events=setting_on(settings,'event_frames_fix','BB_EVENT_FRAMES_FIX','1')
    cloth=bool(fps and fps>60 and setting_on(settings,'cloth_fix','BB_CLOTH_FIX','1'))
    camera=bool(fps and setting_on(settings,'camera_fix','BB_CAMERA_FIX','0'))
    wind=bool(elf is not None and setting_on(settings,'wind_fix','BB_WIND_FIX','1'))
    out=[]
    eases=set()
    for offset,data in writes:
        a=offset+EBOOT_BASE
        if effects and a in EFFECT_RATE_SITES and len(data)==4:
            continue
        if events and a in EVENT_FRAME_SITES and len(data)==4:
            continue
        if cloth:
            left=next(((lo,hi) for lo,hi in CLOTH_LEFT_OUT if lo<=a<hi),None)
            if left:
                if a+len(data)>left[1]: raise ValueError(f'frame-rate patch: {a:#x} reaches past {left[1]:#x}')
                continue
            if a==CLOTH_STIFFNESS:
                values=struct.unpack('<4f',data) if len(data)==16 else ()
                if len(set(values))!=1 or not 0<values[0]<=1:
                    raise ValueError(f'frame-rate patch: unexpected cloth stiffness at {a:#x}')
                data=f((60/fps)**2)*4
        if camera:
            if a in CAMERA_FIELDS and len(data)==4:
                data=f(camera_value(CAMERA_FIELDS[a],struct.unpack('<f',data)[0],60/fps))
            elif any(lo<=a<hi for lo,hi in CAMERA_STORES) and len(data)==11 and data[:3]==CAMERA_STORE:
                field,value=struct.unpack('<If',data[3:])
                data=data[:7]+f(camera_value(field,value,60/fps))
            elif a in CAMERA_EASES and len(data)==4:
                data=f(camera_value(None,CAMERA_EASES[a],30/fps))
                eases.add(a)
        out.append((offset,data))
    if camera and elf is not None:
        # The eases no line of the list writes, where the eboot holds the game's value.
        for a,value in CAMERA_EASES.items():
            if a in eases: continue
            at=a-EBOOT_BASE
            if eboot_bytes(elf,at,at+4)==f(value):
                out.append((at,f(camera_value(None,value,30/fps))))
            else:
                print(f'Patches: camera ease at {a:#x} left alone (unexpected code)')
    if wind:
        paced=wind_pace(out,fps,elf)
        wind=paced is not None
        out=paced if wind else out
    names=[n for n,on in (('effect rate',effects),('event waits',events),('cloth',cloth),('camera',camera),('wind',wind)) if on]
    print(f'Patches: frame-rate corrections: {", ".join(names) or "none"}')
    return out


def rate_scaled(writes, fps):
    """The base's writes with their frame-rate values for `fps`; ValueError if a line is not the
    90 FPS value the table expects (another list under that name)."""
    out,seen=[],set()
    for offset,data in writes:
        fields=SCALED_VALUES.get(offset+EBOOT_BASE)
        if fields:
            data=bytearray(data)
            for at,fmt,value in fields:
                size=struct.calcsize(fmt)
                if at+size>len(data) or bytes(data[at:at+size])!=struct.pack(fmt,value(90)):
                    raise ValueError(f'{SCALED_BASE}: unexpected value at {offset+EBOOT_BASE:#x}')
                data[at:at+size]=struct.pack(fmt,value(fps))
            data=bytes(data)
            seen.add(offset+EBOOT_BASE)
        out.append((offset,data))
    missing=set(SCALED_VALUES)-seen
    if missing: raise ValueError(f'{SCALED_BASE}: no write at {", ".join(hex(a) for a in sorted(missing))}')
    return out
# Fixes applied with every preset. The game's two per-frame render heaps (256 KiB each,
# mov edx,0x40000 at guest offsets 0x2195c09/0x2195c46) run out in a busy frame; the pool at
# 0xe45560 then returns NULL and the caller copies into it (memcpy at guest offset 0x21d5b40).
FIXES=['Larger Frame Heaps']
# Upscaler presets (bbport.ini "preset", the in-game menu): output / render size ratio. The game
# then renders at 1920x1080 / ratio and the port's temporal upscaler restores the output size.
OUTPUT_SIZE=(1920,1080)
PRESET_SCALES=[1.0,1.5,1.7,2.0,3.0]
# The community patch changes two independent consumers: the game render/window setup
# and the UI movie viewport. Keep the latter at native size so glyph rasterisation and
# vector tessellation do not inherit the scene's FSR resolution.
RESOLUTION_TEMPLATE='Resolution Patch 1280x720 (16:9)'
# Outputs wider than 16:9: Kyo's 21:9 patch widens the camera's aspect ratio, the HUD canvas
# (1080 high, wider), the HUD projection and the screen-to-HUD mapping of the lock-on and HP-bar
# markers; ultrawide_writes recomputes those values for the output's own aspect ratio. From
# emurphy04 (Supermedo/bloodborne_pc PR #70). Outputs narrower than 16:9 (16:10, portrait) keep
# the camera hook of src/aspect_ratio.cpp and a centred 16:9 stage.
ULTRAWIDE_TEMPLATE='Resolution Patch 2560x1080 (21:9)'
ULTRAWIDE_CANVAS=2560 # the HUD canvas width written by ULTRAWIDE_TEMPLATE
CAMERA_ASPECT=0x0183A35D-EBOOT_BASE
HUD_PROJECTION_WIDTH=0x04D26EA4-EBOOT_BASE
MARKER_BIAS=(0x04CF9A10-EBOOT_BASE,0x04CF9A70-EBOOT_BASE,0x04CF9AE0-EBOOT_BASE)
# Effect switches (bbport.ini, in-game menu, launcher): key -> (patch when the key is 0, patch
# when it is 1). The game reads these at start; a change applies after a restart.
EFFECTS={
    'effect_chromatic_aberration':('Disable Chromatic Aberration',None),
    'effect_dof':('Disable DoF',None),
    'effect_motion_blur':('Disable Motion Blur (perf increase)',None),
    'effect_ssao':('Disable SSAO',None),
    'effect_game_aa':('Disable AA',None),
    'effect_dynamic_shadows':('Disable Dynamic Light Shadows (perf increase)',None),
    'effect_ssr':(None,'Enable Screen Space Reflections (READ NOTE)'),
    'skip_intro':(None,'Skip Intro'),
    'debug_camera':(None,'Restore Debug Camera'),
    'debug_menu':(None,'Restore Debug Menu (READ NOTES)'),
    # Cheats and gameplay tweaks (launcher "Cheats" page).
    'cheat_no_death':(None,'Player No Dead (Read note)'),
    'cheat_stealth':(None,'Player Stealth (Read note)'),
    'cheat_silent':(None,'Player Silent (Read note)'),
    'cheat_rally_no_decay':(None,'No Rally Decay'),
    'cheat_enemy_control':(None,'Enemy Control'),
    'tweak_no_rally':(None,'Disable Rally (HP Regain)'),
    'tweak_camera_distance':(None,'Increased camera distance'),
    'tweak_no_camera_rotation':(None,'Disable Camera Auto Rotation via Movement'),
    'tweak_easy_run':(None,'Sensitive Analog Input (easier to run)'),
    'tweak_ragdoll':(None,'DS1-like physics'),
    # parsifal295, Supermedo/bloodborne_pc PR #17.
    'tweak_circle_confirm':(None,'Circle Confirm (Japanese/Asian button layout)'),
}
# Left out online (BB_ONLINE=1; not seamless co-op's BB_COOP=1, among players who chose it),
# whatever bbport.ini, BB_PATCHES or a third-party file asks for; the live stamina cheats are off there too (bbport_settings.cpp OnlineBlocked).
ONLINE_BLOCKED={EFFECTS[key][1] for key in EFFECTS if key.startswith(('cheat_','debug_'))}
# model_lod: -2 highest, -1 higher, 0 the game's, 1 lower, 2 lowest.
MODEL_LOD={'-2':'Model LOD -2 (Highest)','-1':'Model LOD -1 (Higher)','1':'Model LOD 1 (Lower)','2':'Model LOD 2 (Lowest)'}


def game_app_version(game):
    """APP_VER of the game folder's param.sfo ("01.09"), or None when it cannot be read.
    Game of the Year discs (CUSA03173) ship the 1.09 code with APP_VER 01.00: a game whose
    eboot.bin has the 1.09 patch fingerprint is 01.09 whatever its param.sfo says."""
    try:
        from prepare import sfo
        version=sfo((Path(game) / 'sce_sys/param.sfo').read_bytes()).get('APP_VER')
    except (OSError, ValueError, ImportError):
        return None
    if version!='01.09':
        eboot=Path(game).resolve()/'eboot.bin'
        try:
            stat=eboot.stat()
        except OSError:
            return version
        # Keyed on size and time too: the launcher stays open while eboot.bin is replaced.
        if eboot_is_109(str(eboot),stat.st_size,stat.st_mtime_ns):
            return '01.09'
    return version


@functools.lru_cache(maxsize=None)
def eboot_is_109(path,_size=None,_mtime=None):
    try:
        from prepare import parse_self
        elf=bytes(parse_self(Path(path).read_bytes())[0])
        return patch_fingerprint(elf,BUILTIN_XML,'01.09')==PATCH_FINGERPRINTS['01.09']
    except (OSError, ValueError, KeyError, IndexError, TypeError, struct.error, ET.ParseError, ImportError):
        return False


DEBUG_MENU = 'Restore Debug Menu (READ NOTES)'
DEBUG_MENU_FONTS = ('DbgFont14h.ccm', 'DbgFont14h.tpf')
# Under dvdroot_ps4: the folder the port's notes name, and the one bbport 0.5 found the patched
# game reading them from (BB_FILE_TRACE there). Either holds them.
DEBUG_MENU_FONT_DIRS = ('font', 'adhoc/font')


def find_ignoring_case(root, relative):
    """`root`/`relative` with each component matched ignoring case (a mod may ship Adhoc/Font or
    dbgfont14h.*); None when absent."""
    path = Path(root)
    for part in relative.split('/'):
        exact = path / part
        if exact.exists():
            path = exact
            continue
        try:
            path = next(p for p in path.iterdir() if p.name.lower() == part.lower())
        except (OSError, StopIteration):
            return None
    return path


def debug_menu_problem(game):
    """Why the debug menu patch cannot work with this game folder (with its mods), or None."""
    dvdroot = Path(game) / 'dvdroot_ps4'
    for folder in DEBUG_MENU_FONT_DIRS:
        found = [find_ignoring_case(dvdroot, f'{folder}/{name}') for name in DEBUG_MENU_FONTS]
        if all(f and f.is_file() and f.stat().st_size > 0 for f in found):
            return None
    places = ' or '.join(f'dvdroot_ps4/{folder}' for folder in DEBUG_MENU_FONT_DIRS)
    return (f'it needs non-empty {" and ".join(DEBUG_MENU_FONTS)} in {places} of the game folder '
            'or of a mod (https://www.nexusmods.com/bloodborne/mods/253)')


def validate_patch_requirements(names, game):
    """The patches to apply: raises for conflicts; leaves the debug menu out (with a message)
    when its font files are missing, rather than refusing to start the game."""
    if 'Restore Debug Camera' in names and 'Enemy Control' in names:
        raise ValueError('Restore Debug Camera conflicts with Enemy Control; enable only one')
    if DEBUG_MENU in names:
        if problem := debug_menu_problem(game):
            print(f'Patches: debug menu left out: {problem}; or turn debug_menu off in bbport.ini',
                  file=sys.stderr)
            return [n for n in names if n != DEBUG_MENU]
    return names


def online():
    return os.environ.get('BB_ONLINE')=='1' and os.environ.get('BB_COOP')!='1'


def online_allowed(names):
    """`names` without the patches online play leaves out (ONLINE_BLOCKED, or a name with
    "cheat" in it), when playing online."""
    if not online(): return names
    blocked=[n for n in names if n in ONLINE_BLOCKED or 'cheat' in n.lower()]
    if blocked: print(f'Patches: playing online, left out: {", ".join(blocked)}')
    return [n for n in names if n not in blocked]


def effect_patches(settings):
    names=[]
    for key,(off,on) in EFFECTS.items():
        if key not in settings: continue
        name=on if settings[key]=='1' else off
        if name: names.append(name)
    # Enemy Control and the free camera use the same buttons: the free camera wins.
    if 'Enemy Control' in names and 'Restore Debug Camera' in names:
        names.remove('Enemy Control')
        print('Patches: Enemy Control skipped (it conflicts with the free camera)')
    # Rally that never decays means nothing without Rally.
    if 'No Rally Decay' in names and 'Disable Rally (HP Regain)' in names:
        names.remove('No Rally Decay')
    lod=MODEL_LOD.get(settings.get('model_lod','0'))
    if lod: names.append(lod)
    return names
SCENE_WIDTH=0x02196A6B-EBOOT_BASE
SCENE_HEIGHT=0x02196A7A-EBOOT_BASE
UI_WIDTH=0x02358554-EBOOT_BASE
UI_HEIGHT=0x0235855D-EBOOT_BASE


def read_settings(path):
    path = Path(path)
    # Path.read_text() defaults to the process locale; bbport.ini may contain
    # UTF-8 outside that encoding. Match the Windows launcher (encoding='utf-8').
    settings={}
    if path.exists():
        for line in path.read_text(encoding='utf-8').splitlines():
            key,sep,value=line.partition('=')
            if sep and not line.startswith('#'): settings[key.strip()]=value.strip()
    return settings


def render_size(settings,override=''):
    """Render resolution for the upscaler preset, or None for native."""
    if override:
        w,h=(int(v) for v in override.lower().split('x'))
        return (w,h)
    if settings.get('upscaler','fsr3')=='off': return None
    preset=int(settings.get('preset','0') or 0)
    scale=PRESET_SCALES[max(0,min(preset,len(PRESET_SCALES)-1))]
    if scale==1.0: return None
    # Even sizes (the game has half-resolution buffers).
    return tuple(max(2,round(v/scale/2)*2) for v in OUTPUT_SIZE)


def output_size(settings):
    """Output (UI) size from bbport.ini output_res, e.g. 3840x2160; 1920x1080 by default."""
    try:
        w,h=(int(v) for v in settings.get('output_res','').lower().split('x'))
        if 256<=w<=7680 and 144<=h<=4320 and not (w%2 or h%2): return (w,h)
    except ValueError:
        pass
    return OUTPUT_SIZE


def scaled_sizes(settings):
    """(render, output) for an output other than 1080p (above it, or 720p for the Steam Deck):
    the game renders at output / preset scale (or at the output size without upscaler) and the
    upscaler fills the output. None at 1080p and for TAA (native, live host targets only)."""
    out=output_size(settings)
    if out==OUTPUT_SIZE or (settings.get('upscaler')=='taa' and is_16_9(out)): return None
    scale=1.0
    if settings.get('upscaler','fsr3') not in ('off','taa'):
        preset=int(settings.get('preset','0') or 0)
        scale=PRESET_SCALES[max(0,min(preset,len(PRESET_SCALES)-1))]
    render=tuple(max(2,round(v/scale/2)*2) for v in out)
    # A scene of exactly the UI coordinate space's size (1920x1080 at 4K Performance, the
    # ultrawide HUD canvas at 2560x1080 or 3840x1080 native) is indistinguishable from it: the
    # port's UI composition recognizes UI draws by that size (HUD gauges came out too long).
    canvas=hud_canvas(out)
    if render==canvas: render=(canvas[0]-4,canvas[1]-2)
    return render,out


def is_16_9(output):
    """16:9 to within a pixel of width: 1366x768 (16:9 is 1365.3 wide) plays as 16:9."""
    return abs(output[0]*9-output[1]*16)<=9


def ultrawide(output):
    """Wider than 16:9."""
    return output[0]*OUTPUT_SIZE[1]>output[1]*OUTPUT_SIZE[0] and not is_16_9(output)


def hud_canvas(output):
    """The HUD's coordinate space for an output: 1920x1080 up to 16:9, wider (still 1080 high,
    even width) for ultrawide outputs, e.g. 2580x1080 for 3440x1440. The port scales it to the
    output uniformly (UiComposition, BB_UI_RES)."""
    w,h=output
    if not ultrawide(output): return OUTPUT_SIZE
    return (round(OUTPUT_SIZE[1]*w/h/2)*2,OUTPUT_SIZE[1])


def ultrawide_writes(xml,output,app_version,segments):
    """ULTRAWIDE_TEMPLATE for `output`'s aspect ratio, without its scene and UI viewport sizes
    (resolution_writes). After resolution_writes: its 1920-wide HUD writes are replaced."""
    canvas=hud_canvas(output)[0]
    viewport={SCENE_WIDTH,SCENE_HEIGHT,UI_WIDTH,UI_HEIGHT}
    floats={CAMERA_ASPECT:(ULTRAWIDE_CANVAS/1080,output[0]/output[1]),
            HUD_PROJECTION_WIDTH:(ULTRAWIDE_CANVAS,canvas)}
    floats.update({site:(0.5*1920/ULTRAWIDE_CANVAS,0.5*1920/canvas) for site in MARKER_BIAS})
    old_width=ULTRAWIDE_CANVAS.to_bytes(3,'little')
    out=[]
    found=set()
    for offset,data in compile_patches(xml,[ULTRAWIDE_TEMPLATE],app_version,segments):
        if offset in viewport: continue
        if offset in floats:
            old,value=floats[offset]
            if len(data)!=4 or abs(struct.unpack('<f',data)[0]-old)>1e-3:
                raise ValueError(f'unexpected ultrawide value at {offset+EBOOT_BASE:#x}')
            data=struct.pack('<f',value)
            found.add(offset)
        elif len(data)==4 and data[0] in (0xB8,0xB9,0xBE) and data[1:]==old_width:
            # mov e?x, 2560 (the imm32's last byte is the next write's): the HUD canvas width.
            data=bytes([data[0]])+canvas.to_bytes(3,'little')
        out.append((offset,data))
    if found!=floats.keys():
        raise ValueError('ultrawide patch is missing camera/HUD values')
    return out


def resolution_writes(xml,size,app_version,segments,ui=OUTPUT_SIZE):
    writes=compile_patches(xml,[RESOLUTION_TEMPLATE],app_version,segments)
    replacements={SCENE_WIDTH:(0xB8,0x500,size[0]),
                  SCENE_HEIGHT:(0xB8,0x2D0,size[1]),
                  UI_WIDTH:(0xB8,0x500,ui[0]),
                  UI_HEIGHT:(0xB9,0x2D0,ui[1])}
    out=[]
    seen=set()
    for offset,data in writes:
        if offset in replacements:
            opcode,old,value=replacements[offset]
            if data!=bytes([opcode])+old.to_bytes(3,'little') or offset in seen:
                raise ValueError(f'unexpected resolution patch at {offset+EBOOT_BASE:#x}')
            data=bytes([opcode])+value.to_bytes(3,'little')
            seen.add(offset)
        out.append((offset,data))
    if seen!=replacements.keys():
        raise ValueError('resolution patch is missing scene/UI viewport instructions')
    return out


# Built-in frame-rate fix (Supermedo/bloodborne_pc#33 by Aljoscha-Ide; the same bytes as #31 by
# DUDUYG), applied with every preset above 30 FPS. After each physics step the player controller
# (0x1514500) checks for "blocked" movement: commanded speed > 5 m/s while the character moved
# less than 1/30 m this frame (vmulss 30.0 at 0x1514c24). The threshold is per frame, so it means
# 1 m/s only at 30 FPS; at 144 FPS it is 4.8 m/s, sprinting counts as blocked, the root motion
# scale (ctrl+0x1e0) drops to its floor of 0.5 and the halved speed keeps it there. The fix
# divides the moved distance by the frame time (dt, local -0x74(%rbp)) instead: "blocked" is
# then below 1 m/s at any frame rate. Off with sprint_fix=0 in bbport.ini or BB_SPRINT_FIX=0.
FRAME_RATE_FIXES=[
    # (vaddr, (length, site_check of the original vmulss), replacement)
    (0x1514c24,(8,'d84fc9844600213c'),bytes.fromhex('c5fa5e458c0f1f00')),  # vdivss -0x74(%rbp),%xmm0; nop
]


def sprint_fix_enabled(settings):
    return os.environ.get('BB_SPRINT_FIX',settings.get('sprint_fix','1'))!='0'


def frame_rate_fixes(elf):
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    loads=[]
    for i in range(phnum):
        kind,_,offset,vaddr,_,filesz,_,_=struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize)
        if kind==1: loads.append((vaddr,offset,filesz))
    writes=[]
    for vaddr,original,new in FRAME_RATE_FIXES:
        at=next((offset+vaddr-start for start,offset,filesz in loads if start<=vaddr<start+filesz),None)
        if at is None or site_check(elf[at:at+original[0]])!=original[1]:
            print(f'Patches: frame-rate fix at {vaddr:#x} skipped (unexpected code)')
            continue
        writes.append((vaddr,new))
    return writes


def eboot_segments(elf):
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    segments=[]
    for i in range(phnum):
        kind,_,_,vaddr,_,_,memsz,_=struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize)
        if kind==1: segments.append((vaddr,vaddr+memsz))
    return segments


def eboot_bytes(elf,start,end):
    """The eboot's memory image at [start, end): its PT_LOAD file bytes, zero past their file size."""
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    out=bytearray(end-start)
    for i in range(phnum):
        kind,_,offset,vaddr,_,filesz,_,_=struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize)
        low,high=max(start,vaddr),min(end,vaddr+filesz)
        if kind==1 and low<high: out[low-start:high-start]=elf[offset+low-vaddr:offset+high-vaddr]
    return bytes(out)


# The patches write bytes into the code of one build of the game. A game whose param.sfo says 01.09
# but whose eboot.bin holds other code (a base-game eboot beside an update's param.sfo, a modified
# eboot) got them written over other instructions and crashed at start (write fault at guest
# offset 0x20348b8). The fingerprint covers the game's original bytes everywhere the FPS and
# resolution patches write, so another region's build with the same code passes. Recompute it
# with --print-fingerprint (after a run of CUSA03173 v01.09) when those patches change.
# Ours is upstream's d9e505b value; upstream's later 34e41d1d... follows its XML's sites (the physics
# step at 0x00F383CA); both were checked on the same clean eboot (sha256 d65f0b4f...).
BUILTIN_XML=Path(__file__).resolve().parent.parent/'patches/Bloodborne.xml'
FINGERPRINT_PATCHES=sorted({n for names in FPS_PRESETS.values() for n in names}|{RESOLUTION_TEMPLATE})
PATCH_FINGERPRINTS={'01.09':'0c970cb2522ee7882ce59fec1b85715942873b82dd8e458b5c6a44bf7c09c116'}


def patch_sites(xml,app_version,names=FINGERPRINT_PATCHES):
    """Sorted, merged [start, end) eboot ranges the named patches write."""
    ranges=[]
    for meta in ET.parse(xml).getroot().iter('Metadata'):
        if meta.get('Name') not in names or meta.get('AppVer')!=app_version: continue
        for line in meta.iter('Line'):
            offset=int(line.get('Address'),0)-EBOOT_BASE
            ranges.append((offset,offset+len(encode(line))))
    merged=[]
    for start,end in sorted(ranges):
        if merged and start<=merged[-1][1]: merged[-1]=(merged[-1][0],max(merged[-1][1],end))
        else: merged.append((start,end))
    return merged


def patch_fingerprint(elf,xml=BUILTIN_XML,app_version='01.09',names=FINGERPRINT_PATCHES):
    digest=hashlib.sha256()
    for start,end in patch_sites(xml,app_version,names):
        digest.update(struct.pack('<QQ',start,end)+eboot_bytes(elf,start,end))
    return digest.hexdigest()


def eboot_matches_patches(elf_path,app_version='01.09'):
    """False when eboot.elf (prepare.py) differs from the build the patches were made for."""
    expected=PATCH_FINGERPRINTS.get(app_version)
    return not expected or patch_fingerprint(Path(elf_path).read_bytes(),BUILTIN_XML,app_version)==expected


def encode(line):
    kind,value=line.get('Type'),line.get('Value')
    if kind=='bytes': return bytes.fromhex(value.replace(' ',''))
    if kind in ('bytes16','bytes32','bytes64'):
        return int(value,0).to_bytes(int(kind[5:])//8,'little')
    if kind=='float32': return struct.pack('<f',float(value))
    if kind=='float64': return struct.pack('<d',float(value))
    if kind=='utf8': return value.encode()+b'\0'
    if kind=='utf16': return value.encode('utf-16-le')+b'\0\0'
    raise ValueError(f'unsupported patch type {kind!r}')


# Patches that replace code which the fingerprint above does not cover: the bytes they overwrite,
# as the 1.09 build has them (eboot offset, hex). Another build there gets the patch left out.
# name: (offset, length, site_check of the code the patch expects there)
PATCH_GUARDS={
    'Skip Online/Offline Choice':(0x1b39030,20,'2ba50af621aa1f80'),
}
# The same, by length and hash (sha256 prefix) of the code they expect, so no game bytes are kept:
# name -> (eboot offset, length, check).
HASH_GUARDS={
    'Circle Confirm (Japanese/Asian button layout)':(0x1fb8730,6,'f909b8f17e0b6494'),
}


# The game's graphics heaps at PC sizes (auser1337's "Increased Graphics Heap Sizes"; with the
# larger direct memory pool, BB_DMEM_MB 9152, which run.py sets with it). The PS4's sizes left models
# missing with the highest model detail or screen-space reflections (ladders, ground, the Astral
# Clocktower's clock). Awakenchan/BloodBorneButPc 0.9.32 gives every player and setting these
# sizes; before, the port applied them for scenes above 1080p only. pc_heaps=0 in bbport.ini or
# BB_PC_HEAPS=0: that again. The guard: (offset, length, sha256 prefix of the 1.09 build's bytes
# over both of the patch's writes).
HEAP_PATCH='Increased Graphics Heap Sizes'
HEAP_GUARD=(0x04b36dda-EBOOT_BASE,10,'2655d1cb82ac9da1')
# The game's GX range allocator (the GPU buffers the CPU writes each draw: per-draw shader
# parameters, CPU-deformed vertices) gets 160 MiB, its `mov dword [rbx+0x88], imm32` at
# GX_REGION_SITE holding the part past the first 32 MiB. Ebrietas' trail effect filled it right
# before her device losses, and two per-frame passes write through stale pointers when it is full
# (Awakenchan/BloodBorneButPc 0.9.37 beta 8, f029d89e; xiaodoudou's 2.2-community build).
# BB_GX_REGION_MB: 160-416, default 288; 160 or less keeps the game's. With the graphics heaps at
# the PS4 sizes, GFX_GraphicsPrivate (the heap table entry at GX_HEAP_SITE) grows by as much.
# Guards: (offset, length, sha256 prefix of the 1.09 build's bytes).
GX_REGION_GUARD=(0x1d9933d,10,'1927b84c46d6f004')
GX_HEAP_GUARD=(0x4736dd8,8,'07f614ee209aa02b')
GX_REGION_GAME,GX_REGION_DEFAULT,GX_REGION_MAX=160,288,416
GX_HEAP_PS4=0x8ef00000  # GFX_GraphicsPrivate's PS4 size, the value GX_HEAP_GUARD checks


def pc_heaps_enabled(settings):
    return setting_on(settings,'pc_heaps','BB_PC_HEAPS','1')


def site_matches(elf,guard):
    offset,length,check=guard
    return hashlib.sha256(eboot_bytes(elf,offset,offset+length)).hexdigest()[:16]==check


def gx_region_writes(elf,heaps_written,env=os.environ):
    """The writes that enlarge the GX range allocator, and GFX_GraphicsPrivate by as much when
    the graphics heaps are still at the PS4 size."""
    value=env.get('BB_GX_REGION_MB','').strip()
    mib=int(value) if value.isdigit() else GX_REGION_DEFAULT
    if mib<=GX_REGION_GAME:
        print(f"Patches: the GX range allocator at the game's {GX_REGION_GAME} MiB (BB_GX_REGION_MB)")
        return []
    mib=min(mib,GX_REGION_MAX)
    if not site_matches(elf,GX_REGION_GUARD) or (not heaps_written and not site_matches(elf,GX_HEAP_GUARD)):
        print("Patches: the GX range allocator left at the game's size: this eboot.bin differs there")
        return []
    offset,length,_=GX_REGION_GUARD
    out=[(offset+length-4,struct.pack('<I',(mib-GX_REGION_GAME+128)<<20))]
    if not heaps_written:
        out.append((GX_HEAP_GUARD[0],struct.pack('<Q',GX_HEAP_PS4+((mib-GX_REGION_GAME)<<20))))
    print(f"Patches: the GX range allocator {mib} MiB (the game's {GX_REGION_GAME})"+
          ('' if heaps_written else f', the graphics heap {(GX_HEAP_PS4>>20)+mib-GX_REGION_GAME} MiB'))
    return out


def guarded_patches(names,elf):
    """`names` without the guarded patches whose site in eboot.elf is not the code they expect."""
    kept=[]
    for name in names:
        if name in HASH_GUARDS and not site_matches(elf,HASH_GUARDS[name]):
            print(f'Patches: {name} left out: this eboot.bin has other code at its address')
            continue
        guard=PATCH_GUARDS.get(name)
        if guard:
            offset,length,expected=guard
            if site_check(eboot_bytes(elf,offset,offset+length))!=expected:
                print(f'Patches: {name} left out: this eboot.bin has other code at its address')
                continue
        kept.append(name)
    return kept


def compile_patches(xml, names, app_version, segments):
    found={}
    for meta in ET.parse(xml).getroot().iter('Metadata'):
        if meta.get('Name') in names and meta.get('AppVer')==app_version and meta.get('AppElf','eboot.bin')=='eboot.bin':
            found[meta.get('Name')]=meta
    missing=[n for n in names if n not in found]
    if missing: raise ValueError(f'patches not found for app version {app_version}: {missing}')
    writes=[]
    for name in names:
        for line in found[name].iter('Line'):
            offset=int(line.get('Address'),0)-EBOOT_BASE
            # The 1.09 FPS XML mistypes the physics-step address by 0x200000.
            # 0x011383ca lies inside an unrelated multi-byte NOP;
            # the intended float immediate is at 0x00f383ca in this build (1/30 in the game).
            # The values in patches/Bloodborne.xml for these presets are shadPS4's maintained
            # ones (upstream be1cde3): 1/90 at 90, 1/120 uncapped; 60 FPS keeps the game's 1/30.
            if (app_version == '01.09' and name in ('60 FPS++', '90 FPS++', 'Uncap FPS++')
                    and offset == 0x011383ca-EBOOT_BASE):
                offset = 0x00f383ca-EBOOT_BASE
            data=encode(line)
            if not any(start<=offset and offset+len(data)<=end for start,end in segments):
                raise ValueError(f'{name}: address {line.get("Address")} is outside the eboot')
            writes.append((offset,data))
    return writes


# Third-party patch files (shadPS4/GoldHEN XML) in the data directory's patches/ folder.
# Every retail release with update 1.09 (the same executable): US, EU, UK, JP, Asia, then the
# editions with The Old Hunters.
BLOODBORNE_IDS={'CUSA00900','CUSA00207','CUSA00208','CUSA00299','CUSA01363',
                'CUSA03179','CUSA03173','CUSA03014','CUSA03023'}


def external_patches(directory, app_version='01.09', exclude=Path(__file__).resolve().parent.parent/'patches/Bloodborne.xml'):
    """[(key, file, metadata)] of eboot patches for this version in directory/*.xml.
    key is "<file name>/<patch name>" (the launcher's selection, patches.json)."""
    found=[]
    directory=Path(directory)
    for path in sorted(directory.glob('*.xml')) if directory.is_dir() else []:
        if path.resolve()==Path(exclude).resolve(): continue  # the built-in file (patches/ in a checkout)
        # Copies of the built-in file in the data folder's patches/ (an older package's, or the
        # shadPS4 file of that name): their isEnabled frame-rate patches went on top of the
        # port's own (Uncap FPS++ over 90 FPS++). After Awakenchan/BloodBorneButPc.
        # external_bloodborne_xml=1 in bbport.ini or BB_EXTERNAL_BLOODBORNE_XML=1 reads them.
        if path.name.lower()==Path(exclude).name.lower() and not external_copy_allowed():
            print(f'Patches: {path} ignored: a copy of the built-in patch file '
                  '(BB_EXTERNAL_BLOODBORNE_XML=1 reads it)',file=sys.stderr)
            continue
        # Left by bbbridge's app (Supermedo's builds): play logs every 5 s to The Hunter's Dream.
        if path.name.lower()=='bbbridge-live-map.xml': continue
        try:
            root=ET.parse(path).getroot()
        except ET.ParseError as error:
            print(f'Patches: {path.name}: {error}',file=sys.stderr)
            continue
        ids={e.text.strip() for e in root.iter('ID') if e.text}
        if ids and not ids&BLOODBORNE_IDS: continue
        for meta in root.iter('Metadata'):
            if meta.get('AppVer')==app_version and meta.get('AppElf','eboot.bin')=='eboot.bin':
                found.append((f'{path.name}/{meta.get("Name")}',path,meta))
    return found


def external_copy_allowed():
    value=os.environ.get('BB_EXTERNAL_BLOODBORNE_XML')
    if value is None:
        config=os.environ.get('BB_CONFIG')
        value=read_settings(config).get('external_bloodborne_xml','0') if config else '0'
    return value=='1'


def external_selection(found, config):
    """Selected external patches: patches.json {"enabled": [...], "disabled": [...]} overrides
    each file's isEnabled."""
    settings={}
    if config and Path(config).is_file():
        settings=json.loads(Path(config).read_text(encoding='utf-8'))
    enabled,disabled=set(settings.get('enabled',[])),set(settings.get('disabled',[]))
    return [(key,path,meta) for key,path,meta in found
            if key in enabled or (key not in disabled and meta.get('isEnabled','false').lower()=='true')]


def compile_external(selected, segments):
    """Writes of the selected external patches; a patch with unsupported lines is skipped whole."""
    writes=[]
    for key,_,meta in selected:
        try:
            ours=[]
            for line in meta.iter('Line'):
                offset=int(line.get('Address') or '',0)-EBOOT_BASE
                data=encode(line)
                if not any(start<=offset and offset+len(data)<=end for start,end in segments):
                    raise ValueError(f'address {line.get("Address")} is outside the eboot')
                ours.append((offset,data))
        except ValueError as error:
            print(f'Patches: skipped {key}: {error}',file=sys.stderr)
            continue
        writes+=ours
        print(f'Patches: external {key} ({len(ours)} writes)')
    return writes


def with_external(writes, preset_count, selected, segments):
    """`writes` (the FPS preset's first, `preset_count` of them) and the selected external
    patches' after them, and the key of an external patch that replaced the preset, if any. An FPS
    patch of its own (e.g. "60FPS (no deltatime)", at 60 FPS++'s addresses with other values)
    replaces the built-in preset whole: applied over it, the two mixed. From ethan (emurphy04),
    Supermedo/bloodborne_pc PR #93 (8042500)."""
    preset_ranges=[(offset,offset+len(data)) for offset,data in writes[:preset_count]]
    external=[]
    replaced=None
    for item in selected:
        ours=compile_external([item],segments)
        if not replaced and preset_count and any(start<offset+len(data) and offset<end
                                                 for offset,data in ours for start,end in preset_ranges):
            writes=writes[preset_count:]
            replaced=item[0]
        external+=ours
    return writes+external,replaced


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--patches-dir',type=Path,help='third-party patch XML files (shadPS4 format)')
    p.add_argument('--patches-config',type=Path,help='patches.json: enabled/disabled external patches')
    p.add_argument('--xml',type=Path,default=Path(__file__).resolve().parent.parent/'patches/Bloodborne.xml')
    p.add_argument('--fps',choices=sorted(FPS_PRESETS)+sorted(SCALED_FPS),default='uncap')
    p.add_argument('--extra',default='',help='additional patch names, separated by ";"')
    p.add_argument('--app-version',default='01.09')
    p.add_argument('--out',type=Path,default=Path(__file__).resolve().parent.parent/'out')
    p.add_argument('--settings',type=Path,default=Path(__file__).resolve().parent.parent/'bbport.ini')
    p.add_argument('--game-dir',type=Path,default=Path(os.environ.get('BB_GAME_DIR','../CUSA03173')))
    p.add_argument('--render-res',default='',help='render resolution WxH (overrides the preset)')
    p.add_argument('--print-preset-size',action='store_true',help='print the selected preset size, if reduced')
    p.add_argument('--output-res',default='',help='output resolution WxH (centered 1920x1080 logical HUD stage)')
    p.add_argument('--print-scaled',action='store_true',
                   help='print "RENDER OUTPUT" (WxH) when bbport.ini selects an output other than 1080p')
    p.add_argument('--print-fingerprint',action='store_true',
                   help='print the patch fingerprint of <out>/eboot.elf (PATCH_FINGERPRINTS)')
    a=p.parse_args()
    if a.print_fingerprint:
        print(patch_fingerprint((a.out/'eboot.elf').read_bytes(),BUILTIN_XML,a.app_version))
        return
    if a.print_scaled:
        sizes=scaled_sizes(read_settings(a.settings))
        if sizes: print(f'{sizes[0][0]}x{sizes[0][1]} {sizes[1][0]}x{sizes[1][1]}')
        return
    if a.print_preset_size:
        settings=read_settings(a.settings)
        if 'BB_UPSCALER' in os.environ:
            settings['upscaler']='fsr3' if os.environ['BB_UPSCALER']=='fsr3' else 'off'
        if 'BB_UPSCALE_PRESET' in os.environ:
            settings['preset']=os.environ['BB_UPSCALE_PRESET']
        size=render_size(settings)
        if size: print(f'{size[0]}x{size[1]}')
        return
    # The patches are byte writes at the addresses of one game version: on another version they
    # would corrupt code. Such a game runs unpatched (run.sh/run.py then choose 30 FPS).
    version=game_app_version(a.game_dir)
    force=os.environ.get('BB_FORCE_PATCHES')
    if version and version!=a.app_version and not force:
        (a.out/'patches.bin').write_bytes(struct.pack('<8sQQ',b'BBPATCH2',EBOOT_BASE,0))
        print(f'Patches: game version {version}, patches are for {a.app_version}: none applied '
              '(30 FPS, no effect or resolution patches)')
        return
    if not force and not eboot_matches_patches(a.out/'eboot.elf',a.app_version):
        (a.out/'patches.bin').write_bytes(struct.pack('<8sQQ',b'BBPATCH2',EBOOT_BASE,0))
        print(f'Patches: eboot.bin is not the {a.app_version} build the patches were made for: none '
              'applied (30 FPS, no effect or resolution patches; BB_FORCE_PATCHES=1 applies them)')
        return
    elf=(a.out/'eboot.elf').read_bytes()
    preset=fps_preset(a.fps,elf,a.xml,a.app_version)
    names=preset+FIXES+[n.strip() for n in a.extra.split(';') if n.strip()]
    names+=[n for n in effect_patches(read_settings(a.settings)) if n not in names]
    names=validate_patch_requirements(online_allowed(names),a.game_dir)
    names=guarded_patches(names,elf)
    segments=eboot_segments(elf)
    writes=preset_writes(a.xml,preset,a.fps,a.app_version,segments,elf,read_settings(a.settings))
    preset_count=len(writes)  # the first writes, replaced whole by an external FPS patch
    writes+=compile_patches(a.xml,[n for n in names if n not in preset],a.app_version,segments)
    if a.fps!='30' and sprint_fix_enabled(read_settings(a.settings)):
        fixes=frame_rate_fixes(elf)
        writes+=fixes
        if fixes: names.append('Frame-rate fix: blocked-movement check')
    size=render_size(read_settings(a.settings),a.render_res) if a.render_res else None
    # Scaleform's logical stage remains 1920x1080. At non-16:9 output sizes,
    # give its show-all mode the actual physical viewport so it centers the stage.
    # The renderer uses that coordinate space for native-resolution UI draws.
    # Ultrawide outputs widen those coordinates instead (hud_canvas; run.py passes the same
    # size as BB_UI_RES) and get Kyo's 21:9 patch for their aspect ratio.
    ui=OUTPUT_SIZE
    out=output_size({'output_res': a.output_res}) if a.output_res else OUTPUT_SIZE
    if ultrawide(out): ui=hud_canvas(out)
    elif not is_16_9(out): ui=out
    if size:
        writes+=resolution_writes(a.xml,size,a.app_version,segments,ui)
        if ultrawide(out):
            writes+=ultrawide_writes(a.xml,out,a.app_version,segments)
            names.append(f'ultrawide {out[0]}x{out[1]} (HUD {ui[0]}x{ui[1]})')
        print(f'Patches: scene {size[0]}x{size[1]}; UI {ui[0]}x{ui[1]}')
    if size and size[0]*size[1]>OUTPUT_SIZE[0]*OUTPUT_SIZE[1] or pc_heaps_enabled(read_settings(a.settings)):
        if site_matches(elf,HEAP_GUARD):
            writes+=compile_patches(a.xml,[HEAP_PATCH],a.app_version,segments)
            names.append(HEAP_PATCH)
        else:
            print(f'Patches: {HEAP_PATCH} left out: this eboot.bin has other code at its address')
    if a.patches_dir:
        # After the built-in ones: an external patch of the same bytes wins.
        selected=external_selection(external_patches(a.patches_dir,a.app_version,a.xml),a.patches_config)
        allowed=online_allowed([key.split('/',1)[1] for key,_,_ in selected])
        selected=[item for item in selected if item[0].split('/',1)[1] in allowed]
        writes,replaced=with_external(writes,preset_count,selected,segments)
        if replaced:
            print(f'Patches: external {replaced} replaces the built-in {", ".join(preset)}')
            names=[n for n in names if n not in preset]
    heap=GX_HEAP_GUARD[0]
    writes+=gx_region_writes(elf,any(offset<heap+16 and offset+len(data)>heap for offset,data in writes))
    # BBPATCH2: the patch base, so the loader can rebase pointers the patches write into
    # relocated slots (60/90 FPS++ replace function pointers).
    blob=struct.pack('<8sQQ',b'BBPATCH2',EBOOT_BASE,len(writes))
    for offset,data in writes: blob+=struct.pack('<QQ',offset,len(data))+data
    (a.out/'patches.bin').write_bytes(blob)
    print(f'Patches: FPS preset {a.fps}; {len(writes)} writes from {names or "none"}')


if __name__=='__main__':
    main()
