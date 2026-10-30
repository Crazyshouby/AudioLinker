# Changelog

Le format suit [Keep a Changelog](https://keepachangelog.com/fr/1.1.0/) ; les versions suivent [SemVer](https://semver.org/lang/fr/).

## [Non publié]

### Modifié
- **Notifications dans le centre rapide du shell KDX** : les erreurs et avertissements du groupe y sont aussi envoyés (une même ligne n'est pas répétée avant 10 s). Sans shell KDX, rien ne change.
- **Accent de la suite KDX** : AudioLinker suit l'accent de `%APPDATA%\KDX\suite.json` (relu à chaud) et recolore ses fenêtres sans redémarrer.
- **Liaison avec les autres applications KDX** (tube `\\.\pipe\kdx.audiolinker`, utilisateur courant seulement) : la barre du shell KDX affiche le groupe (voyant, coupure, volume, setups) tant qu'AudioLinker tourne. Messages `audio.get`, `audio.toggle`, `audio.volume`, `audio.mute`, `audio.setup` ; AudioLinker répond `audio.state`.
- **Apparence : charte KDX-UI** (dépôt `kdx-ui`, copiée dans `kdx/`). Police pixel unique à la place d'Archivo et d'Overpass Mono (une seule taille, aucun gras), plus aucun arrondi ni dégradé hors jetons, cadre, relief et barre de titre mesurés sur KDX 1.6, rangées et sélections de la charte, voyants (`.orb`) à la place des pastilles et du spinner, cases à cocher biseautées. L'exécutable est plus léger (les pages embarquées passent de 284 Ko et 195 Ko à 163 Ko et 80 Ko). Le moteur audio et les tailles de fenêtres natives ne changent pas. **Ne pas publier avant la décision sur le nom « KDX » et la police** (`THIRD-PARTY-NOTICES.txt` mentionne désormais que la police reproduit celle de KDX).
- `tools/gen_assets.py` ne génère plus que `assets/icons.js` ; les polices viennent de la charte.

### Ajouté
- Intégration de la signature de code SignPath dans la CI (inerte tant que SignPath n'est pas configuré), page de politique de signature de code, en-tête de release généré (installation, vérification, état de la signature).

## [1.2.0] — 2026-10-28

Première version publiée (la 1.1.0 n'a existé que pendant le développement). Elle regroupe le moteur multi-sorties WASAPI, la correction de dérive d'horloge, le calage micro, l'égaliseur par sortie, les setups, les raccourcis globaux et la zone de notification, plus ce qui suit.

### Ajouté
- **Interface en plusieurs fenêtres, dans le style de KDX (Haxial)** : un menu principal étroit (colonne de boutons) et une vraie fenêtre sans bordure par fonction — Sorties avec la fiche enceinte, Setups, Niveaux, Journal, Options. Déplaçables, redimensionnables et enroulables (barre de titre dessinée, double-clic, poignée), avec mémoire de la position, de la taille et de l'état de chaque fenêtre. La fenêtre active est rouge.
- **Fenêtre Niveaux** : VU-mètre et état de chaque sortie cochée.
- **Fenêtre Journal** : le journal de diagnostic en direct, avec une pastille orange dans le menu principal quand un avertissement arrive fenêtre fermée.
- **Installateur Windows** (`AudioLinker-Setup-<version>.exe`, par utilisateur, sans droits administrateur) : raccourcis, démarrage avec Windows en option, vérification et installation du runtime WebView2, arrêt propre d'une instance en cours lors d'une mise à jour, désinstallation propre avec choix de conserver ou supprimer les réglages.
- Option de ligne de commande `--quit` : arrête proprement l'instance en cours (groupe arrêté, sortie par défaut de Windows restaurée).
- Rapport de plantage : un fichier `.dmp` est écrit dans `%LOCALAPPDATA%\AudioLinker\crash\` (les 5 derniers sont conservés), sans rien envoyer.
- Mode « sans câble » (expérimental), setups appliqués à chaud, automatisation à la connexion d'une enceinte, calage micro en basse latence.

### Modifié
- Un seul thème (sombre) : le réglage clair/sombre et la couleur d'accentuation Windows disparaissent. Polices Archivo et Overpass Mono et pictogrammes pixel-art embarqués.
- Fermer ou réduire le menu principal (« − ») range l'application dans la zone de notification ; « Quitter » l'arrête.
- L'exécutable n'a plus besoin du redistribuable Visual C++ (runtime statique) et est compilé avec les protections `/guard:cf`, ASLR et DEP.
- L'interface web ne peut plus naviguer hors de ses pages (liens et fenêtres externes bloqués), DevTools et raccourcis de navigateur désactivés.

### Corrigé
- Rafraîchissement figé après l'ouverture d'une fiche, ligne de setup, changement de setup.
