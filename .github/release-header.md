## AudioLinker VERSION

Diffuse le son de Windows sur plusieurs sorties à la fois, avec une interface en plusieurs fenêtres dans le style de CDX. Détail dans le [changelog](https://github.com/Crazyshouby/AudioLinker/blob/main/CHANGELOG.md).

### Installer

1. Télécharger **`AudioLinker-Setup-VERSION.exe`** et le lancer (aucun droit administrateur nécessaire, installation pour votre compte).
2. Installer [VB-CABLE](https://vb-audio.com/Cable/) (gratuit) : c'est le câble audio virtuel qui sert à capturer le son du système. Le mode « Sans câble » des Options (expérimental) permet d'essayer sans.
3. Le runtime WebView2 est installé automatiquement s'il manque (déjà présent sur Windows 11).

Windows 10 (version 2004) ou Windows 11, 64 bits. `AudioLinker-VERSION-portable.exe` est l'exécutable seul, sans installation.

### Vérifier le téléchargement

Les empreintes SHA-256 sont dans `AudioLinker-Setup-VERSION.exe.sha256` : `sha256sum -c AudioLinker-Setup-VERSION.exe.sha256`, ou `Get-FileHash` sous PowerShell.

