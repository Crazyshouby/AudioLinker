"""Rebuilds THIRD-PARTY-NOTICES.txt (shipped with the installer) from the licence
files vendored in the repository. Run from anywhere: python tools/make_notices.py
"""
from pathlib import Path

root = Path(__file__).resolve().parent.parent

PARTS = [
    ("AudioLinker", "LICENSE",
     "AudioLinker est distribué sous licence MIT."),
    ("nlohmann/json (JSON for Modern C++)", "third_party/json/LICENSE.MIT",
     "Bibliothèque JSON intégrée au programme."),
    ("Microsoft Edge WebView2 SDK (chargeur statique)", "third_party/webview2/LICENSE.txt",
     "Le programme affiche son interface avec le runtime Microsoft Edge WebView2, installé séparément."),
    ("Microsoft Edge WebView2 SDK, mentions", "third_party/webview2/NOTICE.txt", ""),
    ("Archivo (police de l'interface)", "third_party/fonts/OFL-Archivo.txt",
     "Police intégrée à l'interface, sous SIL Open Font License 1.1."),
    ("Overpass Mono (police des consoles)", "third_party/fonts/OFL-OverpassMono.txt",
     "Police intégrée à l'interface, sous SIL Open Font License 1.1."),
]

out = ["MENTIONS LÉGALES ET LICENCES DES COMPOSANTS TIERS",
       "=" * 49,
       "",
       "AudioLinker inclut ou utilise les composants suivants. Leur texte de licence",
       "est reproduit tel quel ci-dessous.",
       "",
       "L'interface s'inspire visuellement du logiciel KDX (Haxial). Aucune ressource",
       "de KDX (image, police, fichier d'apparence) n'est reprise : tout est redessiné.",
       "VB-CABLE (VB-Audio) n'est pas fourni avec AudioLinker : il s'installe séparément.",
       ""]
for title, path, note in PARTS:
    text = (root / path).read_text(encoding="utf-8", errors="replace").strip()
    out += ["", "-" * 78, title, "-" * 78]
    if note:
        out += [note, ""]
    out += [text, ""]

(root / "THIRD-PARTY-NOTICES.txt").write_text("\r\n".join(out), encoding="utf-8-sig")
print("THIRD-PARTY-NOTICES.txt", (root / "THIRD-PARTY-NOTICES.txt").stat().st_size, "octets")
