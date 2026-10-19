import { type Example, origHTML, lanesHTML, sqlHTML, SQL_SPLIT, SQL_JOIN } from './console';

/** The four steps of the demo. Rendered at build time for the default example, and by the client for the others. */
export function consoleBody(ex: Example) {
  return `
<div class="step step-q1" data-step="1"><span class="ps" aria-hidden="true">D</span><code class="sql">${sqlHTML(SQL_SPLIT)}</code></div>
<div class="step step-out" data-step="2">
  <p class="orig"><span class="orig-label">line</span><span class="orig-text">${origHTML(ex)}</span></p>
  <dl class="lanes">${lanesHTML(ex)}</dl>
</div>
<div class="step step-q2" data-step="3"><span class="ps" aria-hidden="true">D</span><code class="sql">${sqlHTML(SQL_JOIN)}</code></div>
<div class="step step-verdict" data-step="4"><span class="ok"><span class="ok-mark" aria-hidden="true">✓</span> true</span><span class="muted">identical, ${ex.bytes} bytes</span></div>`;
}
