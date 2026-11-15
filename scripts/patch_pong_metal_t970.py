#!/usr/bin/env python3
"""Let the PongMetal plugin accept the SM-T970 Adreno 650 as well as the A730.

drivers/PongMetal is an upstream submodule, so the change is applied to the
checked-out source at build time (like the other *_t970.py patchers). The
A650 backend (drivers/T970Adreno/PongAdrenoCpT970.cpp) reports chip 0x06050002
with 0x120000 bytes of GMEM; the plugin otherwise refuses every chip but
0x07030002/0x200000. Idempotent.
"""
import sys
from pathlib import Path

OLD = 'info.chip_id!=0x07030002 || info.gmem_bytes!=0x200000 ||'
NEW = ('!((info.chip_id==0x07030002 && info.gmem_bytes==0x200000) ||\n'
       '         (info.chip_id==0x06050002 && info.gmem_bytes==0x120000)) || /* T970 A650 */')


def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
    plugin = root / 'drivers/PongMetal/PongMetalPlugin.mm'
    text = plugin.read_text()
    if NEW in text:
        print('already patched:', plugin)
        return
    if text.count(OLD) != 1:
        raise SystemExit('expected exactly one A730 chip check in ' + str(plugin))
    plugin.write_text(text.replace(OLD, NEW))
    print('patched:', plugin)


if __name__ == '__main__':
    main()
