import { app, ipcMain } from 'electron';
import * as fs from 'fs';
import * as path from 'path';
import * as crypto from 'crypto';
import { VRConfig } from '../src/types';
import { assertTrustedIpcSender, gamePathsMap, gameExeMap, validateGameId, validateConfig, safeGamePath } from './utils';
import { getLocalManifest, compareSemver, getAppVersion } from './updateManager';

const defaultVRConfig: VRConfig = {
  motionAimSensitivity: 1.0,
  useRecommendedResolution: true,
  srgbCorrection: true,
  depthSubmission: false,
  rawInputMode: true,
  autoInjectOnLaunch: true,
  contrast: 1.0,
  saturation: 1.0,
  brightness: 1.0,
  telemetryOptIn: false,
};

const curatedProfiles: Record<string, Partial<VRConfig>> = {
  // Hogwarts Legacy (Steam / Epic)
  '990080': {
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
  },
  'd0614ddf466b44a2a229a43a75db9efd': {
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
  },
  // Cyberpunk 2077
  '1091500': {
    engine: 'Generic',
    api: 'DX12',
    reverseZ: true,
    rowMajorMatrices: true,
    matrixPrecision: 'Float32',
    motionAimSensitivity: 1.2,
    useRecommendedResolution: true,
    srgbCorrection: true,
    depthSubmission: true,
    rawInputMode: true,
    autoInjectOnLaunch: true,
  },
  // Elden Ring
  '1245620': {
    engine: 'Generic',
    api: 'DX12',
    reverseZ: true,
    rowMajorMatrices: true,
    matrixPrecision: 'Float32',
    motionAimSensitivity: 0.9,
    useRecommendedResolution: true,
    srgbCorrection: true,
    depthSubmission: false,
    rawInputMode: true,
    autoInjectOnLaunch: true,
  },
  // Atomic Heart
  '668580': {
    engine: 'UnrealEngine4',
    api: 'DX12',
    reverseZ: true,
    rowMajorMatrices: true,
    matrixPrecision: 'Float32',
    motionAimSensitivity: 1.1,
    useRecommendedResolution: true,
    srgbCorrection: true,
    depthSubmission: true,
    rawInputMode: true,
    autoInjectOnLaunch: true,
  },
  // Mortal Shell
  '1110910': {
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
  },
  // Palworld
  '1623730': {
    engine: 'UnrealEngine5',
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
  },
  // Sekiro: Shadows Die Twice
  '814380': {
    engine: 'Generic',
    api: 'DX11',
    reverseZ: false,
    rowMajorMatrices: false,
    matrixPrecision: 'Float32',
    motionAimSensitivity: 1.0,
    useRecommendedResolution: true,
    srgbCorrection: true,
    depthSubmission: true,
    rawInputMode: true,
    autoInjectOnLaunch: true,
  },
  // No Man's Sky
  '275850': {
    engine: 'Generic',
    api: 'Vulkan',
    reverseZ: false,
    rowMajorMatrices: false,
    matrixPrecision: 'Float32',
    motionAimSensitivity: 1.0,
    useRecommendedResolution: true,
    srgbCorrection: true,
    depthSubmission: false,
    rawInputMode: true,
    autoInjectOnLaunch: true,
  },
  // The Elder Scrolls V: Skyrim Special Edition
  '489830': {
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
  },
  // Fallout 4
  '377160': {
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
  },
  // Valheim
  '892970': {
    engine: 'Unity',
    api: 'Vulkan',
    reverseZ: true,
    rowMajorMatrices: false,
    matrixPrecision: 'Float32',
    motionAimSensitivity: 1.0,
    useRecommendedResolution: true,
    srgbCorrection: true,
    depthSubmission: false,
    rawInputMode: true,
    autoInjectOnLaunch: true,
  },
  // Monster Hunter: World
  '582010': {
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
  },
};

