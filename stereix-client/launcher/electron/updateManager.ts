import { app, ipcMain, shell } from 'electron';
import * as path from 'path';
import * as fs from 'fs';
import * as crypto from 'crypto';
import { assertTrustedIpcSender, resolveWithinRoot } from './utils';

export interface UpdateManifest {
  engineVersion: string;
  timestamp: number;
  changelog: string;
  features?: string[];
  fixes?: string[];
  files: string[];
  hashes?: Record<string, string>;
  signature?: string;
}

export const OTA_PINNED_PUBLIC_KEY = `-----BEGIN PUBLIC KEY-----
MCowBQYDK2VwAyEAeSfbx4ROSXaiiYejnxCdmI0A6eRd8/EK0CBAVqGapL0=
-----END PUBLIC KEY-----`;

export function getCanonicalManifestData(manifest: UpdateManifest): Buffer {
  const files = Array.isArray(manifest.files) ? [...manifest.files].sort() : [];
  const hashes: Record<string, string> = {};
  if (manifest.hashes) {
    for (const key of Object.keys(manifest.hashes).sort()) {
      hashes[key] = manifest.hashes[key];
    }
  }
  return Buffer.from(JSON.stringify({
    engineVersion: manifest.engineVersion,
    timestamp: manifest.timestamp,
    files,
    hashes,
  }));
}

export function verifyManifestSignature(manifest: UpdateManifest, pinnedKey: string = OTA_PINNED_PUBLIC_KEY): boolean {
  if (!manifest.signature || typeof manifest.signature !== 'string' || manifest.signature.length < 64) {
    return false;
  }
  try {
    const canonical = getCanonicalManifestData(manifest);
    return crypto.verify(null, canonical, pinnedKey, Buffer.from(manifest.signature, 'hex'));
  } catch {
    return false;
  }
}

export interface UpdateStatus {
  checking: boolean;
  hasUpdate: boolean;
  updated: boolean;
  version: string;
  changelog?: string;
  features?: string[];
  fixes?: string[];
  error?: string;
}

const MANIFEST_URLS = [
  'https://stereix-engine.pages.dev/updates/manifest.json',
  'https://raw.githubusercontent.com/sathishssj3/Stereix-Engine/main/updates/manifest.json',
  'https://raw.githubusercontent.com/sathishssj3/Stereix-Engine-Releases/main/updates/manifest.json',
  'https://cdn.jsdelivr.net/gh/sathishssj3/Stereix-Engine@main/updates/manifest.json',
  'https://fastly.jsdelivr.net/gh/sathishssj3/Stereix-Engine@main/updates/manifest.json',
  'https://nexvr-engine.pages.dev/updates/manifest.json',
  'https://raw.githubusercontent.com/sathishssj3/NexVR-Engine/main/updates/manifest.json',
  'https://raw.githubusercontent.com/sathishssj3/NexVR-Engine-Releases/main/updates/manifest.json',
];

export function getUpdatesDir(): string {
  const userData = app?.getPath ? app.getPath('userData') : path.join(process.env.APPDATA || process.cwd(), 'Stereix Engine');
  const dir = path.join(userData, 'updates');
  if (!fs.existsSync(dir)) {
    fs.mkdirSync(dir, { recursive: true });
  }
  return dir;
}

export function parseSemver(v: string): number[] {
  const clean = v.replace(/^v/i, '').trim();
  const parts = clean.split('.').map(p => Number.parseInt(p, 10) || 0);
  while (parts.length < 3) parts.push(0);
  return parts;
}

export function compareSemver(v1: string, v2: string): number {
  const p1 = parseSemver(v1);
  const p2 = parseSemver(v2);
  for (let i = 0; i < 3; i++) {
    if (p1[i] > p2[i]) return 1;
    if (p1[i] < p2[i]) return -1;
  }
  return 0;
}

