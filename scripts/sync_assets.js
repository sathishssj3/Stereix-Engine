// ==============================================================================
// sync_assets.js - Single-Source-of-Truth Asset Synchronization Pipeline
// Synchronizes binaries, shaders, and profiles across dev, release, OTA staging, and docs.
// Recomputes SHA-256 hashes & Ed25519 signature for updates/manifest.json automatically.
// CRIT-03 Guard: Verifies updates/vrinject.dll SHA-256 matches the UTF-16LE
// EXPECTED_DLL_HASH embedded inside updates/vr-inject-cli.exe (never mutates DLL post-link).
// ==============================================================================

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const rootDir = path.resolve(__dirname, '..');
const clientDir = fs.existsSync(path.join(rootDir, 'stereix-client'))
  ? path.join(rootDir, 'stereix-client')
  : path.join(rootDir, 'nexvr-client');
const docsDir = fs.existsSync(path.join(rootDir, 'stereix-docs'))
  ? path.join(rootDir, 'stereix-docs')
  : path.join(rootDir, 'nexvr-docs');
const launcherPkgFile = path.join(clientDir, 'launcher', 'package.json');
const manifestFile = path.join(rootDir, 'updates', 'manifest.json');
const docsManifestFile = path.join(docsDir, 'landing-page', 'public', 'updates', 'manifest.json');

console.log('=== Stereix Engine Asset Synchronization Pipeline ===');

// 1. Read current version from package.json
let currentVersion = '0.1.122';
try {
  if (fs.existsSync(launcherPkgFile)) {
    const pkg = JSON.parse(fs.readFileSync(launcherPkgFile, 'utf-8'));
    if (pkg.version) currentVersion = pkg.version;
  }
} catch (e) {
  console.warn('Could not read launcher package.json version:', e.message);
}
console.log(`[+] Current Engine Version: v${currentVersion}`);

function safeWriteFileSync(filePath, data) {
  try {
    fs.writeFileSync(filePath, data, 'utf-8');
  } catch (err) {
    const tmp = filePath + '.tmp.' + Date.now();
    fs.writeFileSync(tmp, data, 'utf-8');
    try { fs.unlinkSync(filePath); } catch {}
    fs.renameSync(tmp, filePath);
  }
}

function safeCopyFileSync(src, dst) {
  try {
    fs.copyFileSync(src, dst);
  } catch (err) {
    try {
      const tmpDst = dst + '.tmp.' + Date.now();
      fs.copyFileSync(src, tmpDst);
      try { fs.unlinkSync(dst); } catch {}
      fs.renameSync(tmpDst, dst);
    } catch (e) {
      console.warn(`[!] Non-fatal copy notice for ${path.basename(dst)}:`, e.message);
    }
  }
}

function verifyEmbeddedDllHash(dllPath, cliPath) {
  if (!fs.existsSync(dllPath) || !fs.existsSync(cliPath)) {
    return { checked: false };
  }
  const dllHashUpper = crypto.createHash('sha256').update(fs.readFileSync(dllPath)).digest('hex').toUpperCase();
  const cliBuf = fs.readFileSync(cliPath);
  const hashUtf16Le = Buffer.from(dllHashUpper, 'utf16le');
  const matched = cliBuf.indexOf(hashUtf16Le) !== -1;
  return { checked: true, matched, dllHashUpper };
}

// 1.5 Synchronize native build binaries if present
const binDir = path.join(rootDir, 'build', 'bin');
const updatesDir = path.join(rootDir, 'updates');
const docsUpdatesDir = path.join(docsDir, 'landing-page', 'public', 'updates');
const appDataUpdatesDir = process.env.APPDATA ? path.join(process.env.APPDATA, 'Stereix Engine', 'updates') : null;