function loadProfilesFromDisk(): Record<string, Partial<VRConfig>> {
  const profiles: Record<string, Partial<VRConfig>> = Object.assign(Object.create(null), curatedProfiles);
  try {
    let userUpdatesProfiles = '';
    let customProfilesDir = '';
    try {
      userUpdatesProfiles = path.join(app.getPath('userData'), 'updates', 'profiles');
      customProfilesDir = path.join(app.getPath('userData'), 'custom_profiles');
      if (!fs.existsSync(customProfilesDir)) {
        fs.mkdirSync(customProfilesDir, { recursive: true });
      }
    } catch {}

    // Check root profiles directory (development, packaged resources, OTA updates, and custom user profiles)
    // Ordered highest-priority-first, then reversed below because the load loop applies last-writer-wins.
    const localDirs = [
      path.resolve(__dirname, '../../../../stereix-client/profiles'),
      path.resolve(__dirname, '../../../../nexvr-client/profiles'),
      path.resolve(__dirname, '../../../profiles'),
      path.resolve(__dirname, '../../profiles'),
      path.join(process.resourcesPath, 'profiles'),
    ];
    const localDirsLowToHigh = [...localDirs].reverse();
    const localManifest = app?.isPackaged ? getLocalManifest() : null;
    const hasVerifiedNewerOta = Boolean(
      localManifest &&
      localManifest.hashes &&
      compareSemver(localManifest.engineVersion, getAppVersion()) >= 0
    );

    // HIGH-13 / BUG-19: Loop is last-writer-wins, so order LOWEST priority first -> HIGHEST priority last.
    // Dev mode: custom_profiles < local repo profiles (never let OTA cache overwrite local dev profiles).
    // Packaged mode: bundled local profiles < verified OTA hotfix profiles < user custom_profiles.
    const candidateDirs = (!app?.isPackaged)
      ? [
          ...(customProfilesDir ? [customProfilesDir] : []),
          ...localDirsLowToHigh,
        ]
      : [
          ...localDirsLowToHigh,
          ...(hasVerifiedNewerOta && userUpdatesProfiles ? [userUpdatesProfiles] : []),
          ...(customProfilesDir ? [customProfilesDir] : []),
        ];
    for (const pDir of candidateDirs) {
      if (fs.existsSync(pDir)) {
        const isOtaDir = Boolean(userUpdatesProfiles && pDir === userUpdatesProfiles);
        const files = fs.readdirSync(pDir);
        for (const f of files) {
          if (f.endsWith('.json') && f !== 'schema.json') {
            try {
              const fullProfilePath = path.join(pDir, f);
              const rawBytes = fs.readFileSync(fullProfilePath);
              if (isOtaDir) {
                // HIGH-10: Fail-closed SHA-256 verification for every OTA profile against the signed manifest
                const expectedHash = localManifest?.hashes?.[`profiles/${f}`];
                if (!expectedHash || typeof expectedHash !== 'string' || expectedHash.trim().length !== 64) {
                  continue;
                }
                const actualHash = crypto.createHash('sha256').update(rawBytes).digest('hex');
                if (actualHash.toLowerCase() !== expectedHash.toLowerCase()) {
                  console.warn(`[ConfigManager] Rejecting unverified OTA profile ${f}: SHA-256 mismatch.`);
                  continue;
                }
              }
              const data = JSON.parse(rawBytes.toString('utf-8'));
              if (data && typeof data.id === 'string') {
                const safeProfileId = validateGameId(data.id);
                profiles[safeProfileId] = {
                  motionAimSensitivity: typeof data.motionAimSensitivity === 'number' ? data.motionAimSensitivity : 1.0,
                  useRecommendedResolution: data.useRecommendedResolution !== false,
                  srgbCorrection: typeof data.srgbCorrection === 'boolean' ? data.srgbCorrection : true,
                  depthSubmission: Boolean(data.depthSubmission),
                  rawInputMode: data.rawInputMode !== false,
                  autoInjectOnLaunch: data.autoInjectOnLaunch !== false,
                  engine: typeof data.engine === 'string' ? data.engine : undefined,
                  api: typeof data.api === 'string' ? data.api : undefined,
                  reverseZ: typeof data.reverseZ === 'boolean' ? data.reverseZ : undefined,
                  rowMajorMatrices: typeof data.rowMajorMatrices === 'boolean' ? data.rowMajorMatrices : undefined,
                  matrixPrecision: typeof data.matrixPrecision === 'string' ? data.matrixPrecision : undefined,
                  contrast: typeof data.contrast === 'number' ? data.contrast : undefined,
                  saturation: typeof data.saturation === 'number' ? data.saturation : undefined,
                  brightness: typeof data.brightness === 'number' ? data.brightness : undefined,
                  convergence: typeof data.convergence === 'number' ? data.convergence : undefined,
                  ipd: typeof data.ipd === 'number' ? data.ipd : undefined,
                  forceCinemaMode: typeof data.forceCinemaMode === 'boolean' ? data.forceCinemaMode : undefined,
                  theaterDistance: typeof data.theaterDistance === 'number' ? data.theaterDistance : undefined,
                  aiNotes: typeof data.aiNotes === 'string' ? data.aiNotes : undefined,
                };
              }
            } catch {}
          }
        }
      }
    }
  } catch {}
  return profiles;
}

