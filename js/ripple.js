/*
 * ascii ripple -- ported from usr40k/header injector.js (createASCIIShift).
 *
 * Hover a piece of text and a wave of scrambled characters spreads out from
 * the character under the cursor, settling back to the real text.
 *
 *   asciiRipple(el)            attach to an element that contains only text
 *   asciiRipple(el).pulse(i)   fire a wave from character index i (default: middle)
 *   asciiRipple.auto(root)     attach to every [data-ripple] under root
 *
 * Differences from the header version:
 *   - re-reads the element's text whenever a wave starts, so it keeps working
 *     when the text changes later (slideshow names, live data)
 *   - no width pinning: the site font is monospace so nothing can shift
 *   - does nothing under prefers-reduced-motion
 */
(function (global) {
  'use strict';

  const WAVE_THRESH = 3;
  const CHAR_MULT = 3;
  const ANIM_STEP = 40;
  const WAVE_BUF = 5;
  const MAX_WAVES = 6;

  const reduced = () => global.matchMedia && global.matchMedia('(prefers-reduced-motion: reduce)').matches;

  function asciiRipple(el, opts) {
    if (!el) return null;
    if (el.__ripple) return el.__ripple;
    if (el.children.length) return null; // text-only elements

    const cfg = Object.assign({
      dur: 1400,
      chars: '.,·-─~+:;=*"!?&#$@0123456789*',
      preserveSpaces: true,
      spread: 0.3,
    }, opts || {});

    let orig = el.textContent;
    let origChars = Array.from(orig);
    let waves = [];
    let raf = 0;
    let animating = false;
    let hovering = false;
    let cursorPos = 0;

    const now = () => performance.now();

    const nearestChar = (e) => {
      const node = el.firstChild;
      if (!node || node.nodeType !== 3) return cursorPos;
      const range = document.createRange();
      let best = 0;
      let bestDist = Infinity;
      for (let i = 0; i < node.length; i += 1) {
        range.setStart(node, i);
        range.setEnd(node, i + 1);
        const r = range.getBoundingClientRect();
        if (!r.width && !r.height) continue;
        const dx = e.clientX - (r.left + r.width / 2);
        const dy = e.clientY - (r.top + r.height / 2);
        const d = dx * dx + dy * dy;
        if (d < bestDist) { bestDist = d; best = i; }
      }
      return best;
    };

    const charAt = (i, t) => {
      let ch = origChars[i];
      for (const w of waves) {
        const age = t - w.t0;
        const prog = Math.min(age / cfg.dur, 1);
        const dist = Math.abs(i - w.pos);
        const maxDist = Math.max(w.pos, origChars.length - w.pos - 1);
        const rad = (prog * (maxDist + WAVE_BUF)) / cfg.spread;
        if (dist <= rad) {
          const intensity = Math.max(0, rad - dist);
          if (intensity <= WAVE_THRESH && intensity > 0) {
            ch = cfg.chars[(dist * CHAR_MULT + Math.floor(age / ANIM_STEP)) % cfg.chars.length];
          }
        }
      }
      return ch;
    };

    const frame = () => {
      const t = now();
      waves = waves.filter((w) => t - w.t0 < cfg.dur);
      if (!waves.length) {
        el.textContent = orig;
        animating = false;
        return;
      }
      el.textContent = origChars
        .map((c, i) => (cfg.preserveSpaces && c === ' ' ? ' ' : charAt(i, t)))
        .join('');
      raf = requestAnimationFrame(frame);
    };

    const startWave = (pos) => {
      if (reduced()) return;
      if (!animating) {
        // text may have changed since last time
        orig = el.textContent;
        origChars = Array.from(orig);
      }
      if (!origChars.length) return;
      waves.push({ pos: Math.max(0, Math.min(pos, origChars.length - 1)), t0: now() });
      if (waves.length > MAX_WAVES) waves.shift();
      if (!animating) {
        animating = true;
        raf = requestAnimationFrame(frame);
      }
    };

    el.addEventListener('pointerenter', (e) => {
      if (e.pointerType === 'touch') return;
      hovering = true;
      cursorPos = nearestChar(e);
      startWave(cursorPos);
    });
    el.addEventListener('pointermove', (e) => {
      if (!hovering || e.pointerType === 'touch') return;
      const p = nearestChar(e);
      if (p !== cursorPos) { cursorPos = p; startWave(p); }
    });
    el.addEventListener('pointerleave', () => { hovering = false; });

    const api = {
      pulse(i) {
        const len = Array.from(animating ? orig : el.textContent).length;
        startWave(i == null ? Math.floor(len / 2) : i);
      },
    };
    el.__ripple = api;
    return api;
  }

  asciiRipple.auto = function (root) {
    (root || document).querySelectorAll('[data-ripple]').forEach((el) => asciiRipple(el));
  };

  global.asciiRipple = asciiRipple;
})(window);
