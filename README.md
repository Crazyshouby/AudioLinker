# AudioLinker

[![Build](https://github.com/Crazyshouby/AudioLinker/actions/workflows/build.yml/badge.svg)](https://github.com/Crazyshouby/AudioLinker/actions/workflows/build.yml)

Application Windows pour diffuser l'audio du PC sur **plusieurs sorties simultanément** (enceintes, casques, Bluetooth, HDMI…), avec latence, volume, égaliseur et canal réglables par sortie — le tout synchronisé.

## Téléchargement

L'exécutable prêt à l'emploi est publié dans les [Releases](https://github.com/Crazyshouby/AudioLinker/releases) (chaque version taguée `v*` déclenche une compilation automatique). Sinon, voir [Compilation](#compilation) ci-dessous.

## Fonctionnalités

- **Groupe multi-enceintes** : capture système (via câble virtuel) ou **capture par application** (Process Loopback) redistribuée vers autant de sorties que voulu.
- **Synchronisation** : latence ajustable par sortie (0–500 ms), correction de dérive d'horloge continue (resampler ±0,15 %, inaudible), reconnexion automatique des sorties perdues (Bluetooth qui décroche…).
- **Latence maîtrisée** : marge et tampon de sortie réglables, mode basse latence (période audio minimale `IAudioClient3`), et **optimiseur automatique** qui trouve le réglage le plus bas sans craquement sur votre machine.
- **Calage automatique au micro** : joue un balayage sonore sur chaque enceinte, mesure son retard acoustique réel avec un micro placé à la position d'écoute (corrélation croisée), et aligne toutes les latences sur l'enceinte la plus lente.
- **Par sortie** : volume, mute, canal (stéréo / gauche / droite), égaliseur 5 bandes avec spectre temps réel, VU-mètres.
- **Setups** : configurations nommées, applicables via l'UI, le menu tray, un switcher global (`Ctrl+Alt+S`) ou `Ctrl+Maj+1-9`.
- **Intégration Windows** : icône de zone de notification, raccourcis globaux personnalisables, thème clair/sombre, couleur d'accentuation système, démarrage avec Windows, DPI par moniteur.

## Prérequis

- Windows 10 2004+ ou Windows 11 (x64)
- [VB-CABLE](https://vb-audio.com/Cable/) — le câble audio virtuel qui sert de point de capture système
- [WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/) (préinstallé sur Windows 11)

## Compilation

Outils : Visual Studio Build Tools 2022 (MSVC x64), CMake ≥ 3.20, Ninja.

```bat
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

L'exécutable est autonome (`build\AudioLinker.exe`) : les pages de l'interface (WebView2) sont embarquées en ressources, le SDK WebView2 est vendoré dans `third_party/`.

## Architecture

- `src/AudioEngine.cpp` — moteur WASAPI : un thread de capture par source (système ou application), un thread de rendu événementiel par sortie, ring buffers SPSC lock-free, resampler avec correction de dérive (P-controller), EQ biquad.
- `src/Calibrator.cpp` — calage micro : balayage sinusoïdal horodaté (QPC), détection par corrélation croisée.
- `src/Gui.cpp` — fenêtre Win32 + WebView2 (`assets/ui.html`), tray, hotkeys, routage par application (endpoints), persistance INI (`%APPDATA%\AudioLinker.ini`).
- `src/DeviceManager.cpp` — énumération MMDevice et notifications de branchement.

## Licence

[MIT](LICENSE). Le SDK WebView2 vendoré reste soumis à sa propre licence (`third_party/webview2/LICENSE.txt`).