let cachedProfiles: Record<string, Partial<VRConfig>> | null = null;
let lastProfileLoad = 0;

export function invalidateProfileCache(): void {
  cachedProfiles = null;
  lastProfileLoad = 0;
}

export function getActiveProfiles(): Record<string, Partial<VRConfig>> {
  const now = Date.now();
  if (!cachedProfiles || now - lastProfileLoad > 5000) {
    cachedProfiles = loadProfilesFromDisk();
    lastProfileLoad = now;
  }
  return cachedProfiles;
}

ipcMain.handle('config:reloadProfiles', async (event) => {
  assertTrustedIpcSender(event);
  cachedProfiles = loadProfilesFromDisk();
  lastProfileLoad = Date.now();
  return { success: true, count: Object.keys(cachedProfiles).length };
});

ipcMain.handle('config:read', async (event, id: string): Promise<VRConfig> => {
  try {
    assertTrustedIpcSender(event);
    const validId = validateGameId(id);
    if (validId === 'global') {
      const globalCfgPath = path.join(app.getPath('userData'), 'global_config.json');
      if (fs.existsSync(globalCfgPath)) {
        try {
          const parsed = JSON.parse(fs.readFileSync(globalCfgPath, 'utf-8'));
          return validateConfig({ ...defaultVRConfig, ...parsed });
        } catch {}
      }
      return defaultVRConfig;
    }

    const installPath = gamePathsMap[validId];
    const registeredExe = gameExeMap[validId];
    const activeProfiles = getActiveProfiles();
    let matchedProfile = activeProfiles[validId];
    if (!matchedProfile) {
      const exeName = registeredExe ? path.basename(registeredExe, '.exe').toLowerCase() : '';
      const dirName = installPath ? path.basename(installPath).toLowerCase() : '';
      if (exeName.includes('sekiro') || dirName.includes('sekiro')) {
        matchedProfile = activeProfiles['814380'];
      } else if (exeName.includes('dungeonhaven') || dirName.includes('mortalshell') || exeName.includes('mortalshell')) {
        matchedProfile = activeProfiles['1110910'];
      } else if (exeName.includes('hogwarts') || dirName.includes('hogwarts') || exeName.includes('phoenix')) {
        matchedProfile = activeProfiles['990080'];
      } else {
        for (const [pId, pCfg] of Object.entries(activeProfiles)) {
          if (exeName && (pId.includes(exeName) || (pCfg as any).name?.toLowerCase().includes(exeName))) {
            matchedProfile = pCfg;
            break;
          }
        }
      }
    }
    const initialConfig = { ...defaultVRConfig, ...(matchedProfile || {}) };
    if (!installPath) return initialConfig;

    const targetDir = registeredExe ? path.dirname(registeredExe) : undefined;
    // Check target binary directory first, then root install directory
    const candidatePaths: string[] = [];
    if (targetDir) candidatePaths.push(path.join(targetDir, 'vrinject.json'));
    candidatePaths.push(safeGamePath(installPath, 'vrinject.json'));

    for (const cfgPath of candidatePaths) {
      if (fs.existsSync(cfgPath)) {
        const content = fs.readFileSync(cfgPath, 'utf-8');
        const parsed = JSON.parse(content);
        // MED-03: Respect explicit user/profile srgbCorrection value (do not force-overwrite false -> true)
        return validateConfig({ ...initialConfig, ...parsed });
      }
    }
    return initialConfig;
  } catch (e) {
    console.error('config:read error', e);
    return defaultVRConfig;
  }
});