if (fs.existsSync(binDir)) {
  for (const binName of ['vrinject.dll', 'vr-inject-cli.exe']) {
    const srcBin = path.join(binDir, binName);
    const dstBin = path.join(updatesDir, binName);
    if (fs.existsSync(srcBin)) {
      safeCopyFileSync(srcBin, dstBin);
      console.log(`[+] Synced binary ${binName} -> updates/${binName}`);
      if (fs.existsSync(path.dirname(docsUpdatesDir))) {
        if (!fs.existsSync(docsUpdatesDir)) fs.mkdirSync(docsUpdatesDir, { recursive: true });
        safeCopyFileSync(srcBin, path.join(docsUpdatesDir, binName));
        console.log(`[+] Synced binary ${binName} -> docs/updates/${binName}`);
      }
      if (appDataUpdatesDir) {
        if (!fs.existsSync(appDataUpdatesDir)) fs.mkdirSync(appDataUpdatesDir, { recursive: true });
        safeCopyFileSync(srcBin, path.join(appDataUpdatesDir, binName));
        console.log(`[+] Synced binary ${binName} -> AppData/Stereix Engine/updates/${binName}`);
      }
    }
  }
}

// 2. Target Directories for Shaders
const srcShadersDir = path.join(clientDir, 'shaders');
const shaderDestinations = [
  path.join(rootDir, 'updates', 'shaders'),
  path.join(rootDir, 'build', 'bin', 'shaders'),
  path.join(rootDir, 'build', 'bin', 'vrinject_shaders'),
  path.join(docsDir, 'landing-page', 'public', 'updates', 'shaders'),
  ...(appDataUpdatesDir ? [path.join(appDataUpdatesDir, 'shaders')] : []),
];

if (fs.existsSync(srcShadersDir)) {
  const shaderFiles = fs.readdirSync(srcShadersDir).filter(f => f.endsWith('.hlsl'));
  for (const destDir of shaderDestinations) {
    if (fs.existsSync(path.dirname(destDir))) {
      if (!fs.existsSync(destDir)) fs.mkdirSync(destDir, { recursive: true });
      for (const sf of shaderFiles) {
        safeCopyFileSync(path.join(srcShadersDir, sf), path.join(destDir, sf));
      }
      console.log(`[+] Synced ${shaderFiles.length} shaders -> ${path.relative(rootDir, destDir)}`);
    }
  }
}

// 3. Target Directories for Profiles
const srcProfilesDir = path.join(clientDir, 'profiles');
const appDataProfiles = appDataUpdatesDir ? path.join(appDataUpdatesDir, 'profiles') : null;
const profileDestinations = [
  path.join(rootDir, 'updates', 'profiles'),
  path.join(docsDir, 'landing-page', 'public', 'updates', 'profiles'),
  ...(appDataProfiles ? [appDataProfiles] : []),
];

if (fs.existsSync(srcProfilesDir)) {
  const profileFiles = fs.readdirSync(srcProfilesDir).filter(f => f.endsWith('.json'));
  for (const destDir of profileDestinations) {
    if (fs.existsSync(path.dirname(destDir))) {
      if (!fs.existsSync(destDir)) fs.mkdirSync(destDir, { recursive: true });
      for (const pf of profileFiles) {
        safeCopyFileSync(path.join(srcProfilesDir, pf), path.join(destDir, pf));
      }
      console.log(`[+] Synced ${profileFiles.length} profiles -> ${path.relative(rootDir, destDir)}`);
    }
  }
}

// 3.5 CRIT-03 Verification: vrinject.dll MUST already be signed pre-hash by CMake POST_BUILD.
// Only sign unsigned vr-inject-cli.exe copies if needed, NEVER mutate vrinject.dll post-link.
const signScript = path.join(rootDir, 'scripts', 'sign_binaries.ps1');
if (fs.existsSync(signScript)) {
  try {
    const { execSync } = require('child_process');
    console.log('[*] Checking CLI Authenticode signatures (preserving vrinject.dll pre-hash bytes)...');
    execSync(`powershell.exe -ExecutionPolicy Bypass -File "${signScript}" -SkipDllHashMutation`, { stdio: 'inherit' });
  } catch (err) {
    console.warn('[!] Binary signing warning:', err.message);
  }
}

