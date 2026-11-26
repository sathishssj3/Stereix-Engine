import { app, shell, ipcMain } from 'electron';
import * as path from 'path';
import * as fs from 'fs';
import * as crypto from 'crypto';
import * as child_process from 'child_process';
import * as util from 'util';
import { InjectResult } from '../src/types';
import {
  assertTrustedIpcSender,
  canonicalExistingPath,
  gamePathsMap,
  gameExeMap,
  resolveWithinRoot,
  safeGamePath,
  validateGameId,
} from './utils';
import { detectAntiCheat } from './libraryManager';
import { getLocalManifest, compareSemver } from './updateManager';
import { sendDiscordTelemetry } from './telemetryManager';

const execFileAsync = util.promisify(child_process.execFile);
const isDev = !app.isPackaged;

export let cancelInjectionFlag = false;
export let activeTargetExeName = '';
export let activeTargetPid = 0;
export let activeGameId = '';
export let activeSessionStartTime = 0;
export let activeSessionConfig: any = null;

const injectRateLimits: Record<string, number[]> = Object.create(null);
let injectionInProgress = false;
let activeLogPath = '';

function verifyOtaAssetHash(otaPath: string, local: ReturnType<typeof getLocalManifest>): boolean {
  if (!local || !local.hashes) {
    console.warn(`[SECURITY] No cryptographically verified manifest found for ${otaPath}! Discarding OTA cache.`);
    return false;
  }
  const basename = path.basename(otaPath);
  const expectedHash = local.hashes[basename];
  if (!expectedHash) {
    console.warn(`[SECURITY] OTA file ${basename} not found in verified manifest hashes! Discarding.`);
    return false;
  }
  const fileBuf = fs.readFileSync(otaPath);
  const actualHash = crypto.createHash('sha256').update(fileBuf).digest('hex').toLowerCase();
  if (actualHash !== expectedHash.toLowerCase()) {
    console.warn(`[SECURITY] OTA file ${basename} SHA-256 mismatch at deploy time! Discarding OTA and falling back to bundled binary.`);
    try { fs.unlinkSync(otaPath); } catch {}
    return false;
  }
  return true;
}

export function pickPreferredAsset(canonicalPath: string, otaPath: string, minSize = 0): string {
  const otaExists = fs.existsSync(otaPath) && (minSize === 0 || fs.statSync(otaPath).size >= minSize);
  const canonExists = fs.existsSync(canonicalPath) && (minSize === 0 || fs.statSync(canonicalPath).size >= minSize);

  if (!otaExists) return canonicalPath;

  // In development mode (npm run dev / unpacked), local build artifacts must ALWAYS take precedence over downloaded OTA cache!
  if (!app.isPackaged) {
    return canonicalPath;
  }

  // Packaged mode: OTA assets are ONLY eligible when backed by a cryptographically verified manifest
  // strictly newer than the installed app AND matching the manifest's SHA-256 hash (HIGH-10).
  try {
    const local = getLocalManifest();
    const appVer = app.getVersion();
    if (!local || !local.hashes) {
      return canonicalPath;
    }
    if (canonExists && appVer && compareSemver(appVer, local.engineVersion) >= 0) {
      return canonicalPath;
    }
    if (!canonExists) {
      return verifyOtaAssetHash(otaPath, local) ? otaPath : canonicalPath;
    }
    if (verifyOtaAssetHash(otaPath, local)) {
      return otaPath;
    }
    return canonicalPath;
  } catch {
    return canonicalPath;
  }
}

async function terminatePid(pid: number, force = false): Promise<void> {
  if (!Number.isSafeInteger(pid) || pid <= 0) return;
  const args = ['/PID', String(pid)];
  if (force) args.push('/F');
  await execFileAsync('taskkill.exe', args, {
    timeout: 3000,
    windowsHide: true,
  }).catch(() => {});
}

interface RunningGameCandidate {
  pid: number;
  memory: number;
  path: string;
  hasWindow: boolean;
}

async function inspectProcess(pid: number): Promise<{ path: string; hasWindow: boolean }> {
  if (!Number.isSafeInteger(pid) || pid <= 0) return { path: '', hasWindow: false };
  try {
    const command =
      `$p = Get-Process -Id ${pid} -ErrorAction SilentlyContinue; ` +
      `$ep = ''; try { $ep = $p.Path } catch {}; ` +
      `if (-not $ep) { $ep = (Get-CimInstance Win32_Process -Filter "ProcessId = ${pid}" -ErrorAction SilentlyContinue).ExecutablePath }; ` +
      `$hw = if ($p -and $p.MainWindowHandle -ne [IntPtr]::Zero) { '1' } else { '0' }; ` +
      `Write-Output "$hw|$ep"`;
    const { stdout } = await execFileAsync(
      'powershell.exe',
      ['-NoProfile', '-NonInteractive', '-Command', command],
      { encoding: 'utf-8', timeout: 3000, windowsHide: true }
    );
    const trimmed = stdout.trim();
    const sepIdx = trimmed.indexOf('|');
    if (sepIdx === -1) return { path: trimmed, hasWindow: false };
    return {
      hasWindow: trimmed.slice(0, sepIdx) === '1',
      path: trimmed.slice(sepIdx + 1).trim(),
    };
  } catch {
    return { path: '', hasWindow: false };
  }
}