ipcMain.handle('config:write', async (event, id: string, cfg: unknown) => {
  try {
    assertTrustedIpcSender(event);
    const cfgString = JSON.stringify(cfg);
    if (cfgString.length > 10240) throw new Error('Payload too large (max 10KB)');
    
    const validId = validateGameId(id);
    const validCfg = validateConfig(cfg);
    if (validId === 'global') {
      const globalCfgPath = path.join(app.getPath('userData'), 'global_config.json');
      fs.writeFileSync(globalCfgPath, JSON.stringify(validCfg, null, 2), 'utf-8');
      return { success: true };
    }

    const installPath = gamePathsMap[validId];
    if (!installPath) return { success: false, error: 'Game path not found' };

    const registeredExe = gameExeMap[validId];
    const targetDir = registeredExe ? path.dirname(registeredExe) : undefined;

    // Synchronize config to both installPath and target shipping binary directory
    const dirsToSync = new Set<string>([installPath]);
    if (targetDir && fs.existsSync(targetDir)) dirsToSync.add(targetDir);
    for (const sub of ['Phoenix/Binaries/Win64', 'Chameleon/Binaries/Win64', 'Dungeonhaven/Binaries/Win64', 'Binaries/Win64']) {
      const subDir = path.join(installPath, sub);
      if (fs.existsSync(subDir)) dirsToSync.add(subDir);
    }

    const baseProfile = getActiveProfiles()[validId] || {};
    const finalCfg: VRConfig = {
      ...validCfg,
      srgbCorrection: validCfg.srgbCorrection,
      engine: validCfg.engine ?? baseProfile.engine,
      api: validCfg.api ?? baseProfile.api,
      reverseZ: validCfg.reverseZ ?? baseProfile.reverseZ,
      rowMajorMatrices: validCfg.rowMajorMatrices ?? baseProfile.rowMajorMatrices,
      matrixPrecision: validCfg.matrixPrecision ?? baseProfile.matrixPrecision,
      convergence: validCfg.convergence ?? baseProfile.convergence,
      ipd: validCfg.ipd ?? baseProfile.ipd,
      forceCinemaMode: validCfg.forceCinemaMode ?? baseProfile.forceCinemaMode,
      theaterDistance: validCfg.theaterDistance ?? baseProfile.theaterDistance,
      aiNotes: validCfg.aiNotes ?? baseProfile.aiNotes,
    };

    for (const dir of dirsToSync) {
      const cfgPath = path.join(dir, 'vrinject.json');
      // F-5: Read existing vrinject.json to preserve C++-managed fields (IPD, HUD, comfort, shaders, etc.)
      let existingCfg: Record<string, any> = {};
      if (fs.existsSync(cfgPath)) {
        try {
          existingCfg = JSON.parse(fs.readFileSync(cfgPath, 'utf-8'));
        } catch {}
      }
      const mergedCfg = {
        ...existingCfg,
        ...finalCfg,
      };
      const tempPath = path.join(dir, `vrinject.${process.pid}.tmp`);
      fs.writeFileSync(tempPath, JSON.stringify(mergedCfg, null, 2));
      fs.renameSync(tempPath, cfgPath);
      try {
        fs.chmodSync(cfgPath, 0o600); // S6.3: Owner rw only
      } catch {}
    }
    return { success: true };
  } catch (e: any) {
    return { success: false, error: e.message };
  }
});
