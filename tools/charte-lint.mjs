// node tools/charte-lint.mjs <fichier|dossier> ... [--allow charte-lint.allow] [--manifest kdx-sync.json] [--json] [--examples N] [--include-generated]
// Contrôle les règles de CHARTE.md §8. Code de sortie 1 s'il reste des écarts non tolérés (les exceptions justifiées vont dans le fichier --allow).
// Dans une application (cette copie vient de kdx-ui par charte-sync) : --manifest désigne le kdx-sync.json de ses copies de la charte ; ces copies ne sont
// pas contrôlées (elles le sont à la source) et la palette permise est lue dans leur dossier. Sans --manifest : la charte elle-même, à la racine du dépôt.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { RULES, loadPalette, parseAllow, lintPaths, summarize } from './lib/lint.mjs';

const here = path.dirname(fileURLToPath(import.meta.url)), repo = path.resolve(here, '..');
const args = process.argv.slice(2);
const opt = (n, d) => { const i = args.indexOf(n); if (i < 0) return d; const v = args[i + 1]; args.splice(i, 2); return v; };
const flag = n => { const i = args.indexOf(n); if (i < 0) return false; args.splice(i, 1); return true; };
const allowFile = opt('--allow', fs.existsSync(path.join(repo, 'charte-lint.allow')) ? path.join(repo, 'charte-lint.allow') : '');
const manifestFile = opt('--manifest', '');
const examples = +opt('--examples', 3), json = flag('--json'), gen = flag('--include-generated');
if (!args.length) { console.error('usage : node tools/charte-lint.mjs <fichier|dossier> ... [--allow fichier] [--manifest kdx-sync.json] [--json] [--examples N]'); process.exit(2); }

const allow = allowFile ? parseAllow(fs.readFileSync(allowFile, 'utf8')) : [];
let paletteDir = repo, exclude = new Set();
if (manifestFile) {
  const m = JSON.parse(fs.readFileSync(manifestFile, 'utf8'));
  paletteDir = path.dirname(path.resolve(manifestFile));
  exclude = new Set(Object.keys(m.files).map(f => path.resolve(f)));   // les chemins du manifeste sont relatifs à la racine de l'application (le dossier courant)
}
const { files, violations } = lintPaths(args.map(p => path.resolve(p)), { palette: loadPalette(paletteDir), allow, includeGenerated: gen, base: process.cwd(), exclude });
const s = summarize(violations);
if (json) { console.log(JSON.stringify({ files: files.length, ...s, violations }, null, 1)); process.exit(s.total ? 1 : 0); }

console.log(`${files.length} fichier(s) examiné(s) : ${s.total} écart(s), ${s.tolerated} toléré(s)`);
for (const [r, label] of Object.entries(RULES)) if (s.by[r]) {
  console.log(`\n${r.padEnd(10)} ${String(s.by[r]).padStart(4)}  ${label}`);
  for (const v of violations.filter(x => x.rule === r && !x.tolerated).slice(0, examples)) console.log(`    ${v.file}:${v.line}  ${v.text}`);
}
process.exit(s.total ? 1 : 0);
