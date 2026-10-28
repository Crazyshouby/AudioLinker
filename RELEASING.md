# Publier une version

La version vit à **un seul endroit** : `project(AudioLinker VERSION x.y.z …)` dans `CMakeLists.txt`. L'installateur, la ressource de version de l'exécutable et la CI la lisent de là. La CI refuse un tag qui ne correspond pas.

## 1. Préparer

1. Mettre `VERSION` à jour dans `CMakeLists.txt` (SemVer : `x.y.z`).
2. Dans `CHANGELOG.md`, remplacer « à publier » par la date et vérifier la liste.
3. Si des polices, pictogrammes ou licences ont changé : `python tools/gen_assets.py` et `python tools/make_notices.py`, puis commiter (la CI vérifie que ces fichiers sont à jour).
4. Fusionner dans `main` par pull request ; la CI doit être verte : compilation, 4 tests unitaires, test d'interface, installateur, test d'installation / mise à jour / désinstallation.

## 2. Recette manuelle (sur l'installateur de la CI, pas sur un build local)

Télécharger l'artefact `AudioLinker-installer` de la CI de `main`, puis sur une machine Windows :

- [ ] Installation (assistant en français), lancement depuis le menu Démarrer.
- [ ] Les fenêtres s'ouvrent avec leur contenu ; déplacement, redimensionnement, enroulement, fermeture ; le menu principal passe devant au clic.
- [ ] Démarrer / arrêter un groupe de 2 sorties ou plus ; la sortie par défaut de Windows est restaurée à l'arrêt.
- [ ] Setups : enregistrer, jouer, changer à chaud ; `Ctrl+Alt+S`.
- [ ] Calage micro, onglet Entrées, Options (démarrage avec Windows, raccourcis), Journal.
- [ ] « − » du menu principal : tout est rangé dans la zone de notification ; un clic sur l'icône restaure.
- [ ] `--tray` (démarrage de Windows) : aucune fenêtre au démarrage.
- [ ] Mise à jour par-dessus l'ancienne version avec l'application ouverte : elle est arrêtée proprement, les réglages sont conservés.
- [ ] Désinstallation : programmes, raccourcis, entrée de démarrage retirés ; réglages conservés ou supprimés selon la réponse.
- [ ] Un écran à échelle d'affichage différente de 100 %, et deux écrans.

## 3. Publier

```
git tag v1.2.0          # v1.2.0-rc1 pour une préversion
git push origin v1.2.0
```

La CI construit, teste et publie la release GitHub avec :

- `AudioLinker-Setup-<version>.exe` : l'installateur ;
- `AudioLinker-Setup-<version>.exe.sha256` : les empreintes SHA-256 de l'installateur et de l'exécutable portable ;
- `AudioLinker-<version>-portable.exe` : l'exécutable seul.

Un tag contenant `-` est publié comme préversion.

## 4. Signature de code (à faire avant la première diffusion large)

Sans signature, SmartScreen avertit à chaque téléchargement, et l'analyse comportementale de Windows Defender peut classer l'exécutable à tort comme persistant (`Behavior:Win32/Persistence.A!ml`), parce qu'il peut s'inscrire au démarrage de Windows. C'est un faux positif, mais il faut le prévenir :

1. Obtenir un certificat de signature de code (OV ou EV, ou Azure Trusted Signing) au format `.pfx`.
2. Ajouter deux secrets au dépôt GitHub : `SIGN_PFX_BASE64` (le `.pfx` en base64 : `[Convert]::ToBase64String([IO.File]::ReadAllBytes("cert.pfx"))`) et `SIGN_PFX_PASSWORD`.
3. Rien d'autre : la CI signe l'exécutable, l'installateur et le désinstallateur, avec horodatage.

En local : `$env:AL_SIGN_ARGS = '/f cert.pfx /p mot_de_passe /tr http://timestamp.digicert.com /td sha256 /fd sha256'`, signer `AudioLinker.exe` avec `signtool sign`, puis `.\installer\build.ps1 -ExePath build\AudioLinker.exe -Sign`.

Après la publication, soumettre l'installateur et l'exécutable à Microsoft (<https://www.microsoft.com/wdsi/filesubmission>, « Software developer ») pour que la réputation s'établisse plus vite. Si un utilisateur signale une détection, la même page sert à la contester.

## 5. Après la publication

- Surveiller les issues ; demander le journal (`%LOCALAPPDATA%\AudioLinker\audiolinker.log`) et, en cas de plantage, le dernier `.dmp` de `%LOCALAPPDATA%\AudioLinker\crash\`.
- Idées pour la suite : mise à jour automatique (vérifier la dernière release GitHub), publication par winget.
