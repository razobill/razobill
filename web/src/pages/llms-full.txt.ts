import type { APIRoute } from 'astro';
import { loadDocs } from '../lib/docs';
import { SITE, version } from '../data/site';

export const GET: APIRoute = async () => {
  const { nav, byId } = await loadDocs();
  const parts = nav.flatMap((g) => g.items).map((i) => `<!-- source: ${new URL(`docs/${i.id}/`, SITE).href} -->\n${byId.get(i.id)?.body ?? ''}`);
  const body = `# Razobill ${version}: full documentation\n\nAll documentation pages concatenated, in reading order. Relative links point to files in the repository.\n\n---\n\n${parts.join('\n\n---\n\n')}\n`;
  return new Response(body, { headers: { 'Content-Type': 'text/plain; charset=utf-8' } });
};
