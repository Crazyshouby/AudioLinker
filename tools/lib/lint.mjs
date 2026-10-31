// charte-lint : les règles de CHARTE.md §8 appliquées à du CSS, du HTML et du JS. Pas de vrai analyseur CSS : un balayage ligne à ligne, comme un grep
// qui comprend les valeurs. Il compte des déclarations, pas des éléments affichés.
import fs from 'node:fs';
import path from 'node:path';

export const RULES = {
  radius: 'border-radius différent de 0 (aucun arrondi)',
  weight: 'graisse de police (pas de gras)',
  size: 'taille de texte hors multiples de 14 px',
  blur: 'ombre ou flou doux (ombres dures seulement)',
  gradient: 'dégradé doux hors jeton',
  'gray-tint': 'gris teinté (les gris sont neutres)',
  color: 'couleur hors jetons',
};

const norm = h => { h = h.toLowerCase(); return h.length === 4 ? '#' + [...h.slice(1)].map(c => c + c).join('') : h; };
const HEX = /(?<![&\w%])#([0-9a-fA-F]{6}|[0-9a-fA-F]{3})\b(?![\w-])/g;

// Les couleurs permises : celles de tokens.json et celles que cdx-ui.css / cdx-frame.css emploient elles-mêmes.
export function loadPalette(root) {
  const set = new Set();
  for (const f of ['tokens.json', 'cdx-ui.css', 'cdx-frame.css', 'themes.css']) {
    const p = path.join(root, f); if (!fs.existsSync(p)) continue;
    for (const m of fs.readFileSync(p, 'utf8').matchAll(HEX)) set.add(norm('#' + m[1]));
  }
  return set;
}

const splitTop = s => { const out = []; let d = 0, cur = ''; for (const ch of s) { if (ch === '(') d++; if (ch === ')') d--; if (ch === ',' && d === 0) { out.push(cur); cur = ''; } else cur += ch; } out.push(cur); return out; };
const px = v => { const m = /^(-?\d*\.?\d+)(px)?$/.exec(v); return m ? parseFloat(m[1]) : null; };