const updatesDll = path.join(updatesDir, 'vrinject.dll');
const updatesCli = path.join(updatesDir, 'vr-inject-cli.exe');
const hashCheck = verifyEmbeddedDllHash(updatesDll, updatesCli);
if (hashCheck.checked) {
  if (!hashCheck.matched) {
    console.error(`[FATAL CRIT-03] updates/vrinject.dll SHA-256 (${hashCheck.dllHashUpper}) does NOT match EXPECTED_DLL_HASH embedded in updates/vr-inject-cli.exe!`);
    console.error('Rebuild with build.bat so vrinject.dll is signed BEFORE expected_hash.h is generated, then re-run npm run sync:assets.');
    process.exit(1);
  }
  console.log(`[OK] Verified updates/vrinject.dll SHA-256 (${hashCheck.dllHashUpper.slice(0, 16)}...) matches EXPECTED_DLL_HASH in updates/vr-inject-cli.exe!`);
}

// 4. Recompute SHA-256 hashes and update manifest.json
if (fs.existsSync(manifestFile)) {
  try {
    const manifest = JSON.parse(fs.readFileSync(manifestFile, 'utf-8'));
    manifest.engineVersion = currentVersion;
    manifest.timestamp = Date.now();
    manifest.buildDate = new Date().toISOString().split('T')[0];

    const hashes = {};

    if (Array.isArray(manifest.files)) {
      for (const relFile of manifest.files) {
        const fullPath = path.join(updatesDir, relFile);
        if (fs.existsSync(fullPath) && fs.statSync(fullPath).isFile()) {
          const buf = fs.readFileSync(fullPath);
          hashes[relFile] = crypto.createHash('sha256').update(buf).digest('hex');
        } else if (manifest.hashes && manifest.hashes[relFile]) {
          hashes[relFile] = manifest.hashes[relFile];
        }
      }
      manifest.hashes = hashes;
    }

    // 4.5 Canonicalize and digitally sign manifest with Ed25519 publisher key
    function getCanonicalManifestData(m) {
      const files = Array.isArray(m.files) ? [...m.files].sort() : [];
      const h = {};
      if (m.hashes) {
        for (const key of Object.keys(m.hashes).sort()) {
          h[key] = m.hashes[key];
        }
      }
      return Buffer.from(JSON.stringify({
        engineVersion: m.engineVersion,
        timestamp: m.timestamp,
        files,
        hashes: h,
      }));
    }

    const keyFile = path.join(rootDir, 'scripts', 'ota_signing_key.pem');
    const privateKeyPem = process.env.OTA_SIGNING_KEY || (fs.existsSync(keyFile) ? fs.readFileSync(keyFile, 'utf-8') : null);
    if (privateKeyPem) {
      const canonical = getCanonicalManifestData(manifest);
      manifest.signature = crypto.sign(null, canonical, privateKeyPem).toString('hex');
      console.log(`[+] Digitally signed OTA manifest with Ed25519 key (signature: ${manifest.signature.slice(0, 16)}...)`);
    } else {
      console.warn('[!] Warning: OTA signing key not found. Manifest will not contain a cryptographic publisher signature.');
    }

    const jsonStr = JSON.stringify(manifest, null, 2);
    safeWriteFileSync(manifestFile, jsonStr);
    console.log(`[+] Recomputed hashes and updated: ${path.relative(rootDir, manifestFile)}`);

    if (fs.existsSync(path.dirname(docsManifestFile))) {
      safeWriteFileSync(docsManifestFile, jsonStr);
      console.log(`[+] Synced manifest -> ${path.relative(rootDir, docsManifestFile)}`);
    }
    if (appDataUpdatesDir) {
      if (!fs.existsSync(appDataUpdatesDir)) fs.mkdirSync(appDataUpdatesDir, { recursive: true });
      safeWriteFileSync(path.join(appDataUpdatesDir, 'manifest.json'), jsonStr);
      console.log(`[+] Synced manifest -> AppData/Stereix Engine/updates/manifest.json`);
    }
  } catch (e) {
    console.error('Error updating manifest:', e);
    process.exit(1);
  }
}

console.log('=== Asset Synchronization Complete! ===');