export function getAppVersion(): string {
  if (app?.getVersion) {
    try {
      const v = app.getVersion();
      if (v) return v;
    } catch {}
  }
  try {
    const pkgPath = path.join(__dirname, '..', 'package.json');
    if (fs.existsSync(pkgPath)) {
      const pkg = JSON.parse(fs.readFileSync(pkgPath, 'utf-8'));
      if (pkg.version) return pkg.version;
    }
  } catch {}
  try {
    const rootPkgPath = path.join(process.cwd(), 'package.json');
    if (fs.existsSync(rootPkgPath)) {
      const pkg = JSON.parse(fs.readFileSync(rootPkgPath, 'utf-8'));
      if (pkg.version) return pkg.version;
    }
  } catch {}
  return '0.1.29';
}

export function getLocalManifest(): UpdateManifest | null {
  try {
    const file = path.join(getUpdatesDir(), 'installed_manifest.json');
    if (fs.existsSync(file)) {
      const manifest = JSON.parse(fs.readFileSync(file, 'utf-8'));
      if (manifest && verifyManifestSignature(manifest)) {
        return manifest;
      }
      console.warn('[SECURITY] Local installed_manifest.json failed cryptographic signature check. Rejecting untrusted manifest.');
      return null;
    }
    const legacy = path.join(getUpdatesDir(), 'manifest.json');
    if (fs.existsSync(legacy)) {
      const manifest = JSON.parse(fs.readFileSync(legacy, 'utf-8'));
      if (manifest && verifyManifestSignature(manifest)) {
        return manifest;
      }
      console.warn('[SECURITY] Local legacy manifest.json failed cryptographic signature check. Rejecting untrusted manifest.');
      return null;
    }
  } catch {}
  return null;
}

export function purgeStaleOtaCacheIfAppNewer(): void {
  try {
    const local = getLocalManifest();
    const appVer = getAppVersion();
    if (!appVer) return;

    const updatesDir = getUpdatesDir();
    const profilesDir = path.join(updatesDir, 'profiles');
    const shadersDir = path.join(updatesDir, 'shaders');
    const filesToPurge = [
      'vrinject.dll',
      'vr-inject-cli.exe',
      'onnxruntime.dll',
      'DirectML.dll',
      'openxr_loader.dll',
      'vrinject.json',
      'installed_manifest.json',
      'manifest.json',
    ];

    // HIGH-10: Also detect unverified/tampered OTA artifacts when local manifest is missing or failed signature verification
    const hasUntrustedArtifacts = !local && (
      filesToPurge.some(f => fs.existsSync(path.join(updatesDir, f))) ||
      fs.existsSync(shadersDir) ||
      fs.existsSync(profilesDir)
    );

    // If in dev mode, if local cached OTA belongs to an older version than the installed app,
    // or if unverified/tampered OTA files exist without a valid signed manifest, purge the cache.
    if ((!app?.isPackaged) || (local && compareSemver(appVer, local.engineVersion) > 0) || hasUntrustedArtifacts) {
      console.info(`[UpdateManager] Purging stale/untrusted OTA cache (devMode=${!app?.isPackaged}, appVer=${appVer}, localVer=${local?.engineVersion}, untrusted=${hasUntrustedArtifacts}).`);
      for (const f of filesToPurge) {
        const fp = path.join(updatesDir, f);
        if (fs.existsSync(fp)) {
          try { fs.unlinkSync(fp); } catch {}
        }
      }
      if (fs.existsSync(shadersDir)) {
        try { fs.rmSync(shadersDir, { recursive: true, force: true }); } catch {}
      }
      if (fs.existsSync(profilesDir)) {
        try { fs.rmSync(profilesDir, { recursive: true, force: true }); } catch {}
      }
      // Clean up any leftover staging directories from interrupted updates
      try {
        for (const entry of fs.readdirSync(updatesDir)) {
          if (entry.startsWith('.staging_')) {
            fs.rmSync(path.join(updatesDir, entry), { recursive: true, force: true });
          }
        }
      } catch {}
    }
  } catch (err) {
    console.warn('[UpdateManager] Error during stale OTA cache check:', err);
  }
}

// Purge obsolete OTA cache on Electron main-process startup only (never wipe real AppData during unit/static test imports)
if (app && typeof app.getPath === 'function') {
  purgeStaleOtaCacheIfAppNewer();
}

