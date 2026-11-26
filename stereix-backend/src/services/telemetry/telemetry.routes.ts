import express, { Router, Request, Response, NextFunction } from 'express';
import rateLimit from 'express-rate-limit';
import { TelemetryController } from './telemetry.controller.js';
import { authenticate, requireRole } from '../auth/auth.middleware.js';

const router = Router();
const controller = new TelemetryController();

export const MAX_TELEMETRY_BODY_BYTES = 64 * 1024;

// HIGH-03: Dedicated tight rate limit and explicit 64kb payload cap on telemetry ingestion.
// NOTE: Enforced explicitly because gateway/index.ts mounts express.json({ limit: '100kb' }) globally
// before route routers (which causes a second express.json({ limit: '64kb' }) to skip parsing).
export function enforceTelemetryBodyLimit(req: Request, res: Response, next: NextFunction): void {
  const contentLength = Number(req.headers['content-length']);
  if (Number.isFinite(contentLength) && contentLength > MAX_TELEMETRY_BODY_BYTES) {
    res.status(413).json({ status: 'error', message: 'Telemetry payload exceeds 64KB limit.' });
    return;
  }
  if (req.body !== undefined) {
    try {
      const serializedBytes = Buffer.byteLength(JSON.stringify(req.body), 'utf8');
      if (serializedBytes > MAX_TELEMETRY_BODY_BYTES) {
        res.status(413).json({ status: 'error', message: 'Telemetry payload exceeds 64KB limit.' });
        return;
      }
    } catch {
      res.status(400).json({ status: 'error', message: 'Malformed telemetry payload.' });
      return;
    }
  }
  next();
}

const telemetryIngestLimiter = rateLimit({
  windowMs: 60_000,
  max: 15,
  standardHeaders: true,
  legacyHeaders: false,
  message: { status: 'error', message: 'Too many telemetry reports. Please try again later.' },
});

// Anonymous or authenticated telemetry ingestion (rate-limited + bounded body)
router.post(
  '/ingest',
  telemetryIngestLimiter,
  express.json({ limit: '64kb' }),
  enforceTelemetryBodyLimit,
  controller.ingest
);

// Metrics dashboard queries (Admin / Developer only)
router.get('/metrics', authenticate, requireRole('DEVELOPER', 'ADMIN'), controller.getMetrics);

export const telemetryRouter = router;
