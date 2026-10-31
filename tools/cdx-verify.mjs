// node tools/cdx-verify.mjs <cdx-sync.json> [--root <dossier>]
// Copié dans chaque application par charte-sync (aucune dépendance) : vérifie que les copies de la charte n'ont pas été modifiées à la main ni périmées
// par un générateur local. Le manifeste (cdx-sync.json, écrit par charte-sync) donne, pour chaque copie, son chemin et l'empreinte du fichier de cdx-ui dont
// elle vient. Code de sortie 1 au moindre écart : à mettre dans la CI de l'application.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';

const args = process.argv.slice(2);
const ri = args.indexOf('--root'), root = path.resolve(ri >= 0 ? args[ri + 1] : process.cwd());
const manifest = args.find((a, i) => !a.startsWith('--') && args[i - 1] !== '--root');
if (!manifest) { console.error('usage : node tools/cdx-verify.mjs <cdx-sync.json> [--root dossier]'); process.exit(2); }
const m = JSON.parse(fs.readFileSync(path.resolve(root, manifest), 'utf8'));
let bad = 0;
for (const [rel, info] of Object.entries(m.files)) {
  const f = path.join(root, rel);
  if (!fs.existsSync(f)) { console.log('ABSENT   ' + rel); bad++; continue; }
  // Les fins de ligne ne comptent pas : git les convertit en CRLF à l'extraction sous Windows (core.autocrlf), l'empreinte reste celle du fichier LF de cdx-ui.
  const h = crypto.createHash('sha256').update(Buffer.from(fs.readFileSync(f).toString('latin1').replace(/\r\n/g, '\n'), 'latin1')).digest('hex');
  if (h !== info.sha256) { console.log('MODIFIÉ  ' + rel + '  (attendu : copie de cdx-ui ' + m.cdxUi + ' / ' + info.from + ')'); bad++; }
}
console.log(bad ? `${bad} copie(s) de la charte à resynchroniser : node tools/charte-sync.mjs --write <application> dans cdx-ui` : `copies de la charte conformes (cdx-ui ${m.cdxUi}, ${Object.keys(m.files).length} fichiers)`);
process.exit(bad ? 1 : 0);
