export interface Example {
  id: string; label: string; raw: string; template: string;
  ints: number[]; strs: string[]; bytes: number; hash?: string;
}
export type Tok = { k: 't' | 'i' | 's'; text: string; n?: number };

export const esc = (s: string) => s.replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]!));

/** Walk the template. `{i}` and `{s}` are slots; `{{` and `}}` are literal braces. */
export function tokenize(ex: Pick<Example, 'template' | 'ints' | 'strs'>): Tok[] {
  const out: Tok[] = [];
  let i = 0, s = 0, last = 0, lit = '';
  const flush = () => { if (lit) { out.push({ k: 't', text: lit }); lit = ''; } };
  const re = /\{\{|\}\}|\{i\}|\{s\}/g;
  for (let m: RegExpExecArray | null; (m = re.exec(ex.template)); ) {
    lit += ex.template.slice(last, m.index);
    last = m.index + m[0].length;
    if (m[0] === '{{') lit += '{';
    else if (m[0] === '}}') lit += '}';
    else {
      flush();
      if (m[0] === '{i}') out.push({ k: 'i', text: String(ex.ints[i]), n: i++ });
      else out.push({ k: 's', text: ex.strs[s], n: s++ });
    }
  }
  lit += ex.template.slice(last);
  flush();
  return out;
}

export const join = (ex: Pick<Example, 'template' | 'ints' | 'strs'>) => tokenize(ex).map((t) => t.text).join('');

/** Build-time guard: a demo line that does not join back to its bytes must never ship. */
export function assertRoundTrip(ex: Example) {
  if (join(ex) !== ex.raw) throw new Error(`Demo example "${ex.label}" does not join back to the original line.`);
}

const slot = (t: Tok) => `${t.k}${t.n}`;

/** The original line, with each value marked by the lane it lands in. */
export function origHTML(ex: Example) {
  return tokenize(ex).map((t) => t.k === 't' ? esc(t.text)
    : `<span class="tok tok-${t.k}" data-slot="${slot(t)}" tabindex="0">${esc(t.text)}</span>`).join('');
}

export function templateHTML(ex: Example) {
  const t = tokenize({ ...ex }); // placeholders shown as {i}/{s}, literal text escaped
  return t.map((x) => x.k === 't' ? esc(x.text.replace(/\{/g, '{{').replace(/\}/g, '}}'))
    : `<span class="ph ph-${x.k}" data-slot="${slot(x)}">{${x.k}}</span>`).join('');
}

const list = (vals: (string | number)[], k: 'i' | 's') => vals.length === 0 ? '<span class="muted">[]</span>'
  : `<span class="muted">[</span>${vals.map((v, n) => `<span class="chip chip-${k}" data-slot="${k}${n}" tabindex="0">${esc(String(v))}</span>`).join('<span class="muted">, </span>')}<span class="muted">]</span>`;

export function lanesHTML(ex: Example) {
  return `<div class="lane"><dt>template</dt><dd>${templateHTML(ex)}</dd></div>`
    + `<div class="lane"><dt>ints</dt><dd>${list(ex.ints, 'i')}</dd></div>`
    + `<div class="lane"><dt>strs</dt><dd>${list(ex.strs, 's')}</dd></div>`;
}

/** SQL shown in the console, as [class, text] segments so the client can type it character by character. */
type Seg = [string, string];
export const SQL_SPLIT: Seg[][] = [[
  ['kw', 'SELECT'], ['', ' '], ['fn', 'RzSplit'], ['p', '('], ['id', 'line'], ['p', ')'], ['', ' '], ['kw', 'AS'], ['', ' '], ['id', 'parsed'], ['', ' '],
  ['kw', 'FROM'], ['', ' '], ['id', 'logs'], ['', ' '], ['kw', 'WHERE'], ['', ' '], ['id', 'id'], ['', ' = '], ['num', '1'], ['p', ';'],
]];
export const SQL_JOIN: Seg[][] = [
  [['kw', 'SELECT'], ['', ' '], ['fn', 'RzJoin'], ['p', '('], ['id', 'p.template'], ['p', ', '], ['id', 'p.ints'], ['p', ', '], ['id', 'p.strs'], ['p', ')'], ['', ' = '], ['id', 'line'], ['', ' '], ['kw', 'AS'], ['', ' '], ['id', 'identical']],
  [['kw', 'FROM'], ['', ' ('], ['kw', 'SELECT'], ['', ' '], ['id', 'line'], ['p', ', '], ['fn', 'RzSplit'], ['p', '('], ['id', 'line'], ['p', ')'], ['', ' '], ['kw', 'AS'], ['', ' '], ['id', 'p'], ['', ' '],
   ['kw', 'FROM'], ['', ' '], ['id', 'logs'], ['', ' '], ['kw', 'WHERE'], ['', ' '], ['id', 'id'], ['', ' = '], ['num', '1'], ['p', ');']],
];
export const sqlHTML = (lines: Seg[][]) => lines.map((l) => `<span class="sql-line">${l.map(([c, t]) => `<span class="${c ? 's-' + c : ''}" data-full="${esc(t)}">${esc(t)}</span>`).join('')}</span>`).join('');

export function summary(ex: Example) {
  const n = ex.ints.length, w = ex.strs.length;
  return `${ex.label}: ${n} ${n === 1 ? 'number' : 'numbers'} and ${w} ${w === 1 ? 'word' : 'words'} split from the line. Joined back, the line is identical, ${ex.bytes} bytes.`;
}
