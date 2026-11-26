/**
 * /api/report — Automated Telemetry & Bug Reporting Endpoint.
 *
 * Receives bug reports and diagnostic logs from the NexVR Launcher.
 * Stores reports in Cloudflare KV (bound as WAITLIST) and forwards
 * notifications to Discord if a webhook is configured.
 */

const MAX_REPORT_BYTES = 256 * 1024;
const ALLOWED_ADMIN_ORIGINS = new Set([
  'https://stereix-engine.pages.dev',
  'https://stereix.dev',
  'https://nexvr-engine.pages.dev',
]);

async function timingSafeEqualStr(a, b) {
  const enc = new TextEncoder();
  const [da, db] = await Promise.all([
    crypto.subtle.digest('SHA-256', enc.encode(String(a || ''))),
    crypto.subtle.digest('SHA-256', enc.encode(String(b || ''))),
  ]);
  const ba = new Uint8Array(da);
  const bb = new Uint8Array(db);
  let diff = String(a || '').length ^ String(b || '').length;
  for (let i = 0; i < ba.length; i++) {
    diff |= ba[i] ^ bb[i];
  }
  return diff === 0;
}

async function hashClientIp(ip, salt) {
  if (!ip) return 'unknown';
  const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(`${salt}:${ip}`));
  return Array.from(new Uint8Array(digest))
    .slice(0, 8)
    .map(b => b.toString(16).padStart(2, '0'))
    .join('');
}

const json = (data, status = 200, corsOrigin = '*') => {
  const headers = {
    'content-type': 'application/json',
    'cache-control': 'no-store',
    'access-control-allow-methods': 'GET, POST, OPTIONS',
    'access-control-allow-headers': 'Content-Type, Authorization, X-Admin-Key',
  };
  if (corsOrigin) {
    headers['access-control-allow-origin'] = corsOrigin;
    if (corsOrigin !== '*') headers['vary'] = 'Origin';
  }
  return new Response(JSON.stringify(data), { status, headers });
};

function resolveAdminCorsOrigin(request, env) {
  const origin = request.headers.get('origin');
  if (!origin) return null;
  const reqOrigin = new URL(request.url).origin;
  if (origin === reqOrigin || ALLOWED_ADMIN_ORIGINS.has(origin) || (env.ADMIN_ORIGIN && origin === env.ADMIN_ORIGIN)) {
    return origin;
  }
  return null;
}

export async function onRequestOptions({ request, env }) {
  const reqMethod = (request.headers.get('access-control-request-method') || 'POST').toUpperCase();
  const corsOrigin = reqMethod === 'GET' ? resolveAdminCorsOrigin(request, env) : '*';
  const headers = {
    'access-control-allow-methods': 'GET, POST, OPTIONS',
    'access-control-allow-headers': 'Content-Type, Authorization, X-Admin-Key',
    'access-control-max-age': '86400',
  };
  if (corsOrigin) {
    headers['access-control-allow-origin'] = corsOrigin;
    if (corsOrigin !== '*') headers['vary'] = 'Origin';
  }
  return new Response(null, { status: 204, headers });
}

