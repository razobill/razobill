import { consoleBody } from '../lib/consoleBody';
import { summary, type Example } from '../lib/console';

/*
  Hero demo. The finished state is already in the HTML, so it works with JavaScript off.
  With motion allowed, the page types the two queries once (about 3 s, under the 5 s WCAG 2.2.2 limit),
  never loops, and the caret is solid, not blinking. Replay and the example tabs re-run it on request.
*/
const root = document.querySelector<HTMLElement>('.console');
if (root) {
  const data: Example[] = JSON.parse(document.getElementById('console-data')!.textContent!);
  const body = root.querySelector<HTMLElement>('.console-body')!;
  const live = root.querySelector<HTMLElement>('.console-live')!;
  const tabs = [...root.querySelectorAll<HTMLButtonElement>('[role=tab]')];
  const animate = document.documentElement.classList.contains('anim');
  let token = 0, current = 0;

  const sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms));

  const wire = () => {
    const set = (el: Element, on: boolean) => {
      const slot = (el as HTMLElement).dataset.slot;
      if (slot) body.querySelectorAll(`[data-slot="${slot}"]`).forEach((n) => n.classList.toggle('is-hot', on));
    };
    body.querySelectorAll('[data-slot]').forEach((el) => {
      el.addEventListener('mouseenter', () => set(el, true));
      el.addEventListener('mouseleave', () => set(el, false));
      el.addEventListener('focus', () => set(el, true));
      el.addEventListener('blur', () => set(el, false));
    });
  };

  const typeInto = async (step: HTMLElement, speed: number, mine: number) => {
    const caret = document.createElement('span');
    caret.className = 'caret';
    caret.setAttribute('aria-hidden', 'true');
    const spans = [...step.querySelectorAll<HTMLElement>('.sql-line > span')];
    for (const s of spans) s.textContent = '';
    for (const s of spans) {
      const full = s.dataset.full ?? '';
      for (let k = 1; k <= full.length; k++) {
        if (mine !== token) { caret.remove(); return false; }
        s.textContent = full.slice(0, k);
        s.after(caret);
        await sleep((9 + ((k * 7) % 4)) / speed);
      }
    }
    caret.remove();
    return true;
  };

  const play = async (speed = 1) => {
    const mine = ++token;
    const ex = data[current];
    live.textContent = '';
    if (!animate) { live.textContent = summary(ex); return; }
    const steps = [...body.querySelectorAll<HTMLElement>('.step')];
    steps.forEach((s) => s.classList.remove('on'));
    root.dataset.ready = '';
    await sleep(120 / speed);
    const [q1, out, q2, verdict] = steps;
    q1.classList.add('on');
    if (!(await typeInto(q1, speed, mine))) return;
    await sleep(200 / speed);
    out.classList.add('on');
    await sleep(560 / speed);
    if (mine !== token) return;
    q2.classList.add('on');
    if (!(await typeInto(q2, speed, mine))) return;
    await sleep(160 / speed);
    verdict.classList.add('on');
    live.textContent = summary(ex);
  };

  const select = (n: number, speed: number) => {
    current = n;
    tabs.forEach((t, i) => { t.setAttribute('aria-selected', String(i === n)); t.tabIndex = i === n ? 0 : -1; });
    body.setAttribute('aria-labelledby', tabs[n].id);
    body.innerHTML = consoleBody(data[n]);
    wire();
    play(speed);
  };

  tabs.forEach((t, i) => {
    t.addEventListener('click', () => { if (i !== current) select(i, 2); });
    t.addEventListener('keydown', (e) => {
      const k = e.key === 'ArrowRight' ? 1 : e.key === 'ArrowLeft' ? -1 : 0;
      if (!k) return;
      e.preventDefault();
      const n = (i + k + tabs.length) % tabs.length;
      tabs[n].focus(); select(n, 2);
    });
  });
  root.querySelector('.console-replay')?.addEventListener('click', () => play(1));

  wire();
  live.textContent = summary(data[0]);
  if (animate) {
    // start when the console actually enters view, after fonts are in (no metric jump mid-typing)
    const start = () => document.fonts.ready.then(() => play(1));
    if ('IntersectionObserver' in window) {
      const io = new IntersectionObserver((es) => { if (es.some((e) => e.isIntersecting)) { io.disconnect(); start(); } }, { rootMargin: '0px 0px -60px 0px' });
      io.observe(root);
    } else start();
  }
}