async function fetchWithTimeout(url: string, timeoutMs = 8000): Promise<Response> {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const separator = url.includes('?') ? '&' : '?';
    const cacheBustedUrl = `${url}${separator}_t=${Date.now()}`;
    const res = await fetch(cacheBustedUrl, {
      cache: 'no-store',
      headers: {
        'User-Agent': 'Stereix-Launcher',
        'Cache-Control': 'no-cache, no-store, must-revalidate',
        'Pragma': 'no-cache',
      },
      signal: controller.signal,
    });
    return res;
  } finally {
    clearTimeout(timer);
  }
}

async function fetchRemoteManifest(): Promise<{ manifest: UpdateManifest; baseUrl: string } | null> {
  for (const url of MANIFEST_URLS) {
    try {
      console.info(`[UpdateManager] Checking update channel: ${url}`);
      const res = await fetchWithTimeout(url, 15000);
      if (res.status === 404) {
        continue;
      }
      if (!res.ok) {
        throw new Error(`HTTP ${res.status}`);
      }
      const manifest = await res.json() as UpdateManifest;
      // HIGH-10: Verify Ed25519 signature inside the mirror loop so a poisoned/stale mirror does not block failover
      if (
        !manifest ||
        typeof manifest.engineVersion !== 'string' ||
        !Array.isArray(manifest.files) ||
        !manifest.hashes ||
        typeof manifest.hashes !== 'object' ||
        !verifyManifestSignature(manifest)
      ) {
        console.warn(`[UpdateManager] Mirror ${url} returned an invalid or untrusted manifest signature; failing over to next mirror.`);
        continue;
      }
      const baseUrl = url.substring(0, url.lastIndexOf('/') + 1);
      return { manifest, baseUrl };
    } catch (err: any) {
      console.warn(`[UpdateManager] Channel fetch failed for ${url}:`, err?.message || err);
    }
  }
  return null;
}

async function downloadFileWithFallback(
  fileName: string,
  baseUrls: string[],
  destPath: string,
  expectedHash: string
): Promise<void> {
  if (!expectedHash || typeof expectedHash !== 'string' || expectedHash.trim().length !== 64) {
    throw new Error(`Security violation: missing or invalid 64-char SHA-256 hash for OTA asset ${fileName}. OTA updates fail-closed.`);
  }
  let lastErr: Error | null = null;
  const normalizedFile = fileName.replace(/\\/g, '/');
  for (const base of baseUrls) {
    const fileUrl = `${base.replace(/\/+$/, '')}/${normalizedFile}`;
    try {
      const res = await fetchWithTimeout(fileUrl, 30000);
      if (!res.ok) {
        throw new Error(`HTTP ${res.status} from ${fileUrl}`);
      }
      const buffer = Buffer.from(await res.arrayBuffer());
      let verifiedBuffer: Buffer = buffer;

      // Cryptographic integrity verification against SHA-256 manifest hash
      const actualHash = crypto.createHash('sha256').update(buffer).digest('hex');
      if (actualHash.toLowerCase() !== expectedHash.toLowerCase()) {
        // Normalize text line endings (\r\n -> \n and \n -> \r\n) to prevent false-positive CRLF/LF rejections on text assets
        // HIGH-10: Write the exact normalized bytes whose SHA-256 matched expectedHash so deploy-time hash checks succeed.
        const asString = buffer.toString('utf-8');
        const lfBuf = Buffer.from(asString.replace(/\r\n/g, '\n'), 'utf-8');
        const hashLf = crypto.createHash('sha256').update(lfBuf).digest('hex');
        if (hashLf.toLowerCase() === expectedHash.toLowerCase()) {
          verifiedBuffer = lfBuf;
        } else {
          const crlfBuf = Buffer.from(asString.replace(/\r\n/g, '\n').replace(/\n/g, '\r\n'), 'utf-8');
          const hashCrlf = crypto.createHash('sha256').update(crlfBuf).digest('hex');
          if (hashCrlf.toLowerCase() === expectedHash.toLowerCase()) {
            verifiedBuffer = crlfBuf;
          } else {
            throw new Error(
              `Integrity check failed for ${fileName}: expected SHA-256 ${expectedHash}, got ${actualHash}`
            );
          }
        }
      }

      const tempPath = `${destPath}.tmp`;
      fs.writeFileSync(tempPath, verifiedBuffer);
      fs.renameSync(tempPath, destPath);
      return;
    } catch (err: any) {
      lastErr = err;
      console.warn(`[UpdateManager] Failed downloading ${fileName} from ${base}:`, err?.message || err);
    }
  }
  throw lastErr || new Error(`Failed to download ${fileName}`);
}

