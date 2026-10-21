#!/usr/bin/env python3
"""Add the shader variants a console log says were compiled in the background to dante/dksh_variants.txt.

    python3 dante/tools/dksh_variants.py <logs folder or .log files>...

The renderer logs each one once per session ("VS n891 00000000 0000001F no esta en la cache ... (huella ...)"). The list
then goes into the DKSH cache the next time tools/shaders.sh runs (build_mac.sh SHADERS=1).
"""
import re
import sys
from pathlib import Path

LINE = re.compile(r"\[deko3d\] (?:VS|PS) n\d+ ([0-9A-F]{8}) ([0-9A-F]{8}) no esta en la cache.*\(huella ([0-9A-F]{16})\)")
lista = Path(__file__).resolve().parent.parent / "dksh_variants.txt"
texto = lista.read_text()
existentes = {l.split()[0:3].__str__() for l in texto.splitlines() if l and not l.startswith("#")}
nuevas = []
for arg in sys.argv[1:]:
    p = Path(arg)
    for log in sorted(p.rglob("*.log")) if p.is_dir() else [p]:
        for m in LINE.finditer(log.read_text(errors="replace")):
            clave = [m.group(3).lower(), m.group(1).lower(), m.group(2).lower()]
            if str(clave) not in existentes:
                existentes.add(str(clave))
                nuevas.append(" ".join(clave))
if nuevas:
    lista.write_text(texto.rstrip("\n") + "\n" + "\n".join(nuevas) + "\n")
print(f"{len(nuevas)} new variants -> {lista}")
