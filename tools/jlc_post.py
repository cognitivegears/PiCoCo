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

def expand_refs(s):
    """'C4,C6-C10' -> 'C4,C6,C7,C8,C9,C10': JLCPCB does not expand KiCad's ranges."""
    out = []
    for part in s.split(","):
        part = part.strip()
        m = re.fullmatch(r"([A-Za-z]+)(\d+)-([A-Za-z]+)(\d+)", part)
        if m and m.group(1) == m.group(3):
            out += [f"{m.group(1)}{n}" for n in range(int(m.group(2)), int(m.group(4)) + 1)]
        else:
            out.append(part)
    return ",".join(out)


def bom(src, dst):
    rows = list(csv.DictReader(open(src, newline="")))
    with open(dst, "w", newline="") as f:
        w = csv.writer(f); w.writerow(["Comment", "Designator", "Footprint", "LCSC Part #", "MPN"])
        for r in rows:
            w.writerow([r["Value"], expand_refs(r["Reference"]), r["Footprint"].split(":")[-1], r.get("LCSC", ""), r.get("MPN", "")])

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
    # Every other footprint loses its paste: this stencil is used on a board JLCPCB has
    # already assembled, so it must carry apertures for U1's 55 pads and nothing else.
    strip = lambda s: s.replace('(layers "F.Cu" "F.Mask" "F.Paste")', '(layers "F.Cu" "F.Mask")')
    open(dst, "w").write(strip(text[:start]) + fp2 + strip(text[j:]))

if __name__ == "__main__":
    {"bom": bom, "cpl": cpl, "stencil": stencil}[sys.argv[1]](sys.argv[2], sys.argv[3])
