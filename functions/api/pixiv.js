// Cloudflare Pages Function: GET /api/pixiv
//
// Pixiv has no public, unauthenticated, CORS-enabled API, so this can't be
// called directly from the browser. This function runs server-side (on
// Cloudflare's edge, same place your static assets are served from) and:
//
//   GET /api/pixiv            -> { illusts: [{ id, title, url, image }] }
//   GET /api/pixiv?img=<url>  -> proxies an i.pximg.net image with the
//                                 Referer pixiv requires, since images
//                                 hotlinked without it are blocked.
//
// Notes / caveats (untested against the live pixiv.net API from this
// environment, so double-check after deploying):
//  - Uses pixiv's internal `/ajax/user/{id}/profile/*` endpoints. These are
//    undocumented and can change shape or start requiring auth at any time.
//  - Only surfaces public (non-R18, non-followers-only) works, since no
//    session cookie is sent.
//  - Cloudflare's edge IPs occasionally get rate-limited by pixiv; if you
//    see empty results in production but this works locally, that's likely
//    why. A PIXIV_PHPSESSID secret could be added later to authenticate.

const USER_ID = '109219544';
const HEADERS = {
  Referer: `https://www.pixiv.net/en/users/${USER_ID}`,
  'User-Agent': 'Mozilla/5.0 (compatible; usr40k-site/1.0)',
};

function json(obj, status = 200) {
  return new Response(JSON.stringify(obj), {
    status,
    headers: {
      'Content-Type': 'application/json',
      'Cache-Control': 'public, max-age=600',
      'Access-Control-Allow-Origin': '*',
    },
  });
}

async function proxyImage(rawUrl) {
  let decoded;
  try {
    decoded = decodeURIComponent(rawUrl);
  } catch (err) {
    return new Response('bad image url', { status: 400 });
  }

  if (!/^https:\/\/i\.pximg\.net\//.test(decoded)) {
    return new Response('image host not allowed', { status: 400 });
  }

  const imgRes = await fetch(decoded, { headers: HEADERS });
  if (!imgRes.ok) return new Response('upstream image error', { status: 502 });

  return new Response(imgRes.body, {
    headers: {
      'Content-Type': imgRes.headers.get('Content-Type') || 'image/jpeg',
      'Cache-Control': 'public, max-age=3600',
      'Access-Control-Allow-Origin': '*',
    },
  });
}

async function fetchLatestIllusts() {
  const profileRes = await fetch(
    `https://www.pixiv.net/ajax/user/${USER_ID}/profile/all?lang=en`,
    { headers: HEADERS },
  );
  if (!profileRes.ok) {
    throw new Error(`profile/all failed: ${profileRes.status}`);
  }
  const profileData = await profileRes.json();
  const illustIds = Object.keys((profileData.body && profileData.body.illusts) || {});

  const latestIds = illustIds
    .sort((a, b) => Number(b) - Number(a))
    .slice(0, 3);

  if (latestIds.length === 0) return [];

  const idsQuery = latestIds.map((id) => `ids[]=${id}`).join('&');
  const detailRes = await fetch(
    `https://www.pixiv.net/ajax/user/${USER_ID}/profile/illusts?${idsQuery}&lang=en`,
    { headers: HEADERS },
  );
  if (!detailRes.ok) {
    throw new Error(`profile/illusts failed: ${detailRes.status}`);
  }
  const detailData = await detailRes.json();
  const works = (detailData.body && detailData.body.works) || {};

  return latestIds
    .map((id) => works[id])
    .filter(Boolean)
    .map((w) => ({
      id: w.id,
      title: w.title,
      url: `https://www.pixiv.net/en/artworks/${w.id}`,
      image: `/api/pixiv?img=${encodeURIComponent(w.url)}`,
    }));
}

export async function onRequestGet({ request }) {
  const url = new URL(request.url);
  const img = url.searchParams.get('img');

  if (img) {
    try {
      return await proxyImage(img);
    } catch (err) {
      return new Response('image proxy failed', { status: 500 });
    }
  }

  try {
    const illusts = await fetchLatestIllusts();
    return json({ illusts });
  } catch (err) {
    return json({ error: 'pixiv proxy failed', message: String(err && err.message) }, 502);
  }
}
