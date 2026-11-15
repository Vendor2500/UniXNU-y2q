#!/usr/bin/env python3
"""SM-T970 changes to the PongMetal plugin, applied to the checked-out submodule at build time. Idempotent.

1. Accept the Adreno 650 as well as the A730: the A650 backend (drivers/T970Adreno/PongAdrenoCpT970.cpp) reports chip
   0x06050002 with 0x120000 bytes of GMEM; the plugin otherwise refuses every chip but 0x07030002/0x200000.
2. Turnip's debug options from /private/var/pong/tu_debug when that file exists (first line), else the plugin's own
   "sysmem,nocb": the A650 tuning (the same tablet's UniXNU-gts7xlwifi used "noubwc,nocb") without a rebuild.
"""
import sys
from pathlib import Path

EDITS = (
    ('info.chip_id!=0x07030002 || info.gmem_bytes!=0x200000 ||',
     '!((info.chip_id==0x07030002 && info.gmem_bytes==0x200000) ||\n'
     '         (info.chip_id==0x06050002 && info.gmem_bytes==0x120000)) || /* T970 A650 */'),
    ('    setenv("TU_DEBUG","sysmem,nocb",1);\n',
     '    {char tu[64]="sysmem,nocb";FILE *f=fopen("/private/var/pong/tu_debug","r");   /* T970: tuning without a rebuild */\n'
     '     if(f){if(fgets(tu,sizeof tu,f))tu[strcspn(tu,"\\r\\n")]=0;fclose(f);}\n'
     '     setenv("TU_DEBUG",tu,1);}\n'),
)


def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
    plugin = root / 'drivers/PongMetal/PongMetalPlugin.mm'
    text = plugin.read_text()
    for old, new in EDITS:
        if new in text:
            continue
        if text.count(old) != 1:
            raise SystemExit('expected exactly one %r in %s' % (old[:40], plugin))
        text = text.replace(old, new)
    plugin.write_text(text)
    print('patched:', plugin)


if __name__ == '__main__':
    main()
