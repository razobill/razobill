// Wrap tables so wide ones scroll inside their own region (keyboard reachable), never the page.
import { visit } from 'unist-util-visit';
export default function rehypeTables() {
  return (tree) => {
    visit(tree, 'element', (node, index, parent) => {
      if (node.tagName !== 'table' || !parent || parent.properties?.className?.includes?.('table-scroll')) return;
      parent.children[index] = {
        type: 'element', tagName: 'div',
        properties: { className: ['table-scroll'], tabIndex: 0, role: 'region', ariaLabel: 'Scrollable table' },
        children: [node],
      };
      return 'skip';
    });
  };
}
