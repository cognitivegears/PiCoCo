#!/usr/bin/env python3
"""Post-process kicad-cli outputs into JLCPCB's BOM and CPL formats.
usage: jlc_post.py bom  in.csv out.csv
       jlc_post.py cpl  in.csv out.csv
       jlc_post.py stencil in.kicad_pcb out.kicad_pcb   # give U1's pads an F.Paste layer
"""
import csv, re, sys

# Rotation offsets (degrees, added to KiCad's rotation) so JLCPCB's pick-and-place preview
# shows pin 1 where the footprint has it. Not yet verified against the JLCPCB preview; Task 8
# step 2 fills in offsets and records the verification date in this comment.
ROT = {"SOIC-20W": 0, "SOIC-14": 0, "SOT-23": 0, "SOT-223": 0, "D_SMA": 0, "0805": 0, "CP_Elec": 0}

def bom(src, dst):
    rows = list(csv.DictReader(open(src, newline="")))
    with open(dst, "w", newline="") as f:
        w = csv.writer(f); w.writerow(["Comment", "Designator", "Footprint", "LCSC Part #", "MPN"])
        for r in rows:
            w.writerow([r["Value"], r["Reference"], r["Footprint"].split(":")[-1], r.get("LCSC", ""), r.get("MPN", "")])

def cpl(src, dst):
    rows = list(csv.DictReader(open(src, newline="")))
    with open(dst, "w", newline="") as f:
        w = csv.writer(f); w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
        for r in rows:
            if r["Ref"].startswith("FID"):  # fiducials are not placed parts; JLC flags them as unmatched
                continue
            off = next((v for k, v in ROT.items() if k in r["Package"]), 0)
            rot = (float(r["Rot"]) + off) % 360
            w.writerow([r["Ref"], f'{float(r["PosX"]):.3f}mm', f'{float(r["PosY"]):.3f}mm',
                        "Top" if r["Side"].lower() in ("top", "front") else "Bottom", f"{rot:.0f}"])

def stencil(src, dst):
    text = open(src).read()
    i = text.index('(property "Reference" "U1"')
    start = text.rfind("(footprint ", 0, i)
    depth, j = 0, start
    while True:
        c = text[j]; depth += (c == "(") - (c == ")"); j += 1
        if depth == 0: break
    fp = text[start:j]
    fp2 = re.sub(r'\(layers "F\.Cu" "F\.Mask"\)', '(layers "F.Cu" "F.Paste" "F.Mask")', fp)
    open(dst, "w").write(text[:start] + fp2 + text[j:])

if __name__ == "__main__":
    {"bom": bom, "cpl": cpl, "stencil": stencil}[sys.argv[1]](sys.argv[2], sys.argv[3])
