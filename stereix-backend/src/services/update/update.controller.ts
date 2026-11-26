import { Request, Response, NextFunction } from 'express';
import { z } from 'zod';
import { UpdateService } from './update.service.js';
import { ReleaseChannel } from '@prisma/client';

const updateService = new UpdateService();

const safeReleaseKeySchema = z
  .string()
  .regex(/^releases\/[a-zA-Z0-9._/-]{1,256}$/, 'Key must start with releases/ and use safe path characters')
  .refine((k) => !k.includes('..') && !k.includes('//') && !k.includes('\\'), 'Path traversal is not allowed');

const publishReleaseSchema = z.object({
  version: z.string().regex(/^\d+\.\d+\.\d+(-[a-zA-Z0-9.]+)?$/),
  channel: z.nativeEnum(ReleaseChannel).default(ReleaseChannel.STABLE),
  releaseNotes: z.string().max(4000).optional(),
  isMandatory: z.boolean().default(false),
  installerKey: safeReleaseKeySchema,
  cliKey: safeReleaseKeySchema.optional(),
  dllKey: safeReleaseKeySchema.optional(),
  sha256Hash: z.string().regex(/^[0-9a-fA-F]{64}$/),
});

export class UpdateController {
  async check(req: Request, res: Response, next: NextFunction) {
    try {
      const version = (req.query.version as string) || '0.0.0';
      const channelParam = (req.query.channel as string)?.toUpperCase();
      const channel = Object.values(ReleaseChannel).includes(channelParam as ReleaseChannel)
        ? (channelParam as ReleaseChannel)
        : ReleaseChannel.STABLE;

      const result = await updateService.checkForUpdates(version, channel);
      res.status(200).json({ status: 'success', data: result });
    } catch (err) {
      next(err);
    }
  }

  async publish(req: Request, res: Response, next: NextFunction) {
    try {
      const data = publishReleaseSchema.parse(req.body);
      const release = await updateService.publishRelease(data);
      res.status(201).json({ status: 'success', data: release });
    } catch (err) {
      next(err);
    }
  }

  async getUploadPresigned(req: Request, res: Response, next: NextFunction) {
    try {
      const filename = req.query.filename as string;
      const contentType = (req.query.contentType as string) || 'application/octet-stream';
      if (!filename) {
        res.status(400).json({ status: 'error', message: 'Query parameter "filename" is required' });
        return;
      }
      const result = await updateService.getUploadUrl(filename, contentType);
      res.status(200).json({ status: 'success', data: result });
    } catch (err) {
      next(err);
    }
  }
}