export async function checkForEngineHotfix(): Promise<UpdateStatus> {
  try {
    const remoteData = await fetchRemoteManifest();
    if (!remoteData) {
      console.warn('[UpdateManager] Unable to reach update servers, keeping active local build.');
      const local = getLocalManifest();
      const fallbackVer = local?.engineVersion || getAppVersion();
      return {
        checking: false,
        hasUpdate: false,
        updated: !!local,
        version: fallbackVer,
        changelog: local?.changelog,
        features: local?.features,
        fixes: local?.fixes,
        error: 'Unable to connect to update servers. Please check your network connection.',
      };
    }

    const { manifest: remote, baseUrl } = remoteData;
    const local = getLocalManifest();
    const appVer = getAppVersion();
    const activeVersion = local?.engineVersion || appVer;

    // Cryptographic authenticity verification: reject unauthenticated or tampered manifests
    if (!verifyManifestSignature(remote)) {
      console.error('[UpdateManager] Security rejection: Remote manifest failed cryptographic signature verification against pinned publisher key.');
      return {
        checking: false,
        hasUpdate: false,
        updated: !!local,
        version: activeVersion,
        error: 'Security rejection: Untrusted or unsigned OTA update manifest. Update rejected.',
      };
    }

    const versionComp = compareSemver(remote.engineVersion, activeVersion);

    // Prevent older remote hotfix from downgrading a newer packaged installation
    if (versionComp < 0) {
      console.info(`[UpdateManager] Active version v${activeVersion} is newer than remote v${remote.engineVersion}. Skipping hotfix.`);
      return {
        checking: false,
        hasUpdate: false,
        updated: !!local,
        version: activeVersion,
        changelog: local?.changelog || 'Bundled release is up-to-date',
        features: local?.features,
        fixes: local?.fixes,
      };
    }

    // If version is identical: only proceed if local manifest exists and remote has a newer post-release hotfix timestamp
    // On fresh install (!local), the bundled app already possesses all binaries for this release
    if (versionComp === 0) {
      const activeTimestamp = local?.timestamp || 0;
      if (!local || remote.timestamp <= activeTimestamp) {
        console.info(`[UpdateManager] Active version v${activeVersion} is already up-to-date.`);
        return {
          checking: false,
          hasUpdate: false,
          updated: !!local,
          version: activeVersion,
          changelog: local?.changelog || remote.changelog || 'Bundled release is up-to-date',
          features: local?.features || remote.features,
          fixes: local?.fixes || remote.fixes,
        };
      }
    }

    if (!local || compareSemver(remote.engineVersion, local.engineVersion) > 0 || remote.timestamp > local.timestamp) {
      console.info(`[UpdateManager] New engine hotfix available: v${remote.engineVersion} (${remote.changelog})`);
      const updatesDir = getUpdatesDir();

      const candidateBases = [
        baseUrl,
        'https://cdn.jsdelivr.net/gh/sathishssj3/Stereix-Engine@main/updates/',
        'https://fastly.jsdelivr.net/gh/sathishssj3/Stereix-Engine@main/updates/',
        'https://raw.githubusercontent.com/sathishssj3/Stereix-Engine/main/updates/',
        'https://raw.githubusercontent.com/sathishssj3/Stereix-Engine-Releases/main/updates/',
        'https://cdn.jsdelivr.net/gh/sathishssj3/NexVR-Engine@main/updates/',
        'https://fastly.jsdelivr.net/gh/sathishssj3/NexVR-Engine@main/updates/',
        'https://raw.githubusercontent.com/sathishssj3/NexVR-Engine/main/updates/',
        'https://raw.githubusercontent.com/sathishssj3/NexVR-Engine-Releases/main/updates/',
      ];

      // HIGH-10: Stage and verify ALL files in an isolated staging directory before promoting atomically
      // so partial network/hash failures never leave mixed-version binaries or shaders in updatesDir.
      if (!remote.hashes || typeof remote.hashes !== 'object') {
        throw new Error('Integrity check rejected: OTA manifest is missing the hashes dictionary.');
      }
      const stagingDir = path.join(updatesDir, `.staging_${Date.now()}_${process.pid}`);
      fs.mkdirSync(stagingDir, { recursive: true });
      const stagedFiles: Array<{ file: string; stagedPath: string; finalDest: string }> = [];
      try {
        for (const file of remote.files) {
          const finalDest = resolveWithinRoot(updatesDir, file);
          const stagedPath = resolveWithinRoot(stagingDir, file);
          const stagedParent = path.dirname(stagedPath);
          if (!fs.existsSync(stagedParent)) {
            fs.mkdirSync(stagedParent, { recursive: true });
          }
          const expectedHash = remote.hashes[file];
          if (!expectedHash || typeof expectedHash !== 'string' || expectedHash.trim().length !== 64) {
            throw new Error(`Integrity check rejected: file ${file} lacks a valid SHA-256 hash in the OTA manifest.`);
          }
          await downloadFileWithFallback(file, candidateBases, stagedPath, expectedHash);
          stagedFiles.push({ file, stagedPath, finalDest });
          console.info(`[UpdateManager] Downloaded & verified staged hotfix file: ${file}`);
        }

        // Promote verified staged files into updatesDir
        for (const { stagedPath, finalDest } of stagedFiles) {
          const parentDir = path.dirname(finalDest);
          if (!fs.existsSync(parentDir)) {
            fs.mkdirSync(parentDir, { recursive: true });
          }
          const promoteTmp = `${finalDest}.promote_${process.pid}.tmp`;
          fs.copyFileSync(stagedPath, promoteTmp);
          fs.renameSync(promoteTmp, finalDest);
        }

        // Write installed manifest LAST via atomic rename
        const manifestDest = path.join(updatesDir, 'installed_manifest.json');
        const manifestTmp = `${manifestDest}.tmp`;
        fs.writeFileSync(manifestTmp, JSON.stringify(remote, null, 2));
        fs.renameSync(manifestTmp, manifestDest);
        console.info('[UpdateManager] Hotfix installation complete!');
      } finally {
        try {
          fs.rmSync(stagingDir, { recursive: true, force: true });
        } catch {}
      }

      return {
        checking: false,
        hasUpdate: true,
        updated: true,
        version: remote.engineVersion,
        changelog: remote.changelog,
        features: remote.features,
        fixes: remote.fixes,
      };
    } else {
      return {
        checking: false,
        hasUpdate: false,
        updated: true,
        version: local.engineVersion,
        changelog: local.changelog,
        features: local.features,
        fixes: local.fixes,
      };
    }
  } catch (err: any) {
    console.warn('[UpdateManager] Hotfix check encountered an error:', err.message);
    const local = getLocalManifest();
    return {
      checking: false,
      hasUpdate: false,
      updated: !!local,
      version: local?.engineVersion || getAppVersion(),
      changelog: local?.changelog,
      features: local?.features,
      fixes: local?.fixes,
      error: err.message,
    };
  }
}

// IPC Handlers
if (ipcMain) {
  ipcMain.handle('update:check', async (event) => {
    assertTrustedIpcSender(event);
    return await checkForEngineHotfix();
  });

  ipcMain.handle('update:getStatus', async (event) => {
    assertTrustedIpcSender(event);
    const local = getLocalManifest();
    const appVer = getAppVersion();
    const effectiveVersion = (local && compareSemver(local.engineVersion, appVer) > 0) 
      ? local.engineVersion 
      : appVer;
    return {
      version: effectiveVersion,
      timestamp: local?.timestamp || 0,
      changelog: local?.changelog || '',
      features: local?.features || [],
      fixes: local?.fixes || [],
    };
  });

  ipcMain.handle('update:openFolder', async (event) => {
    assertTrustedIpcSender(event);
    const dir = getUpdatesDir();
    if (shell?.openPath) shell.openPath(dir);
  });
}
