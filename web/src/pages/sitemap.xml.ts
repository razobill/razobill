import type { APIRoute } from 'astro';
import { loadDocs } from '../lib/docs';
import { SITE, isoDate } from '../data/site';

// Hand-written (not the sitemap integration) so the URL stays /sitemap.xml, which Search Console already has.
export const GET: APIRoute = async () => {
  const { all } = await loadDocs();
  const paths = ['', 'docs/', ...all.map((d) => `docs/${d.id}/`)];
  const body = `<?xml version="1.0" encoding="UTF-8"?>\n<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n`
    + paths.map((p) => `  <url><loc>${new URL(p, SITE).href}</loc><lastmod>${isoDate}</lastmod></url>`).join('\n') + '\n</urlset>\n';
  return new Response(body, { headers: { 'Content-Type': 'application/xml; charset=utf-8' } });
};