async function findRunningGameCandidate(
  targetExeName: string,
  installPath: string,
  targetExeDir: string,
  allowLowMemoryFallback = false
): Promise<RunningGameCandidate | null> {
  try {
    const { stdout } = await execFileAsync(
      'tasklist.exe',
      ['/fi', `IMAGENAME eq ${targetExeName}`, '/fo', 'csv', '/nh'],
      { encoding: 'utf-8', timeout: 2000, killSignal: 'SIGKILL', windowsHide: true }
    );

    const candidates: Array<{ pid: number; memory: number }> = [];
    for (const line of stdout.split('\n')) {
      if (!line.trim() || line.startsWith('INFO:')) continue;
      const parts = line.split('","');
      if (parts.length < 5) continue;
      const exeName = parts[0].replace(/"/g, '').toLowerCase();
      const pid = Number.parseInt(parts[1].replace(/"/g, ''), 10);
      const memory = Number.parseInt(
        parts[4].replace(/"/g, '').replace(/,/g, '').replace(/\s*K\s*$/i, ''),
        10
      );
      // Filter out processes with invalid PID or negligible memory (e.g. zombie processes < 5MB)
      if (exeName === targetExeName && Number.isSafeInteger(pid) && memory > 5000) {
        candidates.push({ pid, memory: Number.isFinite(memory) ? memory : 0 });
      }
    }

    if (candidates.length === 0) return null;

    // Sort highest-memory candidates first so shipping game binaries win over tiny stubs
    candidates.sort((a, b) => b.memory - a.memory);

    const resolvedInstall = path.resolve(installPath).toLowerCase();
    const installPrefix = resolvedInstall.endsWith(path.sep) ? resolvedInstall : resolvedInstall + path.sep;
    let selectedCandidate: RunningGameCandidate | null = null;
    const uninspectableCandidates: Array<{ pid: number; memory: number }> = [];

    for (const candidate of candidates) {
      const info = await inspectProcess(candidate.pid).catch(() => ({ path: '', hasWindow: false }));
      if (!info.path) {
        uninspectableCandidates.push(candidate);
        continue;
      }

      const resolvedCandPath = path.resolve(info.path).toLowerCase();
      const isInsideInstall = resolvedCandPath.startsWith(installPrefix);
      if (!isInsideInstall) continue;

      const lowerPath = info.path.toLowerCase();
      const isShippingBinary = lowerPath.includes('binaries') || lowerPath.includes('shipping');

      if (isShippingBinary || candidate.memory > 50000) {
        selectedCandidate = { ...candidate, path: info.path, hasWindow: info.hasWindow };
        break;
      }

      if (!selectedCandidate) {
        selectedCandidate = { ...candidate, path: info.path, hasWindow: info.hasWindow };
      }
    }

    // Resilient Fallback: ONLY for candidates whose executable path lookup was blocked by
    // Windows token/elevation permissions (empty info.path). NEVER fall back to a candidate
    // whose path was inspected and proven to live outside installPath (CRIT-02).
    if (!selectedCandidate && uninspectableCandidates.length > 0) {
      const best = uninspectableCandidates[0];
      if (best.memory > 30000 || allowLowMemoryFallback) {
        selectedCandidate = {
          pid: best.pid,
          memory: best.memory,
          path: path.join(targetExeDir, targetExeName),
          hasWindow: false,
        };
      }
    }

    return selectedCandidate;
  } catch {
    return null;
  }
}

function stripAnsiAndBom(text: string): string {
  return text
    .replace(/^\uFEFF/, '')
    .replace(/\x1b\[[0-9;]*m/g, '')
    .trim();
}

function extractCliFailureReason(logFilePath: string, error: any): { message: string; isUacCancelled: boolean } {
  const rawErr = String(error?.stderr || error?.message || '');
  if (/canceled by the user|cancelled by the user|1223/i.test(rawErr)) {
    return {
      message: 'UAC elevation prompt was cancelled by the user.',
      isUacCancelled: true,
    };
  }

  try {
    if (fs.existsSync(logFilePath)) {
      const raw = fs.readFileSync(logFilePath, 'utf-8');
      const lines = raw
        .split(/\r?\n/)
        .map(stripAnsiAndBom)
        .filter((l) => l.length > 0);
      for (let i = lines.length - 1; i >= 0; i--) {
        const line = lines[i];
        if (/\[(\s*FAIL\s*|ERROR|SECURITY[^\]]*)\]/i.test(line)) {
          return { message: line, isUacCancelled: false };
        }
      }
      if (lines.length > 0) {
        return { message: lines[lines.length - 1], isUacCancelled: false };
      }
    }
  } catch {}

  const codeMatch = rawErr.match(/exit code\s+(\d+)/i) || (error?.code ? [String(error.code), String(error.code)] : null);
  const codeSuffix = codeMatch ? ` (Exit code ${codeMatch[1]})` : '';
  return {
    message: `vr-inject-cli failed${codeSuffix}. Check session log for details.`,
    isUacCancelled: false,
  };
}

let activeWatchedLogPaths: string[] = [];
let activeWatchers: fs.FSWatcher[] = [];
let watchedSizes: Record<string, number> = {};
let watchedRemainders: Record<string, string> = {};
export let currentSessionLogPath = '';
export let latestSessionLogPath = '';

export function getLogsDir(): string {
  const logsDir = path.join(app.getPath('userData'), 'logs');
  if (!fs.existsSync(logsDir)) {
    fs.mkdirSync(logsDir, { recursive: true });
  }
  return logsDir;
}

export function getLatestSessionLogPath(): string {
  return latestSessionLogPath || path.join(getLogsDir(), 'latest_session.log');
}

export function emitLogLine(event: Electron.IpcMainInvokeEvent, line: string): void {
  const trimmed = line.trim();
  if (!trimmed) return;

  try {
    if (!event.sender.isDestroyed()) {
      event.sender.send('log:line', trimmed.slice(0, 2000));
    }
  } catch {}

  const chunk = trimmed + '\n';
  try {
    if (currentSessionLogPath) {
      fs.appendFile(currentSessionLogPath, chunk, 'utf-8', () => {});
    }
    if (latestSessionLogPath) {
      fs.appendFile(latestSessionLogPath, chunk, 'utf-8', () => {});
    }
  } catch {}
}

let isReadingLogs = false;
function readNewBytes(normalized: string, event: Electron.IpcMainInvokeEvent): void {
  if (!fs.existsSync(normalized) || isReadingLogs) return;
  isReadingLogs = true;

  try {
    const fd = fs.openSync(normalized, 'r');
    try {
      const stat = fs.fstatSync(fd);
      let lastSize = watchedSizes[normalized] ?? 0;
      if (stat.size < lastSize) {
        lastSize = 0;
        watchedSizes[normalized] = 0;
        watchedRemainders[normalized] = '';
      }
      if (stat.size <= lastSize) return;

      const length = Math.min(stat.size - lastSize, 2 * 1024 * 1024);
      const buf = Buffer.alloc(length);
      const bytesRead = fs.readSync(fd, buf, 0, length, lastSize);
      watchedSizes[normalized] = lastSize + bytesRead;

      const text = (watchedRemainders[normalized] || '') + buf.subarray(0, bytesRead).toString('utf-8');
      const lines = text.split(/\r?\n/);
      watchedRemainders[normalized] = lines.pop() || '';

      const validLines: string[] = [];
      for (const line of lines) {
        const trimmed = line.trim();
        if (trimmed) validLines.push(trimmed);
      }

      if (validLines.length > 0) {
        // Non-blocking asynchronous batch append to session logs
        const chunk = validLines.join('\n') + '\n';
        if (currentSessionLogPath) {
          fs.appendFile(currentSessionLogPath, chunk, 'utf-8', () => {});
        }
        if (latestSessionLogPath) {
          fs.appendFile(latestSessionLogPath, chunk, 'utf-8', () => {});
        }

        // Bounded IPC dispatch to prevent event loop starvation
        if (!event.sender.isDestroyed()) {
          const toSend = validLines.length > 60 ? validLines.slice(validLines.length - 60) : validLines;
          for (const l of toSend) {
            event.sender.send('log:line', l.slice(0, 2000));
          }
        }
      }
    } finally {
      fs.closeSync(fd);
    }
  } catch {} finally {
    isReadingLogs = false;
  }
}

function flushWatchedLogs(event: Electron.IpcMainInvokeEvent): void {
  for (const p of [...activeWatchedLogPaths]) {
    try {
      readNewBytes(p, event);
    } catch {}
  }
}

function stopActiveLogWatch(): void {
  for (const p of activeWatchedLogPaths) {
    try {
      fs.unwatchFile(p);
    } catch {}
  }
  for (const w of activeWatchers) {
    try {
      w.close();
    } catch {}
  }
  activeWatchers = [];
  activeWatchedLogPaths = [];
  watchedSizes = {};
  watchedRemainders = {};
  activeLogPath = '';
  isReadingLogs = false;
}

function startWatchingLogFile(filePath: string, event: Electron.IpcMainInvokeEvent): void {
  const normalized = path.resolve(filePath);
  if (activeWatchedLogPaths.includes(normalized)) return;
  activeWatchedLogPaths.push(normalized);

  let initialSize = 0;
  try {
    if (fs.existsSync(normalized)) {
      initialSize = fs.statSync(normalized).size;
    }
  } catch {}
  watchedSizes[normalized] = initialSize;
  watchedRemainders[normalized] = '';

  // Clean, focused 150ms handle polling without directory storm noise
  fs.watchFile(normalized, { interval: 150 }, () => {
    readNewBytes(normalized, event);
  });

  // Flush any content already written
  readNewBytes(normalized, event);
}

ipcMain.handle('inject:cancel', async (event) => {
  assertTrustedIpcSender(event);
  cancelInjectionFlag = true;

  const pid = activeTargetPid;
  if (pid > 0) {
    await terminatePid(pid);
    setTimeout(() => void terminatePid(pid, true), 2000);
  }

  sendDiscordTelemetry({
    gameId: activeGameId || 'cancelled_session',
    gameName: activeTargetExeName,
    status: 'error',
    message: 'User cancelled injection or session was terminated.',
    logFilePath: currentSessionLogPath || latestSessionLogPath,
  }).catch(() => {});
});

ipcMain.handle('inject:deploy', async (event, id: string): Promise<InjectResult> => {
  try {
    assertTrustedIpcSender(event);
    const validId = validateGameId(id);

    if (injectionInProgress) {
      return { success: false, message: 'Another injection is already in progress.' };
    }
    injectionInProgress = true;

    const now = Date.now();
    const windowMs = 15 * 60 * 1000;
    injectRateLimits[validId] = (injectRateLimits[validId] || []).filter(t => now - t < windowMs);
    if (injectRateLimits[validId].length >= 5) {
      return { success: false, message: 'Rate limit exceeded.' };
    }
    injectRateLimits[validId].push(now);

    const registeredInstallPath = gamePathsMap[validId];
    if (!registeredInstallPath) return { success: false, message: 'Game path not found' };
    const installPath = canonicalExistingPath(registeredInstallPath, 'directory');

    // Anti-Cheat Guard: Block injection into protected games to prevent multiplayer account bans
    const ac = detectAntiCheat(installPath);
    if (ac.hasAntiCheat) {
      return {
        success: false,
        message: `Injection Blocked: ${ac.antiCheatName} detected. To protect your account from multiplayer bans, VR injection is disabled on this title.`
      };
    }

    const logPath = safeGamePath(installPath, 'vrinject.log');
    activeLogPath = logPath;

    stopActiveLogWatch();

    const logsDir = getLogsDir();
    const timestamp = new Date().toISOString().replace(/[:.]/g, '-');
    currentSessionLogPath = path.join(logsDir, `session_${validId}_${timestamp}.log`);
    latestSessionLogPath = path.join(logsDir, 'latest_session.log');

    try {
      const appVer = app.getVersion() || '0.1.60';
      const header = `=== Stereix Engine Session Log [v${appVer}] ===\nGame ID: ${validId}\nStarted: ${new Date().toISOString()}\n==========================================\n`;
      fs.writeFileSync(currentSessionLogPath, header, 'utf-8');
      fs.writeFileSync(latestSessionLogPath, header, 'utf-8');
    } catch {}

    try {
      fs.writeFileSync(logPath, '');
    } catch {}

    emitLogLine(event, `[--] Initializing session diagnostics for game ID: ${validId}`);
    emitLogLine(event, `[--] Session log file: ${path.basename(currentSessionLogPath)}`);

    startWatchingLogFile(logPath, event);

    const localAppData = process.env.LOCALAPPDATA || '';
    if (localAppData) {
      const appDataLog = path.join(localAppData, 'VRInject', 'vrinject.log');
      try {
        const vrDir = path.dirname(appDataLog);
        if (!fs.existsSync(vrDir)) fs.mkdirSync(vrDir, { recursive: true });
        if (!fs.existsSync(appDataLog)) fs.writeFileSync(appDataLog, '', 'utf-8');
      } catch {}
      startWatchingLogFile(appDataLog, event);
    }

    const candidateBinDirs = [
      path.resolve(__dirname, '../../../../build/bin'),
      path.resolve(__dirname, '../../../build/bin'),
      path.resolve(__dirname, '../../build/bin'),
      path.resolve(app.getAppPath(), '../../build/bin'),
      path.resolve(app.getAppPath(), '../../../build/bin'),
      path.resolve(process.cwd(), '../build/bin'),
      path.resolve(process.cwd(), '../../build/bin'),
    ];
    const devBinDir = candidateBinDirs.find(
      (d) => fs.existsSync(d) && (fs.existsSync(path.join(d, 'vrinject.dll')) || fs.existsSync(path.join(d, 'vr-inject-cli.exe')))
    );
    const binSourceDir = (isDev || (!fs.existsSync(path.join(process.resourcesPath, 'vrinject.dll')) && devBinDir))
      ? (devBinDir || process.resourcesPath)
      : process.resourcesPath;
    const canonicalBinSourceDir = canonicalExistingPath(binSourceDir, 'directory');
    const otaCli = path.join(app.getPath('userData'), 'updates', 'vr-inject-cli.exe');
    const bundledCli = resolveWithinRoot(canonicalBinSourceDir, 'vr-inject-cli.exe');
    const canonCli = fs.existsSync(bundledCli)
      ? canonicalExistingPath(bundledCli, 'file')
      : bundledCli;
    const cliSource = pickPreferredAsset(canonCli, otaCli, 10000);
    const candidateShaderDirs = [
      resolveWithinRoot(canonicalBinSourceDir, 'shaders'),
      path.resolve(__dirname, '../../../../build/bin/shaders'),
      path.resolve(__dirname, '../../../build/bin/shaders'),
      path.resolve(__dirname, '../../shaders'),
      path.resolve(process.resourcesPath, 'shaders'),
    ];
    const shadersSource = candidateShaderDirs.find((d) => fs.existsSync(d)) || resolveWithinRoot(canonicalBinSourceDir, 'shaders');
    const candidateModelDirs = [
      resolveWithinRoot(canonicalBinSourceDir, 'models'),
      path.resolve(__dirname, '../../../../stereix-client/models'),
      path.resolve(__dirname, '../../../../nexvr-client/models'),
      path.resolve(__dirname, '../../../../models'),
      path.resolve(__dirname, '../../../models'),
      path.resolve(__dirname, '../../models'),
      path.resolve(process.resourcesPath, 'models'),
    ];
    const modelsSource = candidateModelDirs.find((d) => fs.existsSync(d)) || resolveWithinRoot(canonicalBinSourceDir, 'models');
    const updatesDir = path.join(app.getPath('userData'), 'updates');
    const hotfixShaders = path.join(updatesDir, 'shaders');

    // Step 1: Resolve target executable BEFORE launching so we can detect already-running games
    // and deploy shaders, models, DLLs, and vrinject.json without racing game startup.
    let targetExeName = '';
    let targetExeDir = installPath;
    const registeredExe = gameExeMap[validId];
    if (registeredExe) {
      const customExe = canonicalExistingPath(registeredExe, 'file');
      resolveWithinRoot(installPath, path.relative(installPath, customExe));
      targetExeName = path.basename(customExe).toLowerCase();
      targetExeDir = path.dirname(customExe);
    } else {
      let largestSize = 0;
      let visitedEntries = 0;

      const scan = (dir: string, depth = 0) => {
        if (depth > 8 || visitedEntries >= 20000) return;
        try {
          const entries = fs.readdirSync(dir, { withFileTypes: true });
          for (const entry of entries) {
            if (++visitedEntries >= 20000) break;
            if (entry.isSymbolicLink()) continue;

            const fullPath = resolveWithinRoot(
              installPath,
              path.relative(installPath, path.join(dir, entry.name))
            );
            try {
              if (entry.isDirectory()) {
                scan(fullPath, depth + 1);
              } else if (entry.isFile() && entry.name.toLowerCase().endsWith('.exe')) {
                const lower = entry.name.toLowerCase();
                if (
                  lower.includes('launcher') ||
                  lower.includes('crashreporter') ||
                  lower.includes('crashhandler') ||
                  lower.includes('reporter') ||
                  lower.includes('anticheat') ||
                  lower.includes('eosbootstrapper') ||
                  lower.includes('start_protected_game')
                ) {
                  continue;
                }
                const size = fs.statSync(fullPath).size;
                if (size > largestSize) {
                  largestSize = size;
                  targetExeName = lower;
                  targetExeDir = dir;
                }
              }
            } catch {
              // Ignore inaccessible game subdirectories.
            }
          }
        } catch {
          // Ignore inaccessible game subdirectories.
        }
      };
      scan(installPath);
    }

    if (!targetExeName) {
      stopActiveLogWatch();
      return { success: false, message: 'Could not determine main executable name' };
    }

    // Step 2: Check whether the game process is already running before deploying/launching.
    const alreadyRunning = await findRunningGameCandidate(targetExeName, installPath, targetExeDir, false);
    if (alreadyRunning) {
      targetExeDir = path.dirname(alreadyRunning.path);
    }

    try {
      const verifiedLocalManifest = getLocalManifest();
      const currentAppVer = app.getVersion();
      const isOtaNewerAndVerified = Boolean(
        app.isPackaged &&
        verifiedLocalManifest &&
        verifiedLocalManifest.hashes &&
        currentAppVer &&
        compareSemver(currentAppVer, verifiedLocalManifest.engineVersion) < 0
      );

      // Fail-closed OTA shaders (HIGH-10): ONLY use hotfixShaders when manifest signature is valid,
      // OTA is strictly newer than bundled app, AND EVERY file recursively inside hotfixShaders passes SHA-256 verification.
      let activeShadersSource = shadersSource;
      if (isOtaNewerAndVerified && fs.existsSync(hotfixShaders)) {
        try {
          const collectFiles = (dir: string, relBase = ''): string[] => {
            const out: string[] = [];
            for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
              if (entry.isSymbolicLink()) return ['__SYMLINK_REJECTED__'];
              const rel = relBase ? `${relBase}/${entry.name}` : entry.name;
              const full = path.join(dir, entry.name);
              if (entry.isDirectory()) {
                out.push(...collectFiles(full, rel));
              } else if (entry.isFile()) {
                out.push(rel);
              }
            }
            return out;
          };
          const shaderFiles = collectFiles(hotfixShaders);
          const allVerified = shaderFiles.length > 0 && shaderFiles.every((relFile) => {
            if (relFile === '__SYMLINK_REJECTED__') return false;
            const expected = verifiedLocalManifest?.hashes?.[`shaders/${relFile}`];
            if (!expected) return false;
            const actual = crypto.createHash('sha256').update(fs.readFileSync(path.join(hotfixShaders, relFile))).digest('hex').toLowerCase();
            return actual === expected.toLowerCase();
          });
          if (allVerified) {
            activeShadersSource = hotfixShaders;
          } else {
            console.warn('[SECURITY] OTA shader SHA-256 verification failed or incomplete; failing closed to bundled shaders.');
          }
        } catch {
          activeShadersSource = shadersSource;
        }
      }

      const targetDirs = new Set<string>([targetExeDir, installPath]);
      for (const sub of ['Phoenix/Binaries/Win64', 'Chameleon/Binaries/Win64', 'Dungeonhaven/Binaries/Win64', 'Binaries/Win64']) {
        const subDir = path.join(installPath, sub);
        if (fs.existsSync(subDir)) targetDirs.add(subDir);
      }

      const loadOwnership = (dir: string): { files: Set<string>; dirs: Set<string> } => {
        const manifestPath = path.join(dir, '.stereix_deployed.json');
        try {
          if (fs.existsSync(manifestPath)) {
            const parsed = JSON.parse(fs.readFileSync(manifestPath, 'utf-8'));
            return {
              files: new Set<string>(Array.isArray(parsed.files) ? parsed.files : []),
              dirs: new Set<string>(Array.isArray(parsed.dirs) ? parsed.dirs : []),
            };
          }
        } catch {}
        return { files: new Set<string>(), dirs: new Set<string>() };
      };
      const saveOwnership = (dir: string, owned: { files: Set<string>; dirs: Set<string> }) => {
        try {
          const manifestPath = path.join(dir, '.stereix_deployed.json');
          fs.writeFileSync(
            manifestPath,
            JSON.stringify({ version: 1, files: Array.from(owned.files), dirs: Array.from(owned.dirs) }, null, 2),
            'utf-8'
          );
        } catch {}
      };
      const ownershipByDir = new Map<string, { files: Set<string>; dirs: Set<string> }>();
      for (const d of targetDirs) {
        ownershipByDir.set(d, loadOwnership(d));
      }

      for (const d of targetDirs) {
        const owned = ownershipByDir.get(d)!;
        if (fs.existsSync(activeShadersSource)) {
          try {
            const dstShaders = path.join(d, 'shaders');
            const existedBefore = fs.existsSync(dstShaders);
            if (!existedBefore) owned.dirs.add('shaders');
            fs.cpSync(activeShadersSource, dstShaders, {
              recursive: true,
              force: true,
              dereference: false,
            });
          } catch (e) {}
        }
        if (fs.existsSync(modelsSource)) {
          try {
            const dstModels = path.join(d, 'models');
            if (!fs.existsSync(dstModels)) owned.dirs.add('models');
            fs.cpSync(modelsSource, dstModels, {
              recursive: true,
              force: true,
              dereference: false,
            });
          } catch (e) {}
        }
      }

      // Copy ONNX and DirectML DLLs to prevent target process loader lock/freeze due to missing imports
      // Also copy vrinject.dll and openxr_loader.dll so the implicit Vulkan layer can pick it up BEFORE the game starts
      // Prioritize local builds in dev mode, or newer verified OTA hotfixed binaries if available in packaged mode!
      // MED-04: Track Stereix-owned files so uninstall never deletes a game's pre-existing DirectML/onnxruntime DLLs.
      const dllsToCopy = ['onnxruntime.dll', 'DirectML.dll', 'vrinject.dll', 'openxr_loader.dll'];
      for (const dll of dllsToCopy) {
        const hotfixPath = path.join(updatesDir, dll);
        const canonPath = resolveWithinRoot(canonicalBinSourceDir, dll);
        const srcPath = pickPreferredAsset(canonPath, hotfixPath);

        if (fs.existsSync(srcPath)) {
          for (const d of targetDirs) {
            const owned = ownershipByDir.get(d)!;
            const dstFile = path.join(d, dll);
            const isSharedRuntime = dll === 'onnxruntime.dll' || dll === 'DirectML.dll' || dll === 'openxr_loader.dll';
            try {
              const existedBefore = fs.existsSync(dstFile);
              if (existedBefore && isSharedRuntime && !owned.files.has(dll)) {
                // Preserve pre-existing game-shipped runtime DLL (do not overwrite or claim ownership)
                continue;
              }
              fs.copyFileSync(srcPath, dstFile);
              owned.files.add(dll);
              console.info(`Copied ${dll} to ${d} (source: ${srcPath === hotfixPath ? 'HOTFIX' : 'LOCAL/BUNDLED'})`);
            } catch (e) {
              console.warn(`Could not copy ${dll} to ${d} (already present or locked): ${e}`);
            }
          }
        }
      }

      // Proxy DLL Bootstrapping: Deploy dxgi.dll from proxy directory next to the game.
      // This enables early, natural DLL loading on process creation, avoiding Win32 Error 5 (CreateRemoteThread blocked).
      const proxySourceDir = path.join(canonicalBinSourceDir, 'proxy');
      const proxyDxgi = path.join(proxySourceDir, 'dxgi.dll');
      if (fs.existsSync(proxyDxgi)) {
        for (const d of targetDirs) {
          const owned = ownershipByDir.get(d)!;
          const dstDxgi = path.join(d, 'dxgi.dll');
          try {
            if (!fs.existsSync(dstDxgi) || owned.files.has('dxgi.dll')) {
              fs.copyFileSync(proxyDxgi, dstDxgi);
              owned.files.add('dxgi.dll');
              console.info(`Deployed dxgi.dll proxy bootstrap to ${d}`);
            }
          } catch (e) {
            console.warn(`Could not deploy dxgi.dll proxy to ${d} (may be locked by running game): ${e}`);
          }
        }
      }

      for (const [d, owned] of ownershipByDir.entries()) {
        saveOwnership(d, owned);
      }

      // Ensure vrinject.json configuration is synchronized across all target binary directories
      const rootConfigPath = path.join(installPath, 'vrinject.json');
      let baseProfile: Record<string, any> = {};

      const localProfileDirs = [
        path.join(process.resourcesPath, 'profiles'),
        path.resolve(__dirname, '../../profiles'),
        path.resolve(__dirname, '../../../profiles'),
        path.resolve(__dirname, '../../../../profiles'),
        path.resolve(__dirname, '../../../../stereix-client/profiles'),
      ];
      const otaProfileDir = path.join(app.getPath('userData'), 'updates', 'profiles');
      // Last-writer-wins order (HIGH-10):
      // - Dev mode (!app.isPackaged): local source profiles come LAST and always beat OTA.
      // - Packaged mode: bundled profiles first, then cryptographically verified newer OTA profiles LAST.
      const profileDirs = (!app.isPackaged)
        ? [...localProfileDirs]
        : (isOtaNewerAndVerified ? [...localProfileDirs, otaProfileDir] : [...localProfileDirs]);
      const exeBase = targetExeName ? path.basename(targetExeName, '.exe').toLowerCase() : '';
      for (const pDir of profileDirs) {
        try {
          if (fs.existsSync(pDir)) {
            const isOtaDir = pDir === otaProfileDir;
            for (const f of fs.readdirSync(pDir)) {
              const lowerF = f.toLowerCase();
              let isMatch = f.startsWith(`${validId}_`) || f === `${validId}.json`;
              if (!isMatch) {
                if (exeBase && (lowerF.includes(exeBase) || lowerF === `${exeBase}.json`)) isMatch = true;
                if (lowerF.includes('sekiro') && (exeBase.includes('sekiro') || validId.includes('sekiro') || installPath.toLowerCase().includes('sekiro'))) isMatch = true;
                if (lowerF.includes('mortal_shell') && (exeBase.includes('dungeonhaven') || exeBase.includes('mortalshell') || validId.includes('mortalshell') || installPath.toLowerCase().includes('mortalshell'))) isMatch = true;
                if (lowerF.includes('hogwarts') && (exeBase.includes('hogwarts') || exeBase.includes('phoenix') || validId.includes('hogwarts') || installPath.toLowerCase().includes('hogwarts'))) isMatch = true;
              }
              if (isMatch) {
                const profileFile = path.join(pDir, f);
                const rawBuf = fs.readFileSync(profileFile);
                if (isOtaDir) {
                  const expectedHash = verifiedLocalManifest?.hashes?.[`profiles/${f}`];
                  if (!expectedHash) continue;
                  const actualHash = crypto.createHash('sha256').update(rawBuf).digest('hex').toLowerCase();
                  if (actualHash !== expectedHash.toLowerCase()) {
                    console.warn(`[SECURITY] OTA profile ${f} SHA-256 mismatch — ignoring.`);
                    continue;
                  }
                }
                const parsed = JSON.parse(rawBuf.toString('utf-8'));
                delete parsed.telemetryOptIn; // F-4: Profile JSONs cannot override user privacy consent
                baseProfile = { ...baseProfile, ...parsed };
                break;
              }
            }
          }
        } catch {}
      }

      // Hardened fallback for Sekiro if profile was not matched from disk
      if (Object.keys(baseProfile).length === 0 && (exeBase.includes('sekiro') || validId.includes('sekiro') || installPath.toLowerCase().includes('sekiro'))) {
        baseProfile = {
          id: '814380',
          name: 'Sekiro: Shadows Die Twice',
          engine: 'Generic',
          api: 'DX11',
          reverseZ: false,
          rowMajorMatrices: false,
          matrixPrecision: 'Float32',
          motionAimSensitivity: 1.0,
          useRecommendedResolution: true,
          srgbCorrection: true,
          depthSubmission: false,
          rawInputMode: true,
          autoInjectOnLaunch: true,
        };
      }

      // Hardened fallback for Mortal Shell if profile was not matched from disk
      if (Object.keys(baseProfile).length === 0 && (exeBase.includes('dungeonhaven') || exeBase.includes('mortalshell') || validId.includes('mortalshell') || validId.includes('1110910') || installPath.toLowerCase().includes('mortalshell'))) {
        baseProfile = {
          id: '1110910',
          name: 'Mortal Shell',
          engine: 'UnrealEngine4',
          api: 'DX11',
          reverseZ: true,
          rowMajorMatrices: true,
          matrixPrecision: 'Float32',
          motionAimSensitivity: 1.0,
          useRecommendedResolution: true,
          srgbCorrection: true,
          depthSubmission: false,
          rawInputMode: true,
          autoInjectOnLaunch: true,
        };
      }

      // Hardened fallback for Hogwarts Legacy if profile was not matched from disk
      if (Object.keys(baseProfile).length === 0 && (exeBase.includes('hogwarts') || exeBase.includes('phoenix') || validId.includes('990080') || installPath.toLowerCase().includes('hogwarts'))) {
        baseProfile = {
          id: '990080',
          name: 'Hogwarts Legacy',
          engine: 'UnrealEngine4',
          api: 'DX12',
          reverseZ: true,
          rowMajorMatrices: true,
          matrixPrecision: 'Float32',
          motionAimSensitivity: 1.0,
          useRecommendedResolution: true,
          srgbCorrection: true,
          depthSubmission: true,
          rawInputMode: true,
          autoInjectOnLaunch: true,
          contrast: 1.20,
          saturation: 1.15,
          brightness: 1.14,
        };
      }

      let activeConfig: Record<string, any> = {
        srgbCorrection: baseProfile.srgbCorrection !== undefined ? baseProfile.srgbCorrection : true,
        ...baseProfile,
      };
      if (fs.existsSync(rootConfigPath)) {
        try {
          const cur = JSON.parse(fs.readFileSync(rootConfigPath, 'utf-8'));
          // Preserve explicit user preferences (including per-game srgbCorrection toggle, MED-03)
          // while enforcing curated profile architectural invariants.
          activeConfig = {
            ...cur,
            ...(baseProfile.engine ? { engine: baseProfile.engine } : {}),
            ...(baseProfile.api ? { api: baseProfile.api } : {}),
            ...(baseProfile.reverseZ !== undefined ? { reverseZ: baseProfile.reverseZ } : {}),
            ...(baseProfile.rowMajorMatrices !== undefined ? { rowMajorMatrices: baseProfile.rowMajorMatrices } : {}),
            ...(baseProfile.matrixPrecision ? { matrixPrecision: baseProfile.matrixPrecision } : {}),
            srgbCorrection: cur.srgbCorrection !== undefined
              ? cur.srgbCorrection
              : (baseProfile.srgbCorrection !== undefined ? baseProfile.srgbCorrection : true),
            ...(baseProfile.depthSubmission !== undefined ? { depthSubmission: baseProfile.depthSubmission } : {}),
            ...(baseProfile.contrast !== undefined ? { contrast: baseProfile.contrast } : (cur.contrast !== undefined ? { contrast: cur.contrast } : {})),
            ...(baseProfile.saturation !== undefined ? { saturation: baseProfile.saturation } : (cur.saturation !== undefined ? { saturation: cur.saturation } : {})),
            ...(baseProfile.brightness !== undefined ? { brightness: baseProfile.brightness } : (cur.brightness !== undefined ? { brightness: cur.brightness } : {})),
          };
        } catch {}
      }

      delete activeConfig.telemetryOptIn; // M-6: Ensure planted vrinject.json or profile never overrides telemetry consent
      activeSessionConfig = activeConfig;

      if (Object.keys(activeConfig).length > 0) {
        try {
          fs.writeFileSync(rootConfigPath, JSON.stringify(activeConfig, null, 2), 'utf-8');
        } catch (e) {}

        for (const d of targetDirs) {
          const destCfg = path.join(d, 'vrinject.json');
          if (destCfg !== rootConfigPath) {
            try {
              fs.writeFileSync(destCfg, JSON.stringify(activeConfig, null, 2), 'utf-8');
            } catch (e) {}
          }
        }

        // Clean up any stale global vrinject.json in updates directory to avoid cross-game configuration contamination
        try {
          const staleStageCfg = path.join(updatesDir, 'vrinject.json');
          if (fs.existsSync(staleStageCfg)) {
            fs.unlinkSync(staleStageCfg);
          }
        } catch (e) {}
      }
    } catch (error) {
      console.warn('Non-fatal error copying shader/model assets to target directory:', error);
    }

    activeTargetExeName = targetExeName;
    activeTargetPid = 0;
    activeGameId = validId;
    cancelInjectionFlag = false;

    // Step 3: Launch ONLY if the game is not already running; otherwise attach cleanly.
    if (alreadyRunning) {
      emitLogLine(
        event,
        `[Injector] Detected already-running game process: ${targetExeName} (PID ${alreadyRunning.pid}, ${Math.round(alreadyRunning.memory / 1024)} MB) — attaching without relaunching.`
      );
    } else {
      emitLogLine(event, `[Injector] Assets synchronized. Launching target game (${targetExeName})...`);
      if (validId.startsWith('custom_') && gameExeMap[validId]) {
        const customExe = canonicalExistingPath(gameExeMap[validId], 'file');
        resolveWithinRoot(installPath, path.relative(installPath, customExe));
        // M-9: Use shell.openPath directly to eliminate cmd.exe %VAR% expansion and command injection
        await shell.openPath(customExe);
      } else if (/^\d+$/.test(validId)) {
        await shell.openExternal(`steam://rungameid/${validId}`);
      } else {
        await shell.openExternal(
          `com.epicgames.launcher://apps/${encodeURIComponent(validId)}?action=launch&silent=true`
        );
      }
      emitLogLine(event, `[Injector] Waiting up to 120s for ${targetExeName} to initialize...`);
    }

    // Step 4: Wait for a stable, initialized game process before triggering UAC elevation.
    // On cold launches (especially Unreal Engine + Steam titles), the shipping binary may
    // briefly spawn at low memory or restart under SteamAPI_RestartAppIfNecessary. Waiting
    // for PID stability + real working memory / visible window avoids premature UAC failures.
    let targetPid = 0;
    let stablePid = 0;
    let stableSinceMs = 0;
    let loggedInitialDetect = false;

    for (let attempts = 0; attempts < 240; attempts++) {
      if (cancelInjectionFlag) {
        stopActiveLogWatch();
        return { success: false, cancelled: true, message: 'Cancelled by user.' };
      }

      if (!alreadyRunning || attempts > 0) {
        await new Promise(resolve => setTimeout(resolve, 500));
      }

      const selectedCandidate = await findRunningGameCandidate(
        targetExeName,
        installPath,
        targetExeDir,
        attempts >= 20
      );

      if (!selectedCandidate) {
        if (stablePid !== 0) {
          emitLogLine(event, `[Injector] Process PID ${stablePid} exited during startup — waiting for relaunched game process...`);
          stablePid = 0;
          stableSinceMs = 0;
        }
        continue;
      }

      const nowMs = Date.now();
      if (selectedCandidate.pid !== stablePid) {
        if (stablePid !== 0) {
          emitLogLine(
            event,
            `[Injector] Game process PID changed (${stablePid} -> ${selectedCandidate.pid}) — stabilizing new PID...`
          );
        } else if (!alreadyRunning && !loggedInitialDetect) {
          emitLogLine(
            event,
            `[Injector] Detected ${targetExeName} (PID ${selectedCandidate.pid}) — waiting for game window & memory readiness...`
          );
          loggedInitialDetect = true;
        }
        stablePid = selectedCandidate.pid;
        stableSinceMs = nowMs;
      }

      const stableDurationMs = nowMs - stableSinceMs;

      if (!alreadyRunning) {
        const lowerPath = selectedCandidate.path.toLowerCase();
        const isShippingBinary = lowerPath.includes('binaries') || lowerPath.includes('shipping');

        // Small bootstrap stub (< 50MB) outside Binaries: wait up to 15s for the real child game binary.
        if (!isShippingBinary && selectedCandidate.memory < 50000 && attempts < 30) {
          continue;
        }

        const windowAndMemReady =
          selectedCandidate.hasWindow && selectedCandidate.memory >= 120000 && stableDurationMs >= 3000;
        const heavyMemReady =
          selectedCandidate.memory >= 200000 && stableDurationMs >= 6000;
        const fallbackStableReady =
          selectedCandidate.memory >= 50000 && stableDurationMs >= 15000;

        if (!windowAndMemReady && !heavyMemReady && !fallbackStableReady) {
          continue;
        }

        emitLogLine(
          event,
          `[Injector] Game process ready: PID ${selectedCandidate.pid} (${Math.round(selectedCandidate.memory / 1024)} MB, window=${selectedCandidate.hasWindow ? 'ready' : 'pending'}, stable ${Math.round(stableDurationMs / 1000)}s).`
        );
      }

      targetPid = selectedCandidate.pid;
      activeTargetPid = selectedCandidate.pid;
      targetExeDir = path.dirname(selectedCandidate.path);
      if (targetExeDir && path.resolve(targetExeDir) !== path.resolve(installPath)) {
        const targetDirLog = path.join(targetExeDir, 'vrinject.log');
        startWatchingLogFile(targetDirLog, event);
      }
      break;
    }

    if (!targetPid) {
      flushWatchedLogs(event);
      stopActiveLogWatch();
      sendDiscordTelemetry({
        gameId: validId,
        gameName: targetExeName || validId,
        status: 'error',
        message: 'Game executable not found or closed immediately before injection.',
        config: activeSessionConfig,
        logFilePath: currentSessionLogPath || latestSessionLogPath,
      }).catch(() => {});
      return { success: false, message: 'Game executable not found or closed immediately' };
    }

    const bundledDll = resolveWithinRoot(canonicalBinSourceDir, 'vrinject.dll');
    const canonicalDll = fs.existsSync(bundledDll)
      ? canonicalExistingPath(bundledDll, 'file')
      : bundledDll;
    const otaDll = path.join(app.getPath('userData'), 'updates', 'vrinject.dll');
    const sourceDll = pickPreferredAsset(canonicalDll, otaDll, 100000);
    let dllTarget = resolveWithinRoot(targetExeDir, 'vrinject.dll');
    
    // Check if target directory DLL matches sourceDll SHA-256 (not merely mtimeMs, which can be stale across upgrades/rollbacks).
    // If missing, hash-mismatched, or locked by a running game, fall back to sourceDll so elevated vr-inject-cli verifies & deploys it.
    try {
      if (fs.existsSync(sourceDll)) {
        const srcHash = crypto.createHash('sha256').update(fs.readFileSync(sourceDll)).digest('hex');
        let dstMatches = false;
        if (fs.existsSync(dllTarget)) {
          try {
            const dstHash = crypto.createHash('sha256').update(fs.readFileSync(dllTarget)).digest('hex');
            dstMatches = (dstHash === srcHash);
          } catch {
            dstMatches = false;
          }
        }
        if (!dstMatches) {
          try {
            fs.copyFileSync(sourceDll, dllTarget);
          } catch {
            dllTarget = sourceDll;
          }
        }
      }
    } catch {
      dllTarget = sourceDll;
    }

    if (!fs.existsSync(cliSource)) {
      stopActiveLogWatch();
      return {
        success: false,
        message: `Injector CLI binary not found at '${cliSource}'. Please reinstall or run updates.`,
      };
    }

    // CRIT-01 & CRIT-02 Hardening:
    // 1. Write a restrictive-ACL JSON job file containing authToken, paths, and expected exe/install dir.
    //    No secrets or user/game paths ever cross PowerShell string interpolation or process command lines!
    // 2. Pass ONLY canonicalBinSourceDir (bundled/local signed resources) as copySrc so elevated CLI
    //    never copies unverified support DLLs from user-writable %APPDATA%.
    // 3. Pass cliSource and jobFilePath into PowerShell via pure Base64 UTF-8 ([A-Za-z0-9+/=]),
    //    completely immune to ASCII/Unicode smart-quote breakout (U+2018/U+2019/etc.).
    const jobsDir = path.join(app.getPath('userData'), 'jobs');
    fs.mkdirSync(jobsDir, { recursive: true });
    const jobNonce = crypto.randomBytes(16).toString('hex');
    const jobFilePath = path.join(jobsDir, `job_${jobNonce}.json`);
    const resultFilePath = path.join(jobsDir, `res_${jobNonce}.json`);
    const jobPayload = {
      authToken: process.env.STEREIX_AUTH_TOKEN || process.env.NEXVR_AUTH_TOKEN || '',
      pid: targetPid,
      dllPath: dllTarget,
      copySrc: canonicalBinSourceDir,
      copyDst: targetExeDir,
      logPath,
      expectedExeName: targetExeName,
      expectedInstallPath: installPath,
      resultFile: resultFilePath,
      allowUnverifiedHotfix: !app.isPackaged || process.env.NODE_ENV === 'development',
    };
    fs.writeFileSync(jobFilePath, JSON.stringify(jobPayload), { encoding: 'utf-8', mode: 0o600 });
    try {
      const currentUser = process.env.USERNAME || '';
      const userDomain = process.env.USERDOMAIN || '';
      if (currentUser && /^[A-Za-z0-9._ -]+$/.test(currentUser)) {
        const principal = (userDomain && /^[A-Za-z0-9._-]+$/.test(userDomain))
          ? `${userDomain}\\${currentUser}`
          : currentUser;
        // Use locale-independent well-known SIDs for SYSTEM (*S-1-5-18) and Administrators (*S-1-5-32-544)
        await execFileAsync(
          'icacls.exe',
          [jobFilePath, '/inheritance:r', '/grant:r', `${principal}:(R,W)`, '*S-1-5-18:(F)', '*S-1-5-32-544:(F)'],
          { timeout: 3000, windowsHide: true }
        ).catch(() => {});
      }
    } catch {}

    const cleanupJobFiles = () => {
      try { if (fs.existsSync(jobFilePath)) fs.unlinkSync(jobFilePath); } catch {}
      try { if (fs.existsSync(resultFilePath)) fs.unlinkSync(resultFilePath); } catch {}
    };

    const cliB64 = Buffer.from(cliSource, 'utf-8').toString('base64');
    const jobB64 = Buffer.from(jobFilePath, 'utf-8').toString('base64');
    const innerScript =
      `$cli = [System.Text.Encoding]::UTF8.GetString([System.Convert]::FromBase64String('${cliB64}')); ` +
      `$job = [System.Text.Encoding]::UTF8.GetString([System.Convert]::FromBase64String('${jobB64}')); ` +
      `& $cli --job-file $job; ` +
      `exit $LASTEXITCODE`;
    const base64Inner = Buffer.from(innerScript, 'utf16le').toString('base64');
    const outerScript =
      `$p = Start-Process powershell -Verb RunAs -Wait -PassThru -WindowStyle Hidden ` +
      `-ArgumentList "-NoProfile", "-ExecutionPolicy", "Bypass", ` +
      `"-EncodedCommand", "${base64Inner}"; ` +
      `if ($p.ExitCode -ne 0) { exit $p.ExitCode }`;
    const base64Outer = Buffer.from(outerScript, 'utf16le').toString('base64');

    return await new Promise<InjectResult>((resolve) => {
      let resolved = false;
      const finish = (result: InjectResult) => {
        if (resolved) return;
        resolved = true;
        clearTimeout(timeout);
        cleanupJobFiles();
        // Flush any newly written CLI output before stopping log watchers so the UI sees root cause lines
        flushWatchedLogs(event);
        if (!result.success) stopActiveLogWatch();
        resolve(result);
      };

      const timeout = setTimeout(() => {
        finish({
          success: false,
          message: 'Injector timed out or was blocked by Anti-Cheat. (UAC timeout?)',
        });
      }, 180000);

      child_process.execFile(
        'powershell.exe',
        ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-EncodedCommand', base64Outer],
        {
          timeout: 180000,
          killSignal: 'SIGKILL',
          cwd: installPath,
          windowsHide: true,
        },
        (error) => {
          // Ensure the final lines written by vr-inject-cli.exe are flushed into the UI log stream
          flushWatchedLogs(event);

          if (error) {
            const { message: cliReason, isUacCancelled } = extractCliFailureReason(logPath, error);
            emitLogLine(event, `[Injector] ${cliReason}`);
            sendDiscordTelemetry({
              gameId: validId,
              gameName: targetExeName || validId,
              status: 'error',
              message: `Injection CLI failed: ${cliReason}`,
              config: activeSessionConfig,
              logFilePath: currentSessionLogPath || latestSessionLogPath,
            }).catch(() => {});
            finish({
              success: false,
              cancelled: isUacCancelled,
              message: cliReason,
            });
          } else {
            // Read back post-UAC re-resolved PID from elevated CLI so monitor/cancel stay synchronized (CRIT-02)
            try {
              if (fs.existsSync(resultFilePath)) {
                const resJson = JSON.parse(fs.readFileSync(resultFilePath, 'utf-8'));
                if (Number.isSafeInteger(resJson?.resolvedPid) && resJson.resolvedPid > 4 && resJson.resolvedPid !== targetPid) {
                  targetPid = resJson.resolvedPid;
                  activeTargetPid = targetPid;
                  emitLogLine(event, `[Injector] Re-resolved live game PID after UAC: ${targetPid}`);
                }
              }
            } catch {}

            activeSessionStartTime = Date.now();
            sendDiscordTelemetry({
              gameId: validId,
              gameName: targetExeName,
              status: 'started',
              message: `Stereix Engine successfully injected into ${targetExeName} (PID ${targetPid}).`,
              config: activeSessionConfig,
              logFilePath: currentSessionLogPath || latestSessionLogPath,
            }).catch(() => {});
            finish({ success: true, message: 'Deployed successfully', pid: targetPid });
          }
        }
      );
    });
  } catch (error: any) {
    stopActiveLogWatch();
    sendDiscordTelemetry({
      gameId: activeGameId || 'error_session',
      gameName: activeTargetExeName,
      status: 'error',
      message: `Exception during injection: ${error?.message || String(error)}`,
      config: activeSessionConfig,
      logFilePath: currentSessionLogPath || latestSessionLogPath,
    }).catch(() => {});
    return { success: false, message: error?.message || String(error) };
  } finally {
    injectionInProgress = false;
  }
});

ipcMain.handle('inject:monitor', async (event, pid: number) => {
  assertTrustedIpcSender(event);
  if (!Number.isSafeInteger(pid) || pid <= 0 || pid !== activeTargetPid) {
    throw new Error('Invalid process monitor request');
  }

  // Max 43200 iterations (approx 24 hours at 2s per check)
  for (let i = 0; i < 43200; i++) {
    try {
      const { stdout } = await execFileAsync(
        'tasklist.exe',
        ['/fi', `PID eq ${pid}`, '/fo', 'csv', '/nh'],
        { encoding: 'utf-8', timeout: 2000, killSignal: 'SIGKILL', windowsHide: true }
      );
      if (!stdout.includes(`"${pid}"`)) break;
    } catch {
      break;
    }
    await new Promise(resolve => setTimeout(resolve, 2000));
  }

  const durationSec = activeSessionStartTime > 0 ? Math.floor((Date.now() - activeSessionStartTime) / 1000) : 0;
  const finishedGameId = activeGameId;
  const finishedExeName = activeTargetExeName;
  const logFileToSend = currentSessionLogPath || latestSessionLogPath;

  stopActiveLogWatch();
  activeTargetPid = 0;
  activeTargetExeName = '';
  activeGameId = '';
  activeSessionStartTime = 0;

  sendDiscordTelemetry({
    gameId: finishedGameId || 'completed_session',
    gameName: finishedExeName,
    status: 'completed',
    message: `Game session ended normally. Total playtime: ${durationSec}s.`,
    durationSec,
    logFilePath: logFileToSend,
  }).catch(() => {});
});

// 1-Click "Disable VR / Play Flat" Uninstaller
ipcMain.handle('inject:uninstall', async (event, id: string) => {
  assertTrustedIpcSender(event);
  const validId = validateGameId(id);
  const registeredInstallPath = gamePathsMap[validId];
  if (!registeredInstallPath) return { success: false, message: 'Game path not found' };
  const installPath = canonicalExistingPath(registeredInstallPath, 'directory');

  try {
    // Collect EVERY deployed directory under installPath (deploy writes to installPath, targetExeDir, and Binaries/Win64)
    // without ever traversing symlinks or NTFS junctions (MED-04).
    const dirsToClean = new Set<string>([installPath]);
    const registeredExe = gameExeMap[validId];
    if (registeredExe) {
      try {
        const canonicalExe = canonicalExistingPath(registeredExe, 'file');
        resolveWithinRoot(installPath, path.relative(installPath, canonicalExe));
        dirsToClean.add(path.dirname(canonicalExe));
      } catch {}
    }
    for (const sub of ['Phoenix/Binaries/Win64', 'Chameleon/Binaries/Win64', 'Dungeonhaven/Binaries/Win64', 'Binaries/Win64']) {
      try {
        const subDir = resolveWithinRoot(installPath, sub);
        if (fs.existsSync(subDir) && !fs.lstatSync(subDir).isSymbolicLink()) {
          const canonicalSub = canonicalExistingPath(subDir, 'directory');
          resolveWithinRoot(installPath, path.relative(installPath, canonicalSub));
          dirsToClean.add(canonicalSub);
        }
      } catch {}
    }
    const scanForDeployedDirs = (dir: string, depth = 0): void => {
      if (depth > 6) return;
      try {
        const entries = fs.readdirSync(dir, { withFileTypes: true });
        for (const e of entries) {
          if (e.isSymbolicLink()) continue;
          const lower = e.name.toLowerCase();
          if (e.isFile() && (lower === 'vrinject.dll' || lower === '.stereix_deployed.json')) {
            dirsToClean.add(dir);
          } else if (e.isDirectory() && !lower.includes('support')) {
            try {
              const child = resolveWithinRoot(installPath, path.relative(installPath, path.join(dir, e.name)));
              if (fs.lstatSync(child).isSymbolicLink()) continue;
              const realChild = fs.realpathSync.native(child);
              resolveWithinRoot(installPath, path.relative(installPath, realChild));
              scanForDeployedDirs(realChild, depth + 1);
            } catch {}
          }
        }
      } catch {}
    };
    scanForDeployedDirs(installPath);

    const stereixExclusiveFiles = new Set(['vrinject.dll', 'vrinject.log']);
    const candidateFiles = ['dxgi.dll', 'd3d11.dll', 'vrinject.dll', 'onnxruntime.dll', 'DirectML.dll', 'openxr_loader.dll', 'vrinject.log'];
    let removedCount = 0;

    for (const dir of dirsToClean) {
      // Remove ONLY Stereix-owned deployed VR injection files (MED-04: never delete pre-existing game DLLs/shaders)
      const ownershipManifestPath = safeGamePath(dir, '.stereix_deployed.json');
      let ownedFiles: Set<string> | null = null;
      let ownedDirs: Set<string> | null = null;
      if (fs.existsSync(ownershipManifestPath)) {
        try {
          if (!fs.lstatSync(ownershipManifestPath).isSymbolicLink()) {
            const parsed = JSON.parse(fs.readFileSync(ownershipManifestPath, 'utf-8'));
            ownedFiles = new Set<string>(Array.isArray(parsed.files) ? parsed.files : []);
            ownedDirs = new Set<string>(Array.isArray(parsed.dirs) ? parsed.dirs : []);
          }
        } catch {}
      }

      for (const f of candidateFiles) {
        // Shared runtime DLLs and proxy DLLs are only removed if recorded as Stereix-owned
        const isOwned = stereixExclusiveFiles.has(f) || (ownedFiles !== null && ownedFiles.has(f));
        if (!isOwned) continue;
        const p = safeGamePath(dir, f);
        if (fs.existsSync(p)) {
          try {
            if (!fs.lstatSync(p).isSymbolicLink()) {
              fs.unlinkSync(p);
              removedCount++;
            }
          } catch {}
        }
      }
      for (const dirName of ['shaders', 'models']) {
        if (ownedDirs !== null && ownedDirs.has(dirName)) {
          const targetSub = safeGamePath(dir, dirName);
          if (fs.existsSync(targetSub)) {
            try {
              if (!fs.lstatSync(targetSub).isSymbolicLink()) {
                fs.rmSync(targetSub, { recursive: true, force: true });
                removedCount++;
              }
            } catch {}
          }
        }
      }
      if (fs.existsSync(ownershipManifestPath)) {
        try {
          if (!fs.lstatSync(ownershipManifestPath).isSymbolicLink()) {
            fs.unlinkSync(ownershipManifestPath);
          }
        } catch {}
      }
    }

    console.info(`[Injector] Uninstalled VR mod from ${dirsToClean.size} directory(ies) under ${installPath} (${removedCount} items removed)`);
    return {
      success: true,
      message: 'VR Mod uninstalled successfully. Game is now restored to original flat screen mode.'
    };
  } catch (err: any) {
    return { success: false, message: `Failed to uninstall VR mod: ${err.message}` };
  }
});