export async function onRequestPost({ request, env }) {
  if (Number(request.headers.get('content-length')) > MAX_REPORT_BYTES) {
    return json({ error: 'Report payload too large.' }, 413);
  }

  const reader = request.body?.getReader();
  if (!reader) return json({ error: 'Malformed JSON payload.' }, 400);

  let body;
  try {
    const decoder = new TextDecoder();
    let raw = '';
    let size = 0;
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      size += value.byteLength;
      if (size > MAX_REPORT_BYTES) {
        await reader.cancel();
        return json({ error: 'Report payload too large.' }, 413);
      }
      raw += decoder.decode(value, { stream: true });
    }
    body = JSON.parse(raw + decoder.decode());
    if (!body || typeof body !== 'object' || Array.isArray(body)) {
      return json({ error: 'Malformed JSON payload.' }, 400);
    }
  } catch {
    return json({ error: 'Malformed JSON payload.' }, 400);
  } finally {
    reader.releaseLock();
  }

  // M-12 / MED-08: Rate limiting keyed on salted hash of client IP (never store raw clientIp in KV)
  const rawClientIp = request.headers.get('cf-connecting-ip') || '';
  const ipHashSalt = env.IP_HASH_SALT || env.REPORTS_ADMIN_KEY || 'stereix-telemetry-ip-salt';
  const clientIpHash = await hashClientIp(rawClientIp, ipHashSalt);
  if (rawClientIp && env.WAITLIST) {
    const rlKey = `RL:REPORT:${clientIpHash}`;
    try {
      const count = await env.WAITLIST.get(rlKey);
      if (count && Number(count) >= 5) {
        return json({ error: 'Too many reports submitted. Please wait a few minutes before submitting another report.' }, 429);
      }
      const nextCount = (Number(count) || 0) + 1;
      await env.WAITLIST.put(rlKey, String(nextCount), { expirationTtl: 60 });
    } catch {}
  }

  const timestamp = Date.now();
  const randomSuffix = Math.random().toString(36).slice(2, 8);
  const reportId = `rep_${timestamp}_${randomSuffix}`;

  const report = {
    id: reportId,
    timestamp,
    date: new Date(timestamp).toISOString(),
    gameId: String(body.gameId || 'manual_session').slice(0, 100),
    gameName: String(body.gameName || body.gameId || 'Manual Report').slice(0, 150),
    engineVersion: String(body.engineVersion || '0.1.90').slice(0, 30),
    status: String(body.status || 'manual_report').slice(0, 30),
    vrHeadset: String(body.vrHeadset || 'Unknown HMD').slice(0, 100),
    vrRuntime: String(body.vrRuntime || 'OpenXR').slice(0, 100),
    message: String(body.message || '').slice(0, 2000),
    userNote: String(body.userNote || '').slice(0, 2000),
    systemSpecs: String(body.systemSpecs || '').slice(0, 2000),
    logSnippet: String(body.logContent || body.logSnippet || '').slice(0, 20000),
    clientIpHash,
    country: request.headers.get('cf-ipcountry') || 'Unknown',
  };

  // 1. Store in Cloudflare KV if bound (MED-08: use per-report KV metadata instead of non-atomic REPORTS_INDEX RMW)
  if (env.WAITLIST) {
    try {
      const summaryMeta = {
        id: reportId,
        timestamp,
        date: report.date,
        gameName: report.gameName.slice(0, 100),
        status: report.status,
        vrHeadset: report.vrHeadset.slice(0, 60),
        engineVersion: report.engineVersion,
        userNote: report.userNote.slice(0, 200),
        hasLogs: !!report.logSnippet,
      };
      // Store full report with 30-day TTL (2,592,000 seconds) and lightweight metadata for race-free KV list()
      await env.WAITLIST.put(`REPORT:${reportId}`, JSON.stringify(report), {
        expirationTtl: 2592000,
        metadata: summaryMeta,
      });
    } catch (err) {
      console.warn('Failed to store report in KV:', err);
    }
  }

  // 2. Forward to Discord Webhook if configured
  const webhookUrl = env.DISCORD_WEBHOOK_URL;
  if (webhookUrl && webhookUrl.startsWith('https://discord.com/api/webhooks/')) {
    try {
      const isError = report.status === 'error';
      const statusEmoji = isError ? '🚨' : '📋';
      const embedColor = isError ? 0xef4444 : 0xaa3bff;

      const fields = [
        { name: 'Game', value: `\`${report.gameName}\``, inline: true },
        { name: 'Engine Version', value: `\`v${report.engineVersion}\``, inline: true },
        { name: 'VR Headset / Runtime', value: `${report.vrHeadset} (${report.vrRuntime})`, inline: false },
      ];

      if (report.userNote) {
        fields.push({ name: 'Tester Note', value: report.userNote, inline: false });
      }

      if (report.message && report.message !== report.userNote) {
        fields.push({ name: 'Details', value: `\`\`\`\n${report.message.slice(0, 500)}\n\`\`\``, inline: false });
      }

      if (report.logSnippet) {
        const lastLines = report.logSnippet.trim().split('\n').slice(-15).join('\n');
        fields.push({ name: 'Recent Log Snippet', value: `\`\`\`\n${lastLines.slice(0, 950)}\n\`\`\``, inline: false });
      }

      const discordPayload = {
        embeds: [
          {
            title: `${statusEmoji} Stereix Beta Report [${report.id}]`,
            color: embedColor,
            fields,
            footer: { text: `Reported from ${report.country} · Stereix Telemetry` },
            timestamp: new Date(timestamp).toISOString(),
          },
        ],
      };

      await fetch(webhookUrl, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(discordPayload),
      });
    } catch (err) {
      console.warn('Discord webhook forwarding error:', err);
    }
  }

  return json({
    success: true,
    reportId,
    message: 'Report received and recorded in engineering telemetry.',
  });
}

