import { db } from '../../common/db.js';
import { getCached, setCached, invalidateCache } from '../../common/redis.js';
import { NotFoundError } from '../../common/errors.js';

export class GameService {
  async listGames() {
    const cacheKey = 'games:all';
    const cached = await getCached(cacheKey);
    if (cached) return cached;

    const games = await db.game.findMany({
      include: {
        profiles: {
          where: { isOfficial: true },
          take: 1,
        },
      },
      orderBy: { title: 'asc' },
    });

    await setCached(cacheKey, games, 600);
    return games;
  }

  async getGameBySlug(slug: string) {
    const cacheKey = `game:slug:${slug}`;
    const cached = await getCached(cacheKey);
    if (cached) return cached;

    const game = await db.game.findUnique({
      where: { slug },
      include: {
        profiles: {
          orderBy: { downloads: 'desc' },
        },
      },
    });

    if (!game) {
      throw new NotFoundError(`Game '${slug}' not found`);
    }

    await setCached(cacheKey, game, 600);
    return game;
  }

  async detectGameByExecutable(exeName: string) {
    const trimmedExe = String(exeName || '').trim();
    if (!trimmedExe || trimmedExe.length > 260) {
      return null;
    }
    const normalizedExe = trimmedExe.toLowerCase();
    const cacheKey = `game:exe:${normalizedExe}`;
    const cached = await getCached(cacheKey);
    if (cached) return cached;

    // HIGH-03: Avoid full-table db.game.findMany() scan on every cache miss; query directly via array membership
    const candidates = Array.from(new Set([normalizedExe, trimmedExe]));
    const match = await db.game.findFirst({
      where: {
        exeNames: {
          hasSome: candidates,
        },
      },
    });

    if (!match) {
      return null;
    }

    await setCached(cacheKey, match, 1800);
    return match;
  }

  async upsertGame(data: {
    title: string;
    slug: string;
    exeNames: string[];
    graphicsApis: string[];
    antiCheat?: string;
    supported?: boolean;
  }) {
    const normalizedData = {
      ...data,
      exeNames: data.exeNames.map((e) => e.trim().toLowerCase()).filter(Boolean),
    };
    const game = await db.game.upsert({
      where: { slug: data.slug },
      update: normalizedData,
      create: normalizedData,
    });

    await invalidateCache('games:*');
    await invalidateCache(`game:slug:${data.slug}`);
    // HIGH-03: Invalidate executable lookup cache whenever game metadata changes
    await invalidateCache('game:exe:*');
    return game;
  }
}
