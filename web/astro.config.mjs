import { defineConfig } from 'astro/config';
import { unified } from '@astrojs/markdown-remark';
import { remarkAlert } from 'remark-github-blockquote-alert';
import rehypeSlug from 'rehype-slug';
import rehypeAutolinkHeadings from 'rehype-autolink-headings';
import remarkDocs from './src/plugins/remark-docs.mjs';
import rehypeMermaid from './src/plugins/rehype-mermaid.mjs';
import rehypeTables from './src/plugins/rehype-tables.mjs';

// GitHub Pages project site: https://razobill.github.io/razobill/
// Using a custom domain? Set `site` to it and `base` to '/'.
const site = 'https://razobill.github.io';
const base = '/razobill';

export default defineConfig({
  site,
  base,
  trailingSlash: 'always',
  devToolbar: { enabled: false },
  build: { inlineStylesheets: 'auto' },
  markdown: {
    processor: unified({
      smartypants: false, // render the docs' punctuation exactly as written
      remarkPlugins: [[remarkDocs, { base, repo: 'https://github.com/razobill/razobill', branch: 'main' }], remarkAlert],
      rehypePlugins: [
        rehypeSlug,
        [rehypeAutolinkHeadings, { behavior: 'append', properties: { class: 'anchor', ariaLabel: 'Link to this section' }, content: [] }],
        rehypeTables,
        rehypeMermaid,
      ],
    }),
    syntaxHighlight: { type: 'shiki', excludeLangs: ['mermaid'] },
    shikiConfig: { theme: 'css-variables' },
  },
});
