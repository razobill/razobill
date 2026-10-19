/* Tabs, copy buttons, docs scroll-spy. No framework: each piece is a few lines. */

// ---- tabs: [data-tabs] containing [role=tab][aria-controls] ----
document.querySelectorAll<HTMLElement>('[data-tabs]').forEach((group) => {
  const tabs = [...group.querySelectorAll<HTMLButtonElement>('[role=tab]')];
  const show = (n: number, focus = false) => tabs.forEach((t, i) => {
    const on = i === n;
    t.setAttribute('aria-selected', String(on)); t.tabIndex = on ? 0 : -1;
    const panel = document.getElementById(t.getAttribute('aria-controls')!);
    if (panel) panel.hidden = !on;
    if (on && focus) t.focus();
  });
  tabs.forEach((t, i) => {
    t.addEventListener('click', () => show(i));
    t.addEventListener('keydown', (e) => {
      const k = e.key === 'ArrowRight' ? 1 : e.key === 'ArrowLeft' ? -1 : e.key === 'Home' ? -99 : e.key === 'End' ? 99 : 0;
      if (!k) return;
      e.preventDefault();
      show(k === -99 ? 0 : k === 99 ? tabs.length - 1 : (i + k + tabs.length) % tabs.length, true);
    });
  });
});

// ---- copy ----
const ICON = '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" aria-hidden="true"><rect x="5.5" y="5.5" width="8" height="8"/><path d="M10.5 3.5v-2h-8v8h2"/></svg>';
const DONE = '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.75" aria-hidden="true"><path d="M2.5 8.5l3.5 3.5 7.5-8"/></svg>';
const status = Object.assign(document.createElement('div'), { className: 'sr-only' });
status.setAttribute('role', 'status');
document.body.append(status);

const bindCopy = (btn: HTMLButtonElement, text: () => string) => {
  btn.innerHTML = ICON;
  btn.addEventListener('click', async () => {
    try { await navigator.clipboard.writeText(text()); } catch { return; }
    btn.innerHTML = DONE; btn.dataset.done = ''; status.textContent = 'Copied to clipboard';
    setTimeout(() => { btn.innerHTML = ICON; delete btn.dataset.done; status.textContent = ''; }, 1500);
  });
};
document.querySelectorAll<HTMLButtonElement>('.copy[data-copy]').forEach((b) => bindCopy(b, () => b.dataset.copy!));
document.querySelectorAll<HTMLButtonElement>('.codepanel .copy').forEach((b) => bindCopy(b, () => b.parentElement!.querySelector('pre')!.innerText.replace(/\n$/, '')));
document.querySelectorAll<HTMLElement>('.prose pre').forEach((pre) => {
  const wrap = document.createElement('div'); wrap.className = 'codepanel docs-code';
  pre.replaceWith(wrap);
  const b = document.createElement('button'); b.type = 'button'; b.className = 'copy'; b.setAttribute('aria-label', 'Copy code');
  wrap.append(b, pre);
  bindCopy(b, () => pre.innerText.replace(/\n$/, ''));
});

// ---- docs: highlight the current heading in "On this page" ----
const toc = document.querySelector<HTMLElement>('.toc');
if (toc && 'IntersectionObserver' in window) {
  const links = new Map([...toc.querySelectorAll<HTMLAnchorElement>('a')].map((a) => [decodeURIComponent(a.hash.slice(1)), a]));
  const heads = [...links.keys()].map((id) => document.getElementById(id)).filter(Boolean) as HTMLElement[];
  const io = new IntersectionObserver((es) => {
    for (const e of es) if (e.isIntersecting) {
      links.forEach((a) => a.removeAttribute('aria-current'));
      links.get(e.target.id)?.setAttribute('aria-current', 'true');
    }
  }, { rootMargin: '-72px 0px -70% 0px' });
  heads.forEach((h) => io.observe(h));
}

// ---- diagrams: fit-to-width inline, natural size in a lightbox ----
const figs = document.querySelectorAll<HTMLElement>('figure.diagram');
if (figs.length) {
  const dlg = document.createElement('dialog');
  dlg.className = 'lightbox'; dlg.setAttribute('aria-label', 'Diagram, full size');
  dlg.innerHTML = '<form method="dialog" class="lightbox-bar"><span>Diagram, full size</span><button value="close">Close</button></form><div class="lightbox-body"></div>';
  document.body.append(dlg);
  const body = dlg.querySelector<HTMLElement>('.lightbox-body')!;
  dlg.addEventListener('click', (e) => { if (e.target === dlg) dlg.close(); });
  figs.forEach((fig) => {
    const b = document.createElement('button');
    b.type = 'button'; b.className = 'diagram-open'; b.textContent = 'View full size';
    b.addEventListener('click', () => {
      const shown = [...fig.querySelectorAll<HTMLElement>('.diagram-light, .diagram-dark')].find((d) => getComputedStyle(d).display !== 'none');
      if (!shown) return;
      body.innerHTML = shown.innerHTML;
      dlg.showModal();
    });
    fig.append(b);
  });
}