export function lintText(text, { file = '', palette = new Set() } = {}) {
  const out = [];
  text.split(/\r?\n/).forEach((raw, i) => {
    if (/base64,/.test(raw) && raw.length > 400) return;          // police ou image embarquée
    const line = raw.replace(/\/\*.*?\*\//g, '');
    const push = rule => out.push({ file, line: i + 1, rule, text: raw.trim().slice(0, 170) });

    for (const m of line.matchAll(/(?:border(?:-[a-z]+){0,2}-radius|borderRadius)\s*[:=]\s*['"`]?([^;}'"`\n]+)/g)) {
      const toks = m[1].trim().split(/[\s/]+/).filter(Boolean);
      if (toks.some(t => !/^(0|0px|0%|0em|inherit|initial|unset|none|var\(.*\)|!important)$/.test(t))) push('radius');
    }
    for (const m of line.matchAll(/(?:(?<![\w-])font-weight|fontWeight)\s*[:=]\s*['"`]?([^;}'"`,\n]+)/g)) {
      if (!/^(normal|400|lighter|inherit|initial|unset|var\(.*\))\b/.test(m[1].trim())) push('weight');
    }
    for (const m of line.matchAll(/(?<![\w-])font\s*:\s*([^;}\n]+)/g)) {
      const v = m[1];
      if (/(^|\s)(bold|bolder|[5-9]00)(\s|$)/.test(v)) push('weight');
      const sz = /(^|\s)(\d*\.?\d+)px\b/.exec(v);
      if (sz && Math.abs(parseFloat(sz[2]) / 14 - Math.round(parseFloat(sz[2]) / 14)) > 1e-6) push('size');
    }
    for (const m of line.matchAll(/(?:(?<![\w-])font-size|fontSize)\s*[:=]\s*['"`]?([^;}'"`\n]+)/g)) {
      const v = m[1].trim().toLowerCase();
      if (/^(inherit|initial|unset|1em|1rem|100%|var\(.*\)|calc\(.*\))/.test(v)) continue;
      const n = /^(\d*\.?\d+)px/.exec(v);
      if (!n || Math.abs(parseFloat(n[1]) / 14 - Math.round(parseFloat(n[1]) / 14)) > 1e-6 || parseFloat(n[1]) === 0) push('size');
    }
    for (const m of line.matchAll(/(?:(?<![\w-])(?:box|text)-shadow|boxShadow|textShadow)\s*[:=]\s*['"`]?([^;}'"`\n]+)/g)) {
      const soft = splitTop(m[1]).some(sh => {
        const nums = sh.replace(/(rgba?|hsla?|var)\([^)]*\)/g, ' ').replace(/#[0-9a-fA-F]+/g, ' ').replace(/\b[a-z-]+\b/gi, ' ').trim().split(/\s+/).map(px).filter(n => n !== null);
        return nums.length >= 3 && Math.abs(nums[2]) > 0;
      });
      if (soft) push('blur');
    }
    if (/(?<![\w-])(?:backdrop-filter|filter)\s*:[^;}\n]*blur\(/.test(line)) push('blur');
    if (/(?<![-\w])(linear|radial|conic)-gradient\(/.test(line) && !/^\s*--[\w-]+\s*:/.test(line)) push('gradient');
    if (!/@font-face|url\(/.test(line)) {
      for (const m of line.matchAll(HEX)) {
        const h = norm('#' + m[1]); if (palette.has(h)) continue;
        const [r, g, b] = [1, 3, 5].map(k => parseInt(h.slice(k, k + 2), 16));
        if (r === g && g === b) continue;                           // un gris neutre
        push(Math.max(r, g, b) - Math.min(r, g, b) <= 28 ? 'gray-tint' : 'color');
      }
    }
  });
  return out;
}

// Exceptions justifiées : une par ligne, « règle<TAB>fragment du fichier<TAB>fragment de la ligne<TAB>raison » ; « * » pour toute règle ; # commente.
export function parseAllow(text) {
  return text.split(/\r?\n/).filter(l => l.trim() && !l.trim().startsWith('#')).map(l => { const [rule, file = '', match = '', reason = ''] = l.split('\t'); return { rule: rule.trim(), file: file.trim(), match: match.trim(), reason: reason.trim() }; });
}
export const allowedBy = (v, allow) => allow.find(a => (a.rule === '*' || a.rule === v.rule) && v.file.replace(/\\/g, '/').includes(a.file) && (a.match === '' || v.text.includes(a.match)));

const EXT = /\.(css|html?|js|mjs)$/i;
const SKIP_NAME = /^(fonts\.css|icons\.js|themes\.css|_test-.*|.*\.test\.html|.*\.min\.(js|css))$/i;
const SKIP_DIR = /^(node_modules|target|dist|build|build-dev|\.git|third_party)$/i;

export function collect(paths, { includeGenerated = false, exclude = new Set() } = {}) {
  const files = [];
  const walk = p => {
    const st = fs.statSync(p);
    if (st.isDirectory()) { for (const n of fs.readdirSync(p)) { if (!SKIP_DIR.test(n)) walk(path.join(p, n)); } }
    else if (EXT.test(p) && !exclude.has(path.resolve(p)) && (includeGenerated || !SKIP_NAME.test(path.basename(p)))) files.push(p);
  };
  for (const p of paths) if (fs.existsSync(p)) walk(p);
  return files;
}

export function lintPaths(paths, { palette, allow = [], includeGenerated = false, base = '', exclude = new Set() } = {}) {
  const files = collect(paths, { includeGenerated, exclude });
  const violations = [];
  for (const f of files) {
    const rel = base ? path.relative(base, f) : f;
    for (const v of lintText(fs.readFileSync(f, 'utf8'), { file: rel.replace(/\\/g, '/'), palette })) {
      const a = allowedBy(v, allow); if (a) v.tolerated = a.reason || 'toléré'; violations.push(v);
    }
  }
  return { files, violations };
}

export function summarize(violations) {
  const by = {}, byFile = {};
  for (const v of violations) {
    const k = v.tolerated ? 'tolérées' : v.rule;
    by[k] = (by[k] || 0) + 1;
    if (!v.tolerated) { byFile[v.file] = byFile[v.file] || {}; byFile[v.file][v.rule] = (byFile[v.file][v.rule] || 0) + 1; }
  }
  return { by, byFile, total: violations.filter(v => !v.tolerated).length, tolerated: violations.filter(v => v.tolerated).length };
}
