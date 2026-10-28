/*
 * usr40k tiling wm
 *
 *   layout.json   -> split tree + hover tuning + status bar text
 *   tiles/<id>.json -> one file per tile: { title, accent, min, config, html, css, js }
 *
 * How the layout moves
 * --------------------
 * Every tile is a leaf in a tree of horizontal/vertical splits. A leaf has a
 * weight (base + "excess"); a split's weight is the sum of its leaves, and
 * siblings share space in proportion to weight (plain flex-grow).
 *
 * Hovering a tile (after a short dwell) FOCUSES it. Focusing does two things:
 *   1. every tile's excess decays a bit (excess *= retain)
 *   2. the focused tile gains `grow` excess (capped at `max`)
 * grow and max are fractions of the total base weight of all tiles.
 *
 * Nothing happens when the cursor leaves a tile or moves around inside one:
 * a tile only changes size when some OTHER tile gets focus. So the layout is a
 * function of the path the cursor took (recent tiles stay bigger, old ones
 * fade back to base), not of where the cursor is right now.
 *
 * Esc resets everything. Tab / Alt+h j k l move focus with the keyboard.
 */
(function () {
  'use strict';

  const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;
  const stackedMQ = window.matchMedia('(max-width: 760px)');

  const $app = document.getElementById('app');
  const $wm = document.getElementById('wm');
  const $bar = document.getElementById('bar');

  /* ---------------- helpers ---------------- */

  const h = (tag, cls, text) => {
    const n = document.createElement(tag);
    if (cls) n.className = cls;
    if (text != null) n.textContent = text;
    return n;
  };

  const joinLines = (v) => (Array.isArray(v) ? v.join('\n') : v == null ? '' : String(v));

  const esc = (s) => String(s == null ? '' : s)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;').replace(/'/g, '&#39;');

  const fetchJSON = async (url) => {
    const r = await fetch(url, { cache: 'no-cache' });
    if (!r.ok) throw new Error(`${url}: HTTP ${r.status}`);
    return r.json();
  };

  const addStyle = (css, id) => {
    const s = document.createElement('style');
    if (id) s.dataset.tileCss = id;
    s.textContent = css;
    document.head.appendChild(s);
  };

  const fatal = (msg) => {
    $wm.textContent = '';
    const pre = h('pre', 'fatal', msg);
    $wm.appendChild(pre);
    document.body.dataset.wm = 'error';
  };

  /* ---------------- state ---------------- */

  const leaves = [];          // document order
  const byId = new Map();
  const state = { last: null };
  let cfg = null;
  let hover = { dwell: 90, grow: 0.3, retain: 0.6, max: 0.6 };
  let rootNode = null;

  /* ---------------- tree ---------------- */

  function buildNode(node) {
    if (node && typeof node.tile === 'string') return makeLeaf(node);
    const split = h('div', `split ${node && node.split === 'v' ? 'v' : 'h'}`);
    const kids = ((node && node.children) || []).map(buildNode);
    kids.forEach((k) => split.appendChild(k.el));
    return {
      el: split,
      weight: () => kids.reduce((s, k) => s + k.weight(), 0),
      apply() { kids.forEach((k) => k.apply()); split.style.flexGrow = this.weight(); },
    };
  }

  function makeLeaf(node) {
    const leaf = {
      id: node.tile,
      base: typeof node.weight === 'number' ? node.weight : 1,
      ex: 0,
      def: null,
      el: h('section', 'tile'),
      listeners: [],
      timer: 0,
    };
    leaf.weight = () => leaf.base + leaf.ex;
    leaf.apply = () => { leaf.el.style.flexGrow = leaf.weight(); };
    leaves.push(leaf);
    byId.set(leaf.id, leaf);
    return leaf;
  }

  /* ---------------- focus / hover weights ---------------- */

  function focusLeaf(leaf) {
    if (!leaf || state.last === leaf) return;       // same tile: nothing changes
    // any pending hover-dwell is obsolete once focus has been decided
    leaves.forEach((l) => { clearTimeout(l.timer); l.timer = 0; });
    // grow / max are fractions of the total base weight, so they keep working
    // when tiles are added, removed or re-weighted
    const total = leaves.reduce((s, l) => s + l.base, 0);
    leaves.forEach((l) => { l.ex *= hover.retain; if (l.ex < 0.01 * total) l.ex = 0; });
    leaf.ex = Math.min(hover.max * total, leaf.ex + hover.grow * total);
    state.last = leaf;
    rootNode.apply();
    leaves.forEach((l) => l.el.classList.toggle('is-focus', l === leaf));
    updateBar();
    if (leaf.titleEl && window.asciiRipple) {
      const r = window.asciiRipple(leaf.titleEl);
      if (r) r.pulse();
    }
    leaf.listeners.forEach((fn) => { try { fn(); } catch (e) { console.error(e); } });
  }

  function reset() {
    leaves.forEach((l) => { l.ex = 0; l.el.classList.remove('is-focus'); });
    state.last = null;
    rootNode.apply();
    updateBar();
  }

  function focusById(id, opts) {
    const leaf = byId.get(id);
    if (!leaf) return false;
    focusLeaf(leaf);
    leaf.el.focus({ preventScroll: true });
    if (opts && opts.scroll) leaf.el.scrollIntoView({ block: 'start' });
    return true;
  }

  // Focus-follows-mouse, but only for REAL pointer movement. When the layout
  // shifts under a parked cursor the browser fires pointerenter on whatever
  // is now underneath; that must not count, so we arm the dwell timer from
  // pointermove (which carries movement) instead of pointerenter.
  function bindHover(leaf) {
    leaf.el.addEventListener('pointermove', (e) => {
      if (e.pointerType === 'touch' || stackedMQ.matches) return;
      if (state.last === leaf || leaf.timer) return;
      const moved = e.movementX === undefined || e.movementX !== 0 || e.movementY !== 0;
      if (!moved) return;
      leaf.timer = setTimeout(() => { leaf.timer = 0; focusLeaf(leaf); }, hover.dwell);
    });
    leaf.el.addEventListener('pointerleave', () => { clearTimeout(leaf.timer); leaf.timer = 0; });
    leaf.el.addEventListener('focusin', () => focusLeaf(leaf));
  }

  /* ---------------- keyboard ---------------- */

  function neighbour(dir) {
    const from = state.last || leaves.find((l) => l.el.contains(document.activeElement)) || leaves[0];
    if (!from) return null;
    const r0 = from.el.getBoundingClientRect();
    const c0 = { x: r0.left + r0.width / 2, y: r0.top + r0.height / 2 };
    let best = null;
    let bestScore = Infinity;
    leaves.forEach((l) => {
      if (l === from) return;
      const r = l.el.getBoundingClientRect();
      const dx = r.left + r.width / 2 - c0.x;
      const dy = r.top + r.height / 2 - c0.y;
      const main = dir === 'left' ? -dx : dir === 'right' ? dx : dir === 'up' ? -dy : dy;
      const cross = dir === 'left' || dir === 'right' ? Math.abs(dy) : Math.abs(dx);
      if (main <= 0) return;
      const score = main + cross * 2;
      if (score < bestScore) { bestScore = score; best = l; }
    });
    return best;
  }

  const KEYDIR = { KeyH: 'left', KeyJ: 'down', KeyK: 'up', KeyL: 'right' };

  document.addEventListener('keydown', (e) => {
    const t = e.target;
    if (t && (t.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(t.tagName))) return;
    if (e.key === 'Escape' && !e.altKey && !e.ctrlKey && !e.metaKey) {
      reset();
      return;
    }
    if (e.altKey && !e.ctrlKey && !e.metaKey && KEYDIR[e.code]) {
      const n = neighbour(KEYDIR[e.code]);
      if (n) { e.preventDefault(); n.el.focus({ preventScroll: true }); }
    }
  });

  // [data-focus-tile="id"] anywhere inside a tile focuses that tile
  document.addEventListener('click', (e) => {
    const a = e.target.closest && e.target.closest('[data-focus-tile]');
    if (!a) return;
    e.preventDefault();
    focusById(a.dataset.focusTile, { scroll: stackedMQ.matches });
  });

  function focusFromHash() {
    const id = decodeURIComponent(location.hash.slice(1));
    if (id && byId.has(id)) focusById(id, { scroll: stackedMQ.matches });
  }

  /* ---------------- status bar ---------------- */

  function renderBar() {
    const sb = cfg.statusbar || {};
    $bar.textContent = '';
    $bar.appendChild(h('span', 'sess', sb.session || cfg.site.user));
    leaves.forEach((l, i) => {
      const b = h('button', 'ws', `${i + 1}:${(l.def && l.def.title) || l.id}`);
      b.type = 'button';
      b.addEventListener('click', () => focusById(l.id));
      l.ws = b;
      $bar.appendChild(b);
    });
    $bar.appendChild(h('span', 'spacer'));
    if (sb.hint) $bar.appendChild(h('span', 'hint', sb.hint));
    const clock = h('span', 'clock');
    $bar.appendChild(clock);
    const tick = () => {
      const d = new Date();
      clock.textContent =
        d.toLocaleDateString([], { weekday: 'short', day: '2-digit', month: 'short' }) + ' ' +
        d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', hour12: false });
    };
    tick();
    setInterval(tick, 15000);
  }

  function updateBar() {
    leaves.forEach((l) => { if (l.ws) l.ws.classList.toggle('on', l === state.last); });
  }

  /* ---------------- tiles ---------------- */

  function mountTile(leaf) {
    const def = leaf.def;
    const el = leaf.el;
    el.id = leaf.id;
    el.dataset.tile = leaf.id;
    el.tabIndex = 0;
    el.setAttribute('aria-label', def.title || leaf.id);
    if (def.accent) el.style.setProperty('--accent', def.accent);
    if (def.min && def.min.w) el.style.setProperty('--min-w', `${def.min.w}px`);
    if (def.min && def.min.h) el.style.setProperty('--min-h', `${def.min.h}px`);

    const bar = h('header', 'tile-bar');
    leaf.titleEl = h('span', 'tile-title', def.title || leaf.id);
    bar.appendChild(leaf.titleEl);
    const body = h('div', 'tile-body');
    body.innerHTML = joinLines(def.html);
    el.append(bar, body);

    // custom css is scoped to this tile with native css nesting
    if (def.css) addStyle(`[data-tile="${leaf.id}"] {\n${joinLines(def.css)}\n}`, leaf.id);
    if (def.cssGlobal) addStyle(joinLines(def.cssGlobal), `${leaf.id}-global`);

    body.querySelectorAll('[data-user]').forEach((n) => { n.textContent = cfg.site.user; });

    if (window.asciiRipple) {
      window.asciiRipple(leaf.titleEl);
      window.asciiRipple.auto(body);
    }
    bindHover(leaf);
    leaf.body = body;
  }

  function runTileJs(leaf) {
    const def = leaf.def;
    if (!def.js) return;
    const ctx = {
      id: leaf.id,
      root: leaf.body,
      tile: leaf.el,
      config: def.config || {},
      site: cfg.site,
      esc,
      fetchJSON,
      ripple: (el, o) => window.asciiRipple && window.asciiRipple(el, o),
      pulse: (el, i) => { const r = window.asciiRipple && window.asciiRipple(el); if (r) r.pulse(i); },
      interval: (fn, ms) => setInterval(() => { if (!document.hidden) fn(); }, ms),
      onFocus: (fn) => leaf.listeners.push(fn),
      isFocused: () => state.last === leaf,
      focusTile: (id) => focusById(id),
    };
    let fn;
    try {
      fn = new AsyncFunction('root', 'ctx', joinLines(def.js));
    } catch (err) {
      tileError(leaf, err);
      return;
    }
    Promise.resolve().then(() => fn(leaf.body, ctx)).catch((err) => tileError(leaf, err));
  }

  function tileError(leaf, err) {
    console.error(`[tile ${leaf.id}]`, err);
    const pre = h('pre', 'tile-error', `tile "${leaf.id}" js error: ${err && err.message ? err.message : err}`);
    leaf.body.appendChild(pre);
  }

  /* ---------------- boot ---------------- */

  async function boot() {
    try {
      cfg = await fetchJSON('layout.json');
    } catch (err) {
      fatal(`layout.json failed to load\n${err.message}`);
      return;
    }
    cfg.site = Object.assign({ user: 'usr40k' }, cfg.site);
    hover = Object.assign(hover, cfg.hover);
    if (cfg.title) document.title = cfg.title;

    rootNode = buildNode(cfg.tree);
    if (!leaves.length) { fatal('layout.json: tree has no tiles'); return; }

    await Promise.all(leaves.map(async (leaf) => {
      try {
        leaf.def = await fetchJSON(`tiles/${leaf.id}.json`);
      } catch (err) {
        leaf.def = { title: leaf.id, html: `<pre class='tile-error'>${esc(err.message)}</pre>` };
      }
    }));

    leaves.forEach(mountTile);
    $wm.textContent = '';
    $wm.appendChild(rootNode.el);
    rootNode.apply();
    renderBar();
    leaves.forEach(runTileJs);

    window.addEventListener('hashchange', focusFromHash);
    focusFromHash();

    if (cfg.site.claim) fetch(cfg.site.claim, { mode: 'no-cors' }).catch(() => {});

    window.wm = { leaves, byId, focus: focusById, reset, state, hover };
    document.body.dataset.wm = 'ready';
    window.dispatchEvent(new CustomEvent('wm:ready'));
  }

  boot();
})();
