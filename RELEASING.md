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

Sans signature, SmartScreen avertit à chaque téléchargement, et l'analyse comportementale de Windows Defender peut classer l'exécutable à tort comme persistant (`Behavior:Win32/Persistence.A!ml`), parce qu'il peut s'inscrire au démarrage de Windows. C'est un faux positif, mais il faut le prévenir.

**Ce qui a changé** : depuis le 1er juin 2023, les autorités de certification exigent que la clé privée d'un certificat de signature de code (OV ou EV) soit générée et gardée dans un module matériel (jeton ou HSM). **Un `.pfx` exportable n'existe donc plus pour un certificat neuf.** L'étape « Prepare code signing » du workflow (secrets `SIGN_PFX_BASE64` / `SIGN_PFX_PASSWORD`) ne sert que pour un ancien certificat ; pour les options ci-dessous il faut une autre étape de signature (voir plus bas). Depuis le 1er mars 2026, la durée de validité d'un certificat est aussi limitée à 460 jours.

Options (état des lieux du 28 octobre 2026, à reconfirmer auprès de chaque fournisseur avant de s'engager) :

| Option | Pour | À savoir |
|---|---|---|
| **SignPath Foundation** (programme gratuit pour l'open source) | Dépôt public et licence MIT : AudioLinker correspond au profil. Signature faite par une GitHub Action sur un build de confiance. | Demande à déposer sur signpath.org et à faire approuver ; authentification à deux facteurs pour tous les contributeurs ; mention « Free code signing provided by SignPath.io, certificate by SignPath Foundation » ; l'éditeur affiché est la fondation, pas vous. |
| **Azure Artifact Signing** (ex-Trusted Signing) | Signature cloud, environ 10 USD par mois (offre de base), pas de matériel à gérer. | Pour la confiance publique, les **particuliers ne sont acceptés qu'aux États-Unis et au Canada** ; une organisation (société) établie dans l'UE peut l'être, après vérification d'identité. |
| **Certificat OV / EV d'une autorité** (jeton USB ou signature cloud de l'autorité) | Option classique. | Payant, annuel, jeton ou service cloud selon l'autorité ; la CI doit alors signer via le service cloud de l'autorité, pas avec un `.pfx`. |

Recommandation : demander SignPath Foundation (gratuit, adapté à un projet MIT public). **L'intégration est déjà dans le dépôt** et reste inerte tant que SignPath n'est pas configuré.

### Activer SignPath (une fois la demande approuvée)

Conditions de la fondation à remplir avant de demander (<https://signpath.org/terms>, demande sur <https://signpath.org/apply>) : licence OSI (MIT ✔), dépôt public ✔, projet déjà publié (v1.2.0 ✔), fonctionnement décrit sur la page de téléchargement ✔, authentification à deux facteurs sur GitHub **et** SignPath (à vérifier sur votre compte), rôles auteur / relecteur / approbateur et vie privée sur une page « Code signing policy » liée depuis le README et la page de téléchargement ✔ ([`CODE_SIGNING_POLICY.md`](CODE_SIGNING_POLICY.md)). Dans la demande, indiquer la page de politique et le dépôt.

1. **Dans SignPath** (interface web, après approbation) : créer le projet (slug `AudioLinker`), ajouter le *trusted build system* « GitHub.com » et le lier au projet, installer la SignPath GitHub App sur le dépôt. Créer deux *artifact configurations* en collant le contenu de [`installer/signpath/artifact-configuration-exe.xml`](installer/signpath/artifact-configuration-exe.xml) (slug `exe`) et de [`installer/signpath/artifact-configuration-installer.xml`](installer/signpath/artifact-configuration-installer.xml) (slug `installer`). Créer la *signing policy* `release-signing` (approbation manuelle par vous). Créer un jeton d'API pour un utilisateur autorisé à soumettre.
2. **Dans GitHub** (Settings > Secrets and variables > Actions) : secret `SIGNPATH_API_TOKEN` ; variable `SIGNPATH_ORGANIZATION_ID` (l'identifiant de l'organisation SignPath) ; facultatif : `SIGNPATH_PROJECT_SLUG` (défaut `AudioLinker`) et `SIGNPATH_SIGNING_POLICY_SLUG` (défaut `release-signing`).
3. **Au prochain tag `v*`**, le workflow envoie `AudioLinker.exe` puis l'installateur à SignPath (deux demandes, **chacune attend votre approbation dans SignPath**, jusqu'à une heure), vérifie les deux signatures (`Get-AuthenticodeSignature` doit répondre `Valid`), recalcule le SHA-256 de l'installateur signé et publie. Les notes de release passent d'« version non signée » à « signée » toutes seules. Les builds sans tag ne sont jamais signés.
4. Retirer le bandeau « État : en préparation » de `CODE_SIGNING_POLICY.md`.

Ce qui n'a **pas pu être testé** sans compte : les appels à l'action SignPath, la syntaxe exacte des configurations d'artefact (écrite d'après la documentation de SignPath) et le fichier de politique de signature que SignPath peut demander dans le dépôt pour les projets open source (l'emplacement et la syntaxe sont donnés pendant l'intégration ; je n'ai pas voulu en inventer un). Prévoir un tag d'essai (`v1.2.1-rc1`) pour valider la chaîne avant la prochaine vraie version.

En local, avec un certificat accessible à `signtool` : `$env:AL_SIGN_ARGS = '<arguments de signtool>'`, signer `AudioLinker.exe`, puis `.\installer\build.ps1 -ExePath build\AudioLinker.exe -Sign`.

### Soumission à Microsoft (faux positif Defender / réputation SmartScreen)

Une fois la release publiée, déposer l'installateur et l'exécutable sur <https://www.microsoft.com/wdsi/filesubmission> (compte Microsoft requis, profil « Software developer »), et utiliser la même page si un utilisateur signale une détection. Texte à coller :

> AudioLinker 1.2.0 is an open-source (MIT) Windows audio utility that plays the system sound on several output devices at once. Source code: https://github.com/Crazyshouby/AudioLinker. The installer is built by GitHub Actions from the tagged source. The only persistence it creates is an optional "Run at Windows startup" entry in HKCU\Software\Microsoft\Windows\CurrentVersion\Run, which the user enables explicitly (installer task or application option) and which the uninstaller removes. It does not connect to the network except to download the Microsoft WebView2 runtime from Microsoft's own fwlink during installation. Detection name, if any: <à compléter>. Please review and remove the false positive.

## 5. Après la publication

- Surveiller les issues ; demander le journal (`%LOCALAPPDATA%\AudioLinker\audiolinker.log`) et, en cas de plantage, le dernier `.dmp` de `%LOCALAPPDATA%\AudioLinker\crash\`.
- Idées pour la suite : mise à jour automatique (vérifier la dernière release GitHub), publication par winget.
