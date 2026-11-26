import { Request, Response, NextFunction } from 'express';
import jwt from 'jsonwebtoken';
import { config } from '../../common/config.js';
import { UnauthorizedError, ForbiddenError } from '../../common/errors.js';

export interface AuthenticatedUser {
  id: string;
  email: string;
  role: string;
  tier: string;
}

declare global {
  // eslint-disable-next-line @typescript-eslint/no-namespace
  namespace Express {
    interface Request {
      user?: AuthenticatedUser;
    }
  }
}

export function authenticate(req: Request, _res: Response, next: NextFunction): void {
  const authHeader = req.headers.authorization;
  if (!authHeader || !authHeader.startsWith('Bearer ')) {
    throw new UnauthorizedError('Missing or malformed Authorization header');
  }

  const token = authHeader.split(' ')[1];
  try {
    // HIGH-02: Pin HS256 algorithm and reject refresh/non-access token types
    const payload = jwt.verify(token, config.JWT_SECRET, {
      algorithms: ['HS256'],
    }) as AuthenticatedUser & { typ?: string };
    if (
      (payload.typ && payload.typ !== 'access') ||
      typeof payload.id !== 'string' ||
      typeof payload.role !== 'string'
    ) {
      throw new UnauthorizedError('Invalid authentication token type');
    }
    req.user = payload;
    next();
  } catch {
    throw new UnauthorizedError('Invalid or expired authentication token');
  }
}

export function requireRole(...allowedRoles: string[]) {
  return (req: Request, _res: Response, next: NextFunction): void => {
    if (!req.user) {
      throw new UnauthorizedError('Authentication required');
    }
    if (!allowedRoles.includes(req.user.role)) {
      throw new ForbiddenError('Insufficient permissions');
    }
    next();
  };
}

export const requireAdmin = requireRole('ADMIN');
