// Makes the repository's own markdown (docs/**) render as site pages without editing it.
//  1. drops the "[Docs](../README.md) / ... " breadcrumb line (the site renders its own)
//  2. rewrites relative links: docs/*.md -> site routes, anything else -> GitHub
import path from 'node:path';
import { visit } from 'unist-util-visit';

export default function remarkDocs({ base, repo, branch }) {
  return (tree, file) => {
    const abs = file.path;
    if (!abs) return;
    // docs root = the nearest ancestor directory named "docs"
    const parts = abs.split(path.sep);
    const i = parts.lastIndexOf('docs');
    if (i < 0) return;
    const root = parts.slice(0, i + 1).join(path.sep);
    const repoRoot = path.dirname(root);

    // 1. breadcrumb: first paragraph after the H1 that starts with a "Docs" link
    let seenH1 = false;
    tree.children = tree.children.filter((node) => {
      if (node.type === 'heading' && node.depth === 1) { seenH1 = true; return true; }
      if (seenH1 && node.type === 'paragraph') {
        const first = node.children?.[0];
        if (first?.type === 'link' && /readme\.md/i.test(first.url) && first.children?.[0]?.value === 'Docs') {
          seenH1 = false; return false;
        }
        seenH1 = false;
      }
      return true;
    });

    // 2. links
    visit(tree, 'link', (node) => {
      const url = node.url;
      if (!url || /^([a-z]+:|#|\/\/)/i.test(url)) return;
      const [target, hash = ''] = url.split('#');
      const resolved = path.resolve(path.dirname(abs), target);
      const relDocs = path.relative(root, resolved);
      const insideDocs = !relDocs.startsWith('..') && !relDocs.startsWith('web' + path.sep);
      if (insideDocs && /\.md$/i.test(resolved)) {
        const id = relDocs.replace(/\.md$/i, '').split(path.sep).join('/').toLowerCase();
        node.url = id === 'readme' ? `${base}/docs/` : `${base}/docs/${id}/${hash ? '#' + hash : ''}`;
        return;
      }
      const relRepo = path.relative(repoRoot, resolved).split(path.sep).join('/');
      const isDir = !path.extname(resolved);
      node.url = `${repo}/${isDir ? 'tree' : 'blob'}/${branch}/${relRepo}${hash ? '#' + hash : ''}`;
    });
  };
}
