# AudioLinker

[![Build](https://github.com/Crazyshouby/AudioLinker/actions/workflows/build.yml/badge.svg)](https://github.com/Crazyshouby/AudioLinker/actions/workflows/build.yml)

Application Windows pour diffuser l'audio du PC sur **plusieurs sorties simultanément** (enceintes, casques, Bluetooth, HDMI…), avec latence, volume, égaliseur et canal réglables par sortie — le tout synchronisé.

## Téléchargement

L'exécutable prêt à l'emploi est publié dans les [Releases](https://github.com/Crazyshouby/AudioLinker/releases) (chaque version taguée `v*` déclenche une compilation automatique). Sinon, voir [Compilation](#compilation) ci-dessous.

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
- [WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/) (préinstallé sur Windows 11)

## Configuration

Tout est dans `%APPDATA%\AudioLinker\config.json` (setups, enceintes, raccourcis, réglages, règles d'automatisation). Un ancien `%APPDATA%\AudioLinker.ini` est migré automatiquement au premier lancement et conservé en `.bak`. Export/import depuis la fenêtre Options (un `.json`, ou un ancien `.ini`). Le journal de diagnostic est dans `%LOCALAPPDATA%\AudioLinker\audiolinker.log`.

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

## Architecture

- `src/AudioEngine.cpp` — moteur WASAPI : un thread de capture par source (système ou application), un thread de rendu événementiel par sortie, ring buffers SPSC lock-free alignés sur les trames, resampler Hermite avec correction de dérive (P-controller), EQ biquad avant le gain, soft-clip, spectre à la demande.
- `src/WasapiStream.cpp` — ouverture d'un flux partagé, négociation de format, conversions, période basse latence ; partagé par le moteur et le calibrateur.
- `src/Calibrator.cpp` — calage micro : balayage sinusoïdal horodaté (QPC), détection par corrélation croisée.
- `src/Config.cpp` — configuration JSON, migration INI, purge des périphériques fantômes.
- `src/App.h` + `Gui.cpp`, `Group.cpp`, `Devices.cpp`, `SystemAudio.cpp`, `WebBridge.cpp`, `Hotkeys.cpp`, `Switcher.cpp`, `Optimizer.cpp`, `OptionsWindow.cpp`, `Tray.cpp` — interface Win32 + WebView2 (`assets/ui.html`, `assets/options.html`, parties communes `assets/common.*` inlinées au build), setups, routage par application, tray, hotkeys.
- `src/DeviceManager.cpp` — énumération MMDevice et notifications de branchement.
- `tests/` — tests unitaires (ring buffer, resampler, biquad, config) lancés par `ctest` et par la CI.

## Style graphique

L'interface reprend le thème de KDX (Haxial) : fenêtres à barre de titre sur fond rouge, panneaux gris anthracite, boutons biseautés, consoles noires à texte vert, fenêtre active cerclée de rouge, pictogrammes pixel-art. Il n'y a qu'un thème (sombre) : le réglage clair/sombre et la couleur d'accentuation Windows ont disparu.

- `assets/common.css` : jetons de couleur et widgets partagés par les deux pages (valeurs relevées sur une capture de KDX).
- `assets/fonts.css`, `assets/icons.js`, `assets/lava.css` : ressources **générées** (polices Archivo et Overpass Mono en base64, pictogrammes, fond). Elles sont versionnées ; le build n'exécute pas Python. Pour les régénérer : `python tools/gen_assets.py` (`--lava` pour refaire le fond, avec Chrome et Pillow). Les pictogrammes sont dessinés dans `tools/pixel_icons.py`.

## Licence

[MIT](LICENSE). Le SDK WebView2 vendoré reste soumis à sa propre licence (`third_party/webview2/LICENSE.txt`), nlohmann/json à la sienne (`third_party/json/LICENSE.MIT`). Les polices Archivo et Overpass Mono sont sous licence SIL OFL 1.1 (`third_party/fonts/`).
