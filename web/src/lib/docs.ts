import { getCollection, type CollectionEntry } from 'astro:content';
import { groups, hidden, more } from '../data/docs-nav';
import { url } from '../data/site';

export type Doc = CollectionEntry<'docs'>;
export interface NavItem { id: string; title: string; summary: string; href: string; group: string }

const strip = (s: string) => s.replace(/!?\[([^\]]*)\]\([^)]*\)/g, '$1').replace(/[`*_]/g, '').replace(/\s+/g, ' ').trim();

export function titleOf(doc: Doc): string {
  const m = doc.body?.match(/^#\s+(.+)$/m);
  return m ? strip(m[1]) : doc.id;
}

/** First prose paragraph after the H1, skipping the breadcrumb, lists, quotes, tables and code. */
export function summaryOf(doc: Doc): string {
  const lines = (doc.body ?? '').split('\n');
  let i = lines.findIndex((l) => /^#\s/.test(l)) + 1;
  for (; i < lines.length; i++) {
    const l = lines[i].trim();
    if (!l || /^\[Docs\]/.test(l) || /^(#|-|\*|>|\||```|---|\d+\.)/.test(l)) continue;
    return strip(l);
  }
  return '';
}

export const docHref = (id: string) => url(`docs/${id}/`);

export async function loadDocs() {
  const all = (await getCollection('docs')).filter((d) => !hidden.has(d.id));
  const byId = new Map(all.map((d) => [d.id, d]));
  const toItem = (d: Doc, group: string): NavItem => ({ id: d.id, title: titleOf(d), summary: summaryOf(d), href: docHref(d.id), group });
  const listed = new Set<string>();
  const nav = groups.map((g) => {
    const items = g.ids.filter((id) => byId.has(id)).map((id) => { listed.add(id); return toItem(byId.get(id)!, g.id); });
    return { ...g, items };
  });
  const rest = all.filter((d) => !listed.has(d.id)).sort((a, b) => more.indexOf(a.id) - more.indexOf(b.id) || a.id.localeCompare(b.id));
  if (rest.length) nav.push({ id: 'more', title: 'More', lead: 'Contributor material.', ids: rest.map((d) => d.id), items: rest.map((d) => toItem(d, 'more')) });
  const flat = nav.flatMap((g) => g.items);
  return { all, byId, nav, flat };
}
