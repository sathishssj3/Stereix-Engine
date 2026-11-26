import { Request, Response, NextFunction } from 'express';
import { z } from 'zod';
import { TelemetryService } from './telemetry.service.js';

const telemetryService = new TelemetryService();

const telemetrySchema = z.object({
  clientVersion: z.string().min(1).max(32),
  gpuName: z.string().min(1).max(128),
  driverVersion: z.string().max(64).optional(),
  gameTitle: z.string().max(150).optional(),
  targetApi: z.string().max(32).optional(),
  frameRate: z.number().finite().min(0).max(10000).optional(),
  frameTimeMs: z.number().finite().min(0).max(60000).optional(),
  isCrash: z.boolean().default(false),
  errorDetails: z.string().max(2000).optional(),
  stackTrace: z.string().max(8000).optional(),
});

export class TelemetryController {
  async ingest(req: Request, res: Response, next: NextFunction) {
    try {
      const data = telemetrySchema.parse(req.body);
      const report = await telemetryService.recordReport({
        ...data,
        userId: req.user?.id,
      });
      res.status(201).json({ status: 'success', data: { id: report.id } });
    } catch (err) {
      next(err);
    }
  }

  async getMetrics(_req: Request, res: Response, next: NextFunction) {
    try {
      const summary = await telemetryService.getMetricsSummary();
      res.status(200).json({ status: 'success', data: summary });
    } catch (err) {
      next(err);
    }
  }
}
