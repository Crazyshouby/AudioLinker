# Politique de signature de code / Code signing policy

> **État : en préparation.** Les versions publiées jusqu'ici ne sont pas signées. Une demande de signature gratuite auprès de la [SignPath Foundation](https://signpath.org) est prévue ; cette politique décrit comment la signature fonctionnera dès qu'elle est active, et les notes de chaque release indiquent si cette version-là est signée.

Quand la signature est active :

> *Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by [SignPath Foundation](https://signpath.org).*

## Ce qui est signé

Uniquement les fichiers d'AudioLinker, construits à partir du code source de ce dépôt :

- `AudioLinker.exe` (l'application, aussi publiée en version portable) ;
- `AudioLinker-Setup-<version>.exe` (l'installateur, construit avec l'exécutable signé).

Rien d'autre n'est signé avec ce certificat : pas de composant tiers, pas de fichier d'un autre projet. Le désinstallateur généré à l'installation n'est pas signé séparément.

## Comment

Le build se fait **automatiquement par GitHub Actions**, sur des machines hébergées par GitHub, à partir du code source marqué par un tag de version (`v1.2.0`). Le workflow est dans [`.github/workflows/build.yml`](.github/workflows/build.yml) ; il compile, exécute les tests, construit l'installateur, puis envoie les deux fichiers à SignPath. Les fichiers signés sont repris tels quels dans la release GitHub, avec leurs empreintes SHA-256.

Les métadonnées de chaque fichier signé (nom du produit `AudioLinker` et numéro de version) sont vérifiées par SignPath : un fichier dont la version ne correspond pas à celle de la release est refusé.

## Qui

AudioLinker est un projet à un seul mainteneur. Tous les rôles sont tenus par la même personne :

| Rôle | Personne |
|---|---|
| Auteur (écrit et valide le code, propriétaire du dépôt) | Valentin Large ([@Crazyshouby](https://github.com/Crazyshouby)) |
| Relecteur (relit les contributions de tiers avant fusion) | Valentin Large |
| Approbateur (approuve **chaque** demande de signature, manuellement) | Valentin Large |

Toute personne ayant accès à SignPath ou au dépôt utilise l'authentification à deux facteurs. Les contributions de tiers passent par des pull requests relues avant fusion.

## Politique de confidentialité / Privacy policy

AudioLinker **ne collecte, n'envoie et ne partage aucune donnée personnelle.** Il n'y a ni télémétrie, ni statistique d'usage, ni compte.

- Les réglages (setups, enceintes, raccourcis) restent dans `%APPDATA%\AudioLinker`, le journal de diagnostic et les rapports de plantage dans `%LOCALAPPDATA%\AudioLinker`. Ces fichiers ne quittent pas votre ordinateur ; vous les joignez à un signalement si vous le souhaitez.
- Le programme n'accède au réseau que par le composant Microsoft Edge WebView2 qui affiche son interface (pages locales, navigation vers l'extérieur bloquée), et l'installateur télécharge ce composant depuis Microsoft s'il est absent. Le composant WebView2 est soumis à la politique de confidentialité de Microsoft.
- Le micro n'est utilisé que pour le calage automatique et les indicateurs de niveau de l'onglet Entrées, sur votre demande ; rien n'est enregistré.

AudioLinker s'installe et se désinstalle proprement (« Applications installées » de Windows), sans modification cachée du système : l'inscription au démarrage de Windows est optionnelle et retirée à la désinstallation. Il ne contient aucune fonction de recherche ou d'exploitation de vulnérabilités.

## Signaler un problème

Une détection antivirus erronée, un doute sur un fichier signé : [ouvrir une issue](https://github.com/Crazyshouby/AudioLinker/issues).
