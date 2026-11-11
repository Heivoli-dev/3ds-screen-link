"""
Écrit pc/version_info.txt (infos du .exe : nom du produit, version...) à partir
de VERSION dans pc/screenlink_pc.py. Utilisé par PyInstaller (--version-file).
"""
import os
import re

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
src = open(os.path.join(ROOT, "pc", "screenlink_pc.py"), encoding="utf-8").read()
version = re.search(r'^VERSION = "([0-9.]+)"', src, re.M).group(1)
nums = tuple(int(x) for x in (version.split(".") + ["0", "0", "0"])[:4])

info = f"""VSVersionInfo(
  ffi=FixedFileInfo(filevers={nums}, prodvers={nums}, mask=0x3f, flags=0x0,
                    OS=0x40004, fileType=0x1, subtype=0x0, date=(0, 0)),
  kids=[
    StringFileInfo([StringTable('040C04B0', [
      StringStruct('CompanyName', 'Heivoli-dev'),
      StringStruct('FileDescription', '3DS Screen Link'),
      StringStruct('FileVersion', '{version}'),
      StringStruct('InternalName', 'ScreenLinkPC'),
      StringStruct('LegalCopyright', 'Copyright (c) 2026 Heivoli-dev - MIT License'),
      StringStruct('OriginalFilename', 'ScreenLinkPC.exe'),
      StringStruct('ProductName', '3DS Screen Link'),
      StringStruct('ProductVersion', '{version}')])]),
    VarFileInfo([VarStruct('Translation', [1036, 1200])])
  ]
)
"""
with open(os.path.join(ROOT, "pc", "version_info.txt"), "w", encoding="utf-8") as f:
    f.write(info)
print("version_info.txt :", version)
