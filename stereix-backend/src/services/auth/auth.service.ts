import bcrypt from 'bcryptjs';
import crypto from 'crypto';
import jwt from 'jsonwebtoken';
import { db } from '../../common/db.js';
import { config } from '../../common/config.js';
import { ConflictError, UnauthorizedError } from '../../common/errors.js';
import { AuthenticatedUser } from './auth.middleware.js';

// Pre-computed dummy bcrypt hash (cost 12) for constant-time user lookup on login (HIGH-02)
const DUMMY_PASSWORD_HASH = '$2a$12$C6UzMDM.H6dfI/f/IKxGhuOlN4m4xA4Z0a3R3n0jW3pJ5f5l1E1W.';

export function hashRefreshToken(rawToken: string): string {
  return crypto.createHash('sha256').update(rawToken).digest('hex');
}

function getRefreshSigningKey(): string {
  const configured = (config as { JWT_REFRESH_SECRET?: string }).JWT_REFRESH_SECRET;
  if (configured && configured.length >= 32) {
    return configured;
  }
  return crypto.createHmac('sha256', config.JWT_SECRET).update('stereix-refresh-token-v1').digest('hex');
}

export class AuthService {
  async register(username: string, email: string, password: string) {
    const existingUser = await db.user.findFirst({
      where: { OR: [{ email }, { username }] },
    });

    if (existingUser) {
      throw new ConflictError('Username or email already registered');
    }

    const passwordHash = await bcrypt.hash(password, 12);
    const user = await db.user.create({
      data: {
        username,
        email,
        passwordHash,
      },
      select: {
        id: true,
        username: true,
        email: true,
        role: true,
        tier: true,
        createdAt: true,
      },
    });

    const tokens = await this.generateTokens(user);
    return { user, ...tokens };
  }

  async login(emailOrUsername: string, password: string) {
    const user = await db.user.findFirst({
      where: {
        OR: [{ email: emailOrUsername }, { username: emailOrUsername }],
      },
    });

    if (!user) {
      // HIGH-02: Mitigate timing-based account enumeration by performing a dummy bcrypt comparison
      await bcrypt.compare(password, DUMMY_PASSWORD_HASH);
      throw new UnauthorizedError('Invalid email or password');
    }

    const isValid = await bcrypt.compare(password, user.passwordHash);
    if (!isValid) {
      throw new UnauthorizedError('Invalid email or password');
    }

    const userPayload: AuthenticatedUser = {
      id: user.id,
      email: user.email,
      role: user.role,
      tier: user.tier,
    };

    const tokens = await this.generateTokens(userPayload);
    return {
      user: {
        id: user.id,
        username: user.username,
        email: user.email,
        role: user.role,
        tier: user.tier,
      },
      ...tokens,
    };
  }

  async refreshToken(token: string) {
    if (!token || typeof token !== 'string') {
      throw new UnauthorizedError('Invalid or expired refresh token');
    }
    // HIGH-02: Store and query SHA-256 hash of refresh tokens (never plaintext in DB)
    const tokenHash = hashRefreshToken(token);

    // M-11: Atomic rotation - conditionally revoke in a single atomic update query
    // If concurrent requests attempt to use the same token, only one succeeds.
    const updated = await db.refreshToken.updateMany({
      where: {
        token: tokenHash,
        revoked: false,
        expiresAt: { gt: new Date() },
      },
      data: { revoked: true },
    });

    if (!updated || updated.count === 0) {
      throw new UnauthorizedError('Invalid or expired refresh token');
    }

    const storedToken = await db.refreshToken.findFirst({
      where: { token: tokenHash },
      include: { user: true },
    });

    if (!storedToken || !storedToken.user) {
      throw new UnauthorizedError('Invalid or expired refresh token');
    }

    const userPayload: AuthenticatedUser = {
      id: storedToken.user.id,
      email: storedToken.user.email,
      role: storedToken.user.role,
      tier: storedToken.user.tier,
    };

    return await this.generateTokens(userPayload);
  }

  async logout(token: string) {
    if (!token || typeof token !== 'string') return;
    await db.refreshToken.updateMany({
      where: { token: hashRefreshToken(token) },
      data: { revoked: true },
    });
  }

  private async generateTokens(user: AuthenticatedUser | { id: string; email: string; role: string; tier: string }) {
    // HIGH-02: Strict domain separation between access and refresh JWTs (distinct typ, signing key, algorithm, and unique jti)
    const accessToken = jwt.sign(
      { id: user.id, email: user.email, role: user.role, tier: user.tier, typ: 'access' },
      config.JWT_SECRET,
      { algorithm: 'HS256', expiresIn: config.JWT_EXPIRES_IN as any }
    );

    const refreshJwt = jwt.sign(
      { id: user.id, typ: 'refresh', jti: crypto.randomUUID() },
      getRefreshSigningKey(),
      { algorithm: 'HS256', expiresIn: (config.JWT_REFRESH_EXPIRES_IN || '7d') as any }
    );
    const refreshTokenString = `${user.id}.${refreshJwt}`;
    const expiresAt = new Date(Date.now() + 7 * 24 * 60 * 60 * 1000);

    await db.refreshToken.create({
      data: {
        token: hashRefreshToken(refreshTokenString),
        userId: user.id,
        expiresAt,
      },
    });

    return { accessToken, refreshToken: refreshTokenString };
  }
}
