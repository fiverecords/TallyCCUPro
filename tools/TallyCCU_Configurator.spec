# -*- mode: python ; coding: utf-8 -*-
# PyInstaller recipe for TallyCCU_Configurator.exe: one file, windowed, with the icon.
#
# Build on Windows (Python 3.12), from the repository root:
#   python -m pip install -r tools/requirements-build.txt
#   set PYTHONHASHSEED=0
#   python -m PyInstaller --clean --noconfirm tools/TallyCCU_Configurator.spec
# Result: dist/TallyCCU_Configurator.exe. The release workflow
# (.github/workflows/release.yml) builds it the same way and attaches it to the release.
# Pinned versions (requirements-build.txt) and PYTHONHASHSEED=0 keep the build repeatable;
# the .exe is not signed.
import os

HERE = os.path.dirname(os.path.abspath(SPEC))  # noqa: F821 (SPEC is set by PyInstaller)
ICON = os.path.join(HERE, 'tallyccu_icon.ico')

a = Analysis(  # noqa: F821
    [os.path.join(HERE, 'TallyCCU_Configurator.py')],
    pathex=[HERE],
    binaries=[],
    datas=[(ICON, '.')],  # resource_path('tallyccu_icon.ico') for the window icon
    hiddenimports=['serial.tools.list_ports'],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)  # noqa: F821

exe = EXE(  # noqa: F821
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='TallyCCU_Configurator',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=[ICON],
)
