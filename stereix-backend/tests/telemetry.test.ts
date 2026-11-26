const mockDb = {
  telemetryReport: {
    create: jest.fn(),
    count: jest.fn(),
    aggregate: jest.fn(),
    groupBy: jest.fn(),
    findMany: jest.fn(),
  },
};

const mockLogger = {
  info: jest.fn(),
  warn: jest.fn(),
  error: jest.fn(),
};

jest.mock('../src/common/db.js', () => ({
  db: mockDb,
}));

jest.mock('../src/common/logger.js', () => ({
  logger: mockLogger,
}));

import { TelemetryService } from '../src/services/telemetry/telemetry.service.js';
import { enforceTelemetryBodyLimit, MAX_TELEMETRY_BODY_BYTES } from '../src/services/telemetry/telemetry.routes.js';
import type { Request, Response } from 'express';

describe('TelemetryService', () => {
  let telemetryService: TelemetryService;

  beforeEach(() => {
    jest.clearAllMocks();
    telemetryService = new TelemetryService();
  });

  describe('recordReport', () => {
    it('records a healthy runtime telemetry report and does not log crash warnings', async () => {
      mockDb.telemetryReport.create.mockResolvedValue({
        id: 'rep_101',
        clientVersion: '0.1.97',
        gpuName: 'NVIDIA GeForce RTX 4090',
        gameTitle: 'Sekiro: Shadows Die Twice',
        targetApi: 'DX11',
        frameRate: 90.0,
        frameTimeMs: 11.1,
        isCrash: false,
      });

      const report = await telemetryService.recordReport({
        clientVersion: '0.1.97',
        gpuName: 'NVIDIA GeForce RTX 4090',
        gameTitle: 'Sekiro: Shadows Die Twice',
        targetApi: 'DX11',
        frameRate: 90.0,
        frameTimeMs: 11.1,
        isCrash: false,
      });

      expect(mockDb.telemetryReport.create).toHaveBeenCalled();
      expect(mockLogger.warn).not.toHaveBeenCalled();
      expect(report.id).toBe('rep_101');
    });

    it('records a crash report, flags isCrash=true, and emits a structured warning to logger', async () => {
      mockDb.telemetryReport.create.mockResolvedValue({
        id: 'rep_102',
        clientVersion: '0.1.97',
        gpuName: 'AMD Radeon RX 7900 XTX',
        gameTitle: 'Cyberpunk 2077',
        targetApi: 'DX12',
        isCrash: true,
        errorDetails: 'DeviceRemovedException',
        stackTrace: 'at dx12_renderer.cpp:342',
      });

      const report = await telemetryService.recordReport({
        clientVersion: '0.1.97',
        gpuName: 'AMD Radeon RX 7900 XTX',
        gameTitle: 'Cyberpunk 2077',
        targetApi: 'DX12',
        isCrash: true,
        errorDetails: 'DeviceRemovedException',
        stackTrace: 'at dx12_renderer.cpp:342',
      });

      expect(mockLogger.warn).toHaveBeenCalledWith(
        expect.objectContaining({
          version: '0.1.97',
          gpu: 'AMD Radeon RX 7900 XTX',
          game: 'Cyberpunk 2077',
          err: 'DeviceRemovedException',
        }),
        'Crash report received from client'
      );
      expect(report.isCrash).toBe(true);
    });
  });

  describe('getMetricsSummary', () => {
    it('aggregates total reports, crash counts, and average frame performance', async () => {
      mockDb.telemetryReport.count
        .mockResolvedValueOnce(1250) // total
        .mockResolvedValueOnce(5);   // crashes

      mockDb.telemetryReport.aggregate.mockResolvedValue({
        _avg: {
          frameRate: 89.8,
          frameTimeMs: 11.13,
        },
      });

      mockDb.telemetryReport.findMany.mockResolvedValue([
        { id: 'rep_c1', isCrash: true, timestamp: new Date() },
      ]);

      const summary = await telemetryService.getMetricsSummary();

      expect(summary.totalReports).toBe(1250);
      expect(summary.totalCrashes).toBe(5);
      expect(summary.crashRate).toBeCloseTo(0.4, 1);
      expect(summary.averageFrameRate).toBe(89.8);
      expect(summary.averageFrameTimeMs).toBe(11.13);
      expect(summary.recentCrashes).toHaveLength(1);
    });
  });

  describe('enforceTelemetryBodyLimit (HIGH-03)', () => {
    it('rejects >64KB pre-parsed JSON body with HTTP 413 even when global express.json ran first', () => {
      const req = {
        headers: {},
        body: { padding: 'x'.repeat(MAX_TELEMETRY_BODY_BYTES + 1024) },
      } as unknown as Request;
      const res = {
        status: jest.fn().mockReturnThis(),
        json: jest.fn(),
      } as unknown as Response;
      const next = jest.fn();

      enforceTelemetryBodyLimit(req, res, next);
      expect(res.status).toHaveBeenCalledWith(413);
      expect(next).not.toHaveBeenCalled();
    });

    it('rejects oversized Content-Length header with HTTP 413', () => {
      const req = {
        headers: { 'content-length': String(MAX_TELEMETRY_BODY_BYTES + 1) },
        body: {},
      } as unknown as Request;
      const res = {
        status: jest.fn().mockReturnThis(),
        json: jest.fn(),
      } as unknown as Response;
      const next = jest.fn();

      enforceTelemetryBodyLimit(req, res, next);
      expect(res.status).toHaveBeenCalledWith(413);
      expect(next).not.toHaveBeenCalled();
    });

    it('allows valid telemetry body under 64KB', () => {
      const req = {
        headers: { 'content-length': '256' },
        body: { clientVersion: '0.1.122', gpuName: 'RTX 4090' },
      } as unknown as Request;
      const res = {
        status: jest.fn().mockReturnThis(),
        json: jest.fn(),
      } as unknown as Response;
      const next = jest.fn();

      enforceTelemetryBodyLimit(req, res, next);
      expect(next).toHaveBeenCalledTimes(1);
      expect(res.status).not.toHaveBeenCalled();
    });
  });
});