export async function onRequestGet({ request, env }) {
  // MED-08: Never return wildcard CORS on privileged admin telemetry reads
  const adminCorsOrigin = resolveAdminCorsOrigin(request, env);

  // Enforce administrative authorization to protect user telemetry & bug reports
  const authHeader = request.headers.get('authorization') || '';
  const apiKey = request.headers.get('x-admin-key') || '';
  const knownCompromised = 'nexvr_admin_telemetry_secret_2026';

  const bearerToken = authHeader.startsWith('Bearer ') ? authHeader.slice(7) : '';
  if (
    (bearerToken && (await timingSafeEqualStr(bearerToken, knownCompromised))) ||
    (apiKey && (await timingSafeEqualStr(apiKey, knownCompromised)))
  ) {
    return json(
      { error: 'Unauthorized. The provided administrative credential has been revoked.' },
      401,
      adminCorsOrigin
    );
  }

  const adminKeys = [env.REPORTS_ADMIN_KEY, env.ADMIN_API_KEY].filter(
    key => typeof key === 'string' && key.trim().length > 0 && key !== knownCompromised
  );

  // MED-08: Constant-time credential verification
  let isAuthorized = false;
  for (const key of adminKeys) {
    if (
      (bearerToken && (await timingSafeEqualStr(bearerToken, key))) ||
      (apiKey && (await timingSafeEqualStr(apiKey, key)))
    ) {
      isAuthorized = true;
    }
  }

  if (!isAuthorized) {
    return json(
      { error: 'Unauthorized. Administrative credentials required to access diagnostic reports.' },
      401,
      adminCorsOrigin
    );
  }

  const url = new URL(request.url);
  const reportId = url.searchParams.get('id');

  if (!env.WAITLIST) {
    return json({ error: 'Storage namespace is not configured.' }, 503, adminCorsOrigin);
  }

  // Fetch individual report
  if (reportId) {
    if (!/^rep_\d+_[a-z0-9]{1,16}$/i.test(reportId)) {
      return json({ error: 'Invalid report ID' }, 400, adminCorsOrigin);
    }
    const raw = await env.WAITLIST.get(`REPORT:${reportId}`);
    if (!raw) return json({ error: 'Report not found' }, 404, adminCorsOrigin);
    return json(JSON.parse(raw), 200, adminCorsOrigin);
  }

  // MED-08: Race-free report listing using KV prefix list + per-key metadata (paginated so >100 reports never hide newest entries)
  let reports = [];
  try {
    if (typeof env.WAITLIST.list === 'function') {
      let cursor = undefined;
      for (let page = 0; page < 5; page++) {
        const listed = await env.WAITLIST.list({ prefix: 'REPORT:', limit: 1000, cursor });
        for (const entry of listed.keys || []) {
          if (entry.metadata && entry.metadata.id) {
            reports.push(entry.metadata);
          }
        }
        if (listed.list_complete || !listed.cursor) break;
        cursor = listed.cursor;
      }
      reports.sort((a, b) => (b.timestamp || 0) - (a.timestamp || 0));
      reports = reports.slice(0, 40);
    }
    if (reports.length === 0) {
      const rawIndex = await env.WAITLIST.get('REPORTS_INDEX');
      if (rawIndex) reports = JSON.parse(rawIndex);
    }
  } catch {}

  return json({
    count: reports.length,
    reports,
  }, 200, adminCorsOrigin);
}

