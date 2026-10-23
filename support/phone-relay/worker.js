// The phone camera's relay: serves the camera page from a real HTTPS address, so a phone shows
// no certificate warning, and passes the page's WebRTC offer to Rec and Rec's answer back. The
// video then goes straight from the phone to Rec on the local network.
//
// Each Rec phone camera keeps a WebSocket open to the Durable Object of its room, named by a
// hash of the camera's token; the WebSocket hibernates while idle. The page posts its offer to
// the same room and gets Rec's answer in the response. Offers and answers are signed with the
// token, which the relay never sees.
import { DurableObject } from 'cloudflare:workers';
import page from '../../src/core/phone-page.html';

const kMaxSdp = 65536;
const kAnswerWithinMs = 10000;

const text = (status, body) => new Response(body, {status, headers: {'Content-Type': 'text/plain; charset=utf-8', 'Cache-Control': 'no-store'}});

export class Room extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.waiting = new Map();  // Offer ID -> resolve, while the page waits for Rec's answer.
    ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair('ping', 'pong'));
  }

  async fetch(request) {
    if (new URL(request.url).pathname.endsWith('/rec')) {
      if (request.headers.get('Upgrade') !== 'websocket') return text(426, 'Rec connects here with a WebSocket.');
      const [client, server] = Object.values(new WebSocketPair());
      this.ctx.acceptWebSocket(server);
      server.serializeAttachment({since: Date.now()});
      return new Response(null, {status: 101, webSocket: client});
    }
    // The newest connection is the Rec that opened the camera last (the recording, after the preview).
    const sockets = this.ctx.getWebSockets();
    if (!sockets.length) return text(409, 'Rec is not waiting for this phone.');
    const since = s => (s.deserializeAttachment() || {}).since || 0;
    const rec = sockets.reduce((a, b) => (since(b) > since(a) ? b : a));
    const id = crypto.randomUUID();
    const sdp = await request.text();
    const reply = new Promise(resolve => {
      this.waiting.set(id, resolve);
      setTimeout(() => resolve(null), kAnswerWithinMs);
    });
    rec.send(`offer\n${id}\n${request.headers.get('Rec-Mac') || ''}\n${sdp}`);
    const got = await reply;
    this.waiting.delete(id);
    if (!got) return text(504, 'Rec did not answer.');
    if (got.refused) return text(403, got.reason);
    return new Response(got.sdp, {status: 200, headers: {'Content-Type': 'application/sdp', 'Rec-Mac': got.mac, 'Cache-Control': 'no-store'}});
  }

  // "answer\nID\nMAC\nSDP" or "refuse\nID\nREASON" from Rec.
  webSocketMessage(socket, message) {
    if (typeof message !== 'string') return;
    const [kind, id, third] = message.split('\n', 3);
    if (third === undefined) return;
    const resolve = this.waiting.get(id);
    if (!resolve) return;
    const body = message.slice(kind.length + id.length + third.length + 3);
    if (kind === 'answer') resolve({sdp: body, mac: third});
    else if (kind === 'refuse') resolve({refused: true, reason: third});
  }

  webSocketClose(socket, code) {
    try { socket.close(code === 1005 ? 1000 : code, 'closed'); } catch (e) {}
  }
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === '/' && (request.method === 'GET' || request.method === 'HEAD')) {
      return new Response(page, {headers: {
        'Content-Type': 'text/html; charset=utf-8',
        'Content-Security-Policy': "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; media-src 'self' blob: mediastream:",
        'Referrer-Policy': 'no-referrer',
        'Cache-Control': 'no-cache',
      }});
    }
    const match = url.pathname.match(/^\/rooms\/([0-9a-f]{64})\/(offer|rec)$/);
    if (!match) return text(404, 'Not found');
    if (match[2] === 'offer') {
      if (request.method !== 'POST') return text(405, 'Post the offer.');
      if (Number(request.headers.get('Content-Length') || kMaxSdp + 1) > kMaxSdp) return text(413, 'The offer is too large.');
    }
    return env.ROOMS.get(env.ROOMS.idFromName(match[1])).fetch(request);
  },
};
