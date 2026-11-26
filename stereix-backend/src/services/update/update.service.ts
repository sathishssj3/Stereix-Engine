import { db } from '../../common/db.js';
import { getPresignedDownloadUrl, getPresignedUploadUrl } from '../../common/storage.js';
import { getCached, setCached, invalidateCache } from '../../common/redis.js';
import { BadRequestError } from '../../common/errors.js';
import { ReleaseChannel } from '@prisma/client';

const SAFE_UPLOAD_FILENAME_RE = /^[a-zA-Z0-9._-]{1,128}$/;
const SAFE_RELEASE_KEY_RE = /^releases\/[a-zA-Z0-9._/-]{1,256}$/;

export function validateUploadFilename(filename: string): string {
  if (
    typeof filename !== 'string' ||
    !SAFE_UPLOAD_FILENAME_RE.test(filename) ||
    filename.includes('..') ||
    filename.startsWith('.')
  ) {
    throw new BadRequestError('Invalid upload filename: path traversal or disallowed characters rejected');
  }
  return filename;
}

export function validateReleaseKey(key: string, fieldName: string): string {
  if (
    typeof key !== 'string' ||
    !SAFE_RELEASE_KEY_RE.test(key) ||
    key.includes('..') ||
    key.includes('//') ||
    key.includes('\\')
  ) {
    throw new BadRequestError(`Invalid ${fieldName}: must be a safe key under releases/ without traversal`);
  }
  return key;
}

interface ParsedSemver {
  major: number;
  minor: number;
  patch: number;
  prerelease: string[];
}

function parseSemver(v: string): ParsedSemver {
  const clean = String(v || '0.0.0').trim().replace(/^v/i, '');
  const [withoutBuild] = clean.split('+');
  const dashIdx = withoutBuild.indexOf('-');
  const coreStr = dashIdx >= 0 ? withoutBuild.slice(0, dashIdx) : withoutBuild;
  const preStr = dashIdx >= 0 ? withoutBuild.slice(dashIdx + 1) : '';
  const coreParts = coreStr.split('.').map(p => Number.parseInt(p, 10) || 0);
  return {
    major: coreParts[0] || 0,
    minor: coreParts[1] || 0,
    patch: coreParts[2] || 0,
    prerelease: preStr ? preStr.split('.').filter(Boolean) : [],
  };
}

export class UpdateService {
  async checkForUpdates(currentVersion: string, channel: ReleaseChannel = ReleaseChannel.STABLE) {
    const cacheKey = `update:latest:${channel}`;
    let latest = await getCached<any>(cacheKey);

    if (!latest) {
      latest = await db.release.findFirst({
        where: { channel },
        orderBy: { createdAt: 'desc' },
      });
      if (latest) {
        await setCached(cacheKey, latest, 180);
      }
    }

    if (!latest) {
      return { updateAvailable: false, currentVersion };
    }

    const hasNewerVersion = this.compareSemver(latest.version, currentVersion) > 0;
    if (!hasNewerVersion) {
      return { updateAvailable: false, currentVersion };
    }

    const installerDownloadUrl = await getPresignedDownloadUrl(latest.windowsInstallerUrl);
    const cliDownloadUrl = latest.cliUrl ? await getPresignedDownloadUrl(latest.cliUrl) : null;
    const dllDownloadUrl = latest.dllUrl ? await getPresignedDownloadUrl(latest.dllUrl) : null;

    return {
      updateAvailable: true,
      currentVersion,
      latestVersion: latest.version,
      channel: latest.channel,
      releaseNotes: latest.releaseNotes,
      isMandatory: latest.isMandatory,
      sha256Hash: latest.sha256Hash,
      downloads: {
        installer: installerDownloadUrl,
        cli: cliDownloadUrl,
        dll: dllDownloadUrl,
      },
    };
  }

  async publishRelease(data: {
    version: string;
    channel: ReleaseChannel;
    releaseNotes?: string;
    isMandatory?: boolean;
    installerKey: string;
    cliKey?: string;
    dllKey?: string;
    sha256Hash: string;
  }) {
    // HIGH-03: Strictly enforce releases/ prefix and disallow traversal on all S3 artifact keys
    const installerKey = validateReleaseKey(data.installerKey, 'installerKey');
    const cliKey = data.cliKey ? validateReleaseKey(data.cliKey, 'cliKey') : undefined;
    const dllKey = data.dllKey ? validateReleaseKey(data.dllKey, 'dllKey') : undefined;

    const release = await db.release.upsert({
      where: { version: data.version },
      update: {
        channel: data.channel,
        releaseNotes: data.releaseNotes,
        isMandatory: data.isMandatory || false,
        windowsInstallerUrl: installerKey,
        cliUrl: cliKey,
        dllUrl: dllKey,
        sha256Hash: data.sha256Hash,
      },
      create: {
        version: data.version,
        channel: data.channel,
        releaseNotes: data.releaseNotes,
        isMandatory: data.isMandatory || false,
        windowsInstallerUrl: installerKey,
        cliUrl: cliKey,
        dllUrl: dllKey,
        sha256Hash: data.sha256Hash,
      },
    });

    await invalidateCache(`update:latest:${data.channel}`);
    return release;
  }

  async getUploadUrl(filename: string, contentType: string) {
    // HIGH-03: Prevent S3 key traversal via filename (reject .., /, \, leading dot)
    const safeFilename = validateUploadFilename(filename);
    const key = `releases/raw/${Date.now()}-${safeFilename}`;
    const uploadUrl = await getPresignedUploadUrl(key, contentType);
    return { key, uploadUrl };
  }

  compareSemver(v1: string, v2: string): number {
    // HIGH-03: SemVer 2.0.0 compliant comparison including prereleases (e.g. 1.2.3-beta > 1.2.1 and 1.2.3 > 1.2.3-beta)
    const p1 = parseSemver(v1);
    const p2 = parseSemver(v2);

    if (p1.major !== p2.major) return p1.major > p2.major ? 1 : -1;
    if (p1.minor !== p2.minor) return p1.minor > p2.minor ? 1 : -1;
    if (p1.patch !== p2.patch) return p1.patch > p2.patch ? 1 : -1;

    const hasPre1 = p1.prerelease.length > 0;
    const hasPre2 = p2.prerelease.length > 0;
    if (!hasPre1 && !hasPre2) return 0;
    if (!hasPre1 && hasPre2) return 1;  // Release > prerelease
    if (hasPre1 && !hasPre2) return -1; // Prerelease < release

    const len = Math.max(p1.prerelease.length, p2.prerelease.length);
    for (let i = 0; i < len; i++) {
      const id1 = p1.prerelease[i];
      const id2 = p2.prerelease[i];
      if (id1 === undefined) return -1;
      if (id2 === undefined) return 1;
      if (id1 === id2) continue;

      const isNum1 = /^\d+$/.test(id1);
      const isNum2 = /^\d+$/.test(id2);
      if (isNum1 && isNum2) {
        const n1 = Number(id1);
        const n2 = Number(id2);
        return n1 > n2 ? 1 : -1;
      }
      if (isNum1 && !isNum2) return -1; // Numeric identifiers have lower precedence than non-numeric
      if (!isNum1 && isNum2) return 1;
      return id1 > id2 ? 1 : -1;
    }
    return 0;
  }
}
