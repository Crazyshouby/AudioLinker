# AudioLinker

[![Build](https://github.com/Crazyshouby/AudioLinker/actions/workflows/build.yml/badge.svg)](https://github.com/Crazyshouby/AudioLinker/actions/workflows/build.yml)

Application Windows pour diffuser l'audio du PC sur **plusieurs sorties simultanément** (enceintes, casques, Bluetooth, HDMI…), avec latence, volume, égaliseur et canal réglables par sortie — le tout synchronisé.

## Téléchargement

Dans les [Releases](https://github.com/Crazyshouby/AudioLinker/releases) (chaque version taguée `v*` déclenche une compilation automatique) :

- **`AudioLinker-Setup-<version>.exe`** : l'installateur (recommandé). Il s'installe pour l'utilisateur courant, sans droits administrateur, crée les raccourcis, propose le démarrage avec Windows, installe le runtime WebView2 s'il manque, et se désinstalle proprement depuis « Applications installées ».
- `AudioLinker-<version>-portable.exe` : l'exécutable seul, à lancer depuis n'importe quel dossier.
- Les empreintes SHA-256 sont dans le fichier `.sha256` de la release.

Tant que l'installateur n'est pas signé numériquement, SmartScreen peut afficher « Windows a protégé votre ordinateur » : cliquez sur « Informations complémentaires » puis « Exécuter quand même », après avoir vérifié l'empreinte SHA-256. Sinon, voir [Compilation](#compilation) ci-dessous.

## Fonctionnalités

- **Groupe multi-enceintes** : capture système (via câble virtuel) ou **capture par application** (Process Loopback) redistribuée vers autant de sorties que voulu.
- **Synchronisation** : latence ajustable par sortie (0–500 ms), correction de dérive d'horloge continue (resampler ±0,15 %, inaudible), reconnexion automatique des sorties perdues (Bluetooth qui décroche…), retard mesuré par le périphérique affiché pour chaque enceinte.
- **Latence maîtrisée** : marge et tampon de sortie réglables, mode basse latence (période audio minimale `IAudioClient3`), et **optimiseur automatique** qui trouve le réglage le plus bas sans craquement sur votre machine.
- **Calage automatique au micro** : joue un balayage sonore sur chaque enceinte, mesure son retard acoustique réel avec un micro placé à la position d'écoute (corrélation croisée), et aligne toutes les latences sur l'enceinte la plus lente.
- **Par sortie** : volume, mute, canal (stéréo / gauche / droite), égaliseur 5 bandes avec spectre temps réel, VU-mètres, fiche qui s'ouvre sous l'enceinte dans la liste.
- **Setups** : configurations nommées, applicables via l'UI, le menu tray, un switcher global (`Ctrl+Alt+S`) ou `Ctrl+Maj+1-9` (désactivable). Basculer entre deux setups bâtis sur les mêmes enceintes se fait **sans coupure**.
- **Automatisation** : par enceinte, « quand elle se connecte, jouer tel setup » et « quand elle se déconnecte, arrêter le groupe ».
- **Entrées** : micro par défaut Windows, niveau et mute mémorisés par setup, VU-mètres.
- **Intégration Windows** : icône de zone de notification (verte quand le groupe joue, notifications quand la fenêtre est cachée), muet global, raccourcis globaux personnalisables, style KDX sombre, démarrage avec Windows, DPI par moniteur, reprise après veille, surveillance de la sortie par défaut et réparation automatique après un arrêt brutal.

## Prérequis

- Windows 10 2004+ ou Windows 11 (x64)
- [VB-CABLE](https://vb-audio.com/Cable/) — le câble audio virtuel qui sert de point de capture système (sauf en mode sans câble, voir ci-dessous)
- [WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/) (préinstallé sur Windows 11 ; l'installateur le télécharge sinon)

## Configuration

Tout est dans `%APPDATA%\AudioLinker\config.json` (setups, enceintes, raccourcis, réglages, règles d'automatisation). Un ancien `%APPDATA%\AudioLinker.ini` est migré automatiquement au premier lancement et conservé en `.bak`. Export/import depuis la fenêtre Options (un `.json`, ou un ancien `.ini`). Le journal de diagnostic est dans `%LOCALAPPDATA%\AudioLinker\audiolinker.log` (il s'affiche aussi en direct dans la fenêtre Journal). En cas de plantage, un rapport `.dmp` est écrit dans `%LOCALAPPDATA%\AudioLinker\crash\` ; il reste sur votre machine, à joindre à un signalement si vous le souhaitez.

`AudioLinker.exe --quit` arrête proprement l'instance en cours (groupe arrêté, sortie par défaut de Windows restaurée).

## Mode sans câble (expérimental)

Dans Options → Capture du son, le mode **Sans câble** capture tout le système par process loopback (toutes les applications sauf AudioLinker) sans périphérique virtuel. La sortie par défaut de Windows continue alors de jouer en natif : elle est retirée du groupe et sert de **référence de temps**.

Limite physique : le groupe entend le son après tout le pipeline (capture + marge + tampon, soit ~100 ms au mieux en basse latence), la sortie native non. La synchronisation n'est donc possible que si la sortie par défaut a elle-même plus de latence que ce pipeline (par exemple défaut = enceinte Bluetooth, groupe = sorties filaires). Le **calage micro** mesure cet écart et l'affiche : s'il annonce une avance que le groupe ne peut pas rattraper, revenez au câble virtuel pour cette configuration. Pas de sources par application dans ce mode (l'application jouerait deux fois).

## Compilation

Outils : Visual Studio Build Tools 2022 (MSVC x64), CMake ≥ 3.20, Ninja.

```bat
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

`build.cmd [tests]` fait la même chose dans `build-dev\` (pratique quand une instance tourne depuis `build\`). L'exécutable est autonome (`build\AudioLinker.exe`) : les pages de l'interface (WebView2) sont embarquées en ressources, le SDK WebView2 et nlohmann/json sont vendorés dans `third_party/`. La version est définie une seule fois, dans `project(VERSION …)` de `CMakeLists.txt`.

**Installateur** : avec [Inno Setup 6](https://jrsoftware.org/isinfo.php) installé, `.\installer\build.ps1 -ExePath build\AudioLinker.exe` produit `installer\Output\AudioLinker-Setup-<version>.exe` et son SHA-256. La procédure complète de publication (recette, tag, signature de code) est dans [RELEASING.md](RELEASING.md).

## Architecture

- `src/AudioEngine.cpp` — moteur WASAPI : un thread de capture par source (système ou application), un thread de rendu événementiel par sortie, ring buffers SPSC lock-free alignés sur les trames, resampler Hermite avec correction de dérive (P-controller), EQ biquad avant le gain, soft-clip, spectre à la demande.
- `src/WasapiStream.cpp` — ouverture d'un flux partagé, négociation de format, conversions, période basse latence ; partagé par le moteur et le calibrateur.
- `src/Calibrator.cpp` — calage micro : balayage sinusoïdal horodaté (QPC), détection par corrélation croisée.
- `src/Config.cpp` — configuration JSON, migration INI, purge des périphériques fantômes.
- `src/App.h` + `Gui.cpp`, `Group.cpp`, `Devices.cpp`, `SystemAudio.cpp`, `WebBridge.cpp`, `Hotkeys.cpp`, `Switcher.cpp`, `Optimizer.cpp`, `Tray.cpp` — interface Win32 + WebView2 (`assets/ui.html`, `assets/options.html`, parties communes `assets/common.*` inlinées au build), setups, routage par application, tray, hotkeys.
- `src/Panels.cpp` — les fenêtres de l'interface : le menu principal (`Gui.cpp`) et une fenêtre sans bordure par fonction (sorties, setups, niveaux, journal, options), chacune avec son WebView2. La barre de titre est dessinée par la page ; le natif déplace, redimensionne, enroule et ferme à sa demande, et mémorise position, taille et ouverture de chaque fenêtre. `src/Journal.cpp` alimente la fenêtre Journal (le journal de diagnostic en direct).
- `src/DeviceManager.cpp` — énumération MMDevice et notifications de branchement.
- `tests/` — tests unitaires (ring buffer, resampler, biquad, config) lancés par `ctest` et par la CI.

## Style graphique

L'interface reprend le thème et l'organisation de KDX (Haxial) : un menu principal étroit (colonne de boutons) et une vraie fenêtre par fonction (Sorties avec la fiche enceinte, Setups, Niveaux, Journal, Options), déplaçables, redimensionnables et enroulables par la barre de titre. Panneaux gris anthracite, boutons biseautés, consoles noires à texte vert, fenêtre active cerclée de rouge, pictogrammes pixel-art. Réduire le menu principal (« − ») range tout dans la zone de notification ; « Quitter » arrête l'application. Il n'y a qu'un thème (sombre) : le réglage clair/sombre et la couleur d'accentuation Windows ont disparu.

- `assets/common.css` : jetons de couleur et widgets partagés par les deux pages. Les valeurs viennent de l'apparence par défaut de KDX, relevées pixel par pixel sur les captures sans perte de sa documentation (fenêtre active rouge, barres de titre en dégradé, relief des boutons, rangées bleu marine et pêche). Aucun fichier de KDX n'est repris : tout est redessiné.
- `assets/fonts.css`, `assets/icons.js` : ressources **générées** (polices Archivo et Overpass Mono en base64, pictogrammes). Elles sont versionnées ; le build n'exécute pas Python. Pour les régénérer : `python tools/gen_assets.py`. Les pictogrammes sont dessinés dans `tools/pixel_icons.py`.

## Licence

[MIT](LICENSE). Le SDK WebView2 vendoré reste soumis à sa propre licence (`third_party/webview2/LICENSE.txt`), nlohmann/json à la sienne (`third_party/json/LICENSE.MIT`). Les polices Archivo et Overpass Mono sont sous licence SIL OFL 1.1 (`third_party/fonts/`).
