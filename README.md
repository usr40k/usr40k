# usr40k.dev

A tiling window manager disguised as a homepage. tty font, json-driven tiles.

## How it behaves

- Tiles sit in a split tree (`layout.json`). Hover a tile and it **grows**.
- A tile only changes size when *another* tile gets focus. Leaving a tile, or
  moving around inside it, does nothing, so the layout depends on the **path**
  your cursor took (recent tiles stay big, old ones fade back), not where it is.
- `esc` resets. `tab` or `alt+h/j/k/l` move focus. Small screens get a plain stack.
- Text titles ripple in ascii when hovered (`js/ripple.js`, ported from the header repo).

## Files

| path | what |
| --- | --- |
| `layout.json` | site info, hover tuning, status bar, the split tree |
| `tiles/<id>.json` | one tile = everything it needs |
| `js/wm.js`, `js/ripple.js`, `style.css` | engine |
| `data/follows.json` | twitch follows for the streams tile (auto-generated, optional) |
| `media/fonts/` | TTY16 font (Terminus, OFL) |

## Tile json

```json
{
  "title": "shown in the border",
  "accent": "#00ffff",
  "min": { "w": 176, "h": 96 },
  "config": { "anything": "your js reads via ctx.config" },
  "html": ["lines", "joined with newlines"],
  "css": ["scoped to this tile (native css nesting)"],
  "cssGlobal": ["optional, unscoped: @keyframes etc"],
  "js": ["async function body with (root, ctx)"]
}
```

- Add a tile: create `tiles/foo.json`, then put `{ "tile": "foo", "weight": 1 }` in the tree.
- `ctx`: `root`, `tile`, `config`, `site`, `esc`, `fetchJSON`, `ripple(el)`, `pulse(el)`,
  `interval(fn, ms)`, `onFocus(fn)`, `focusTile(id)`.
- `data-user` fills in the site user; `data-ripple` adds the ripple;
  `data-focus-tile="id"` on a link focuses that tile.
- `grow` / `max` in `layout.json` are fractions of the total base weight.

## Streams tile

Shows a slideshow of streamers (live preview when live, avatar when offline; live
status comes from decapi.me). Twitch has no public follow list, so either:

1. list logins by hand in `tiles/streams.json` -> `config.streamers`, or
2. add repo secrets `TWITCH_CLIENT_ID`, `TWITCH_CLIENT_SECRET`, `TWITCH_REFRESH_TOKEN`
   (scope `user:read:follows`) and the **Refresh Twitch follows** workflow keeps
   `data/follows.json` current.

## Font

`TTY16` is Terminus 16px (the NixOS default `Lat2-Terminus16`) converted to a
pixel-grid outline font, renamed per the OFL. Keep text at 16/32px to stay crisp.
Rebuild with `tools/bdf2ttf.py`. License: `media/fonts/OFL.txt`.
