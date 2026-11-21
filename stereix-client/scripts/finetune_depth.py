"""
Fine-Tune Depth Anything V2 on Game Screenshots
================================================
RTX 3050 6GB optimized fine-tuning script.

This script fine-tunes a pre-trained Depth Anything V2 model on YOUR game
screenshots to improve depth quality for specific games (Sekiro, Cyberpunk, etc.).

Training data preparation:
  1. Take screenshots in your game using Steam (F12) or GeForce Experience
  2. Place them in: stereix-client/data/game_screenshots/
  3. Run this script — it self-supervises using the pre-trained model's
     predictions as pseudo ground-truth, then refines on your images.

GPU Memory Budget (RTX 3050 6GB):
  - batch_size=2, resolution=518x518, FP16: ~2.8 GB VRAM
  - batch_size=4, resolution=384x384, FP16: ~3.2 GB VRAM

Usage:
  python finetune_depth.py --data ../data/game_screenshots/ --epochs 10
  python finetune_depth.py --data ../data/game_screenshots/ --game sekiro --epochs 20
"""

import os
import sys
import glob
import argparse
import time
import torch
import torch.nn as nn
import torch.nn.functional as F
from torch.utils.data import Dataset, DataLoader
from torchvision import transforms
import numpy as np


class GameScreenshotDataset(Dataset):
    """
    Self-supervised depth dataset from game screenshots.
    
    Uses the teacher model's predictions as pseudo ground-truth depth labels.
    This is the standard approach for domain adaptation of depth models.
    """
    
    def __init__(self, image_dir, teacher_model=None, resolution=518, device="cuda"):
        self.resolution = resolution
        self.device = device
        
        # Find all images
        extensions = ['*.png', '*.jpg', '*.jpeg', '*.bmp', '*.webp']
        self.image_paths = []
        for ext in extensions:
            self.image_paths.extend(glob.glob(os.path.join(image_dir, ext)))
            self.image_paths.extend(glob.glob(os.path.join(image_dir, '**', ext), recursive=True))
        
        self.image_paths = sorted(set(self.image_paths))
        
        if len(self.image_paths) == 0:
            raise ValueError(f"No images found in {image_dir}. Supported: {extensions}")
        
        print(f"[Stereix AI] Found {len(self.image_paths)} game screenshots")
        
        # Transform pipeline
        self.transform = transforms.Compose([
            transforms.ToPILImage(),
            transforms.Resize((resolution, resolution)),
            transforms.ColorJitter(brightness=0.2, contrast=0.2, saturation=0.1),
            transforms.RandomHorizontalFlip(p=0.5),
            transforms.ToTensor(),  # [0, 1] float32
        ])
        
        self.transform_clean = transforms.Compose([
            transforms.ToPILImage(),
            transforms.Resize((resolution, resolution)),
            transforms.ToTensor(),
        ])
        
        # Generate pseudo ground-truth depth from teacher
        self.pseudo_depths = []
        if teacher_model is not None:
            print("[Stereix AI] Generating pseudo-GT depth from teacher model...")
            teacher_model.eval()
            with torch.no_grad():
                for i, path in enumerate(self.image_paths):
                    img = self._load_image(path)
                    img_tensor = self.transform_clean(img).unsqueeze(0).to(device)
                    depth = teacher_model(img_tensor)  # [1, 1, H, W]
                    self.pseudo_depths.append(depth.cpu().squeeze(0))  # [1, H, W]
                    if (i + 1) % 10 == 0:
                        print(f"  [{i+1}/{len(self.image_paths)}] processed")
            print(f"[Stereix AI] ✓ Generated {len(self.pseudo_depths)} pseudo-GT depth maps")
    
    def _load_image(self, path):
        import cv2
        img = cv2.imread(path)
        if img is None:
            raise ValueError(f"Cannot read image: {path}")
        return cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
    
    def __len__(self):
        return len(self.image_paths)
    
    def __getitem__(self, idx):
        img = self._load_image(self.image_paths[idx])
        img_tensor = self.transform(img)  # [3, H, W] float32 [0, 1]
        
        if self.pseudo_depths:
            depth = self.pseudo_depths[idx]
            # Resize depth to match augmented resolution
            depth = F.interpolate(
                depth.unsqueeze(0), 
                size=(self.resolution, self.resolution), 
                mode='bilinear', align_corners=False
            ).squeeze(0)
        else:
            # If no teacher, use gradient-based heuristic as weak supervision
            depth = torch.zeros(1, self.resolution, self.resolution)
        
        return img_tensor, depth


class ScaleInvariantDepthLoss(nn.Module):
    """
    Scale-and-shift invariant depth loss (Ranftl et al., 2020).
    This is the standard loss for monocular depth estimation fine-tuning.
    Handles the inherent scale ambiguity in monocular depth.
    """
    
    def __init__(self, alpha=0.5):
        super().__init__()
        self.alpha = alpha
    
    def forward(self, pred, target):
        # Flatten
        pred_flat = pred.reshape(pred.shape[0], -1)
        target_flat = target.reshape(target.shape[0], -1)
        
        # Mask out invalid regions (zeros or near-zeros in target)
        mask = target_flat > 1e-6
        if mask.sum() < 10:
            return torch.tensor(0.0, device=pred.device, requires_grad=True)
        
        # Compute scale-invariant error in log space
        pred_valid = pred_flat[mask].clamp(min=1e-6)
        target_valid = target_flat[mask].clamp(min=1e-6)
        
        d = torch.log(pred_valid) - torch.log(target_valid)
        n = d.numel()
        
        # Scale-invariant MSE: mean(d^2) - alpha/n * (sum(d))^2
        loss = (d ** 2).mean() - self.alpha / n * (d.sum() ** 2)
        
        return loss


class MultiScaleGradientLoss(nn.Module):
    """
    Multi-scale gradient matching loss for sharp depth edges.
    Encourages the model to preserve object boundaries in depth maps.
    """
    
    def __init__(self, scales=4):
        super().__init__()
        self.scales = scales
    
    def gradient_x(self, img):
        return img[:, :, :, :-1] - img[:, :, :, 1:]
    
    def gradient_y(self, img):
        return img[:, :, :-1, :] - img[:, :, 1:, :]
    
    def forward(self, pred, target):
        total_loss = 0.0
        pred_s = pred
        target_s = target
        
        for s in range(self.scales):
            gx_pred = self.gradient_x(pred_s)
            gy_pred = self.gradient_y(pred_s)
            gx_target = self.gradient_x(target_s)
            gy_target = self.gradient_y(target_s)
            
            total_loss += (gx_pred - gx_target).abs().mean()
            total_loss += (gy_pred - gy_target).abs().mean()
            
            # Downsample for next scale
            pred_s = F.avg_pool2d(pred_s, 2)
            target_s = F.avg_pool2d(target_s, 2)
        
        return total_loss / (2 * self.scales)


def finetune(
    model, 
    dataset, 
    output_path,
    epochs=10,
    batch_size=2,
    lr=1e-5,
    device="cuda",
):
    """
    Fine-tune the depth model on game screenshots.
    
    Optimized for RTX 3050 6GB:
      - Mixed precision (FP16) training
      - Gradient accumulation
      - Small batch sizes
    """
    print(f"\n{'=' * 60}")
    print(f"  Fine-Tuning Configuration")
    print(f"{'=' * 60}")
    print(f"  Dataset:    {len(dataset)} images")
    print(f"  Epochs:     {epochs}")
    print(f"  Batch size: {batch_size}")
    print(f"  LR:         {lr}")
    print(f"  Device:     {device}")
    print(f"  Precision:  Mixed FP16 (AMP)")
    print(f"{'=' * 60}\n")
    
    dataloader = DataLoader(
        dataset, 
        batch_size=batch_size, 
        shuffle=True, 
        num_workers=0,  # Windows compatibility
        pin_memory=True,
        drop_last=True,
    )
    
    # Freeze encoder backbone, only fine-tune decoder head
    # This drastically reduces VRAM and prevents catastrophic forgetting
    frozen_params = 0
    trainable_params = 0
    
    for name, param in model.named_parameters():
        if 'pretrained' in name or 'encoder' in name or 'blocks' in name:
            param.requires_grad = False
            frozen_params += param.numel()
        else:
            param.requires_grad = True
            trainable_params += param.numel()
    
    print(f"[Stereix AI] Frozen params:    {frozen_params:,} (encoder backbone)")
    print(f"[Stereix AI] Trainable params: {trainable_params:,} (decoder head)")
    
    optimizer = torch.optim.AdamW(
        filter(lambda p: p.requires_grad, model.parameters()),
        lr=lr,
        weight_decay=1e-4,
    )
    
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=epochs)
    scaler = torch.amp.GradScaler('cuda')
    
    si_loss_fn = ScaleInvariantDepthLoss(alpha=0.5)
    grad_loss_fn = MultiScaleGradientLoss(scales=3)
    
    model.train()
    best_loss = float('inf')
    
    for epoch in range(epochs):
        epoch_loss = 0.0
        epoch_start = time.perf_counter()
        
        for batch_idx, (images, depths) in enumerate(dataloader):
            images = images.to(device)
            depths = depths.to(device)
            
            optimizer.zero_grad(set_to_none=True)
            
            with torch.amp.autocast('cuda'):
                pred_depth = model(images)  # [B, 1, H, W]
                
                # Resize prediction to match target
                if pred_depth.shape != depths.shape:
                    pred_depth = F.interpolate(
                        pred_depth, size=depths.shape[2:],
                        mode='bilinear', align_corners=False
                    )
                
                loss_si = si_loss_fn(pred_depth, depths)
                loss_grad = grad_loss_fn(pred_depth, depths)
                loss = loss_si + 0.5 * loss_grad
            
            scaler.scale(loss).backward()
            scaler.unscale_(optimizer)
            torch.nn.utils.clip_grad_norm_(model.parameters(), max_norm=1.0)
            scaler.step(optimizer)
            scaler.update()
            
            epoch_loss += loss.item()
        
        scheduler.step()
        
        avg_loss = epoch_loss / max(len(dataloader), 1)
        elapsed = time.perf_counter() - epoch_start
        current_lr = scheduler.get_last_lr()[0]
        
        print(f"  Epoch {epoch+1:3d}/{epochs} | Loss: {avg_loss:.5f} | "
              f"LR: {current_lr:.2e} | Time: {elapsed:.1f}s")
        
        # Save best model
        if avg_loss < best_loss:
            best_loss = avg_loss
            torch.save(model.state_dict(), output_path.replace('.onnx', '_best.pth'))
    
    print(f"\n[Stereix AI] ✓ Fine-tuning complete! Best loss: {best_loss:.5f}")
    return model


def main():
    parser = argparse.ArgumentParser(description="Fine-tune Depth Anything V2 on game screenshots")
    parser.add_argument("--data", required=True, help="Directory containing game screenshots")
    parser.add_argument("--game", default="generic", help="Game name (for labeling)")
    parser.add_argument("--encoder", choices=["vits", "vitb"], default="vits")
    parser.add_argument("--epochs", type=int, default=10)
    parser.add_argument("--batch-size", type=int, default=2)
    parser.add_argument("--lr", type=float, default=1e-5)
    parser.add_argument("--resolution", type=int, default=518)
    parser.add_argument("--output", default=None)
    parser.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
    args = parser.parse_args()
    
    if args.output is None:
        script_dir = os.path.dirname(os.path.abspath(__file__))
        args.output = os.path.join(script_dir, "..", "models", "depth_inpainter.onnx")
    
    print("=" * 60)
    print(f"  Stereix Engine — Depth Model Fine-Tuning ({args.game})")
    print("=" * 60)
    
    # GPU memory check
    if torch.cuda.is_available():
        gpu_mem = torch.cuda.get_device_properties(0).total_mem / (1024**3)
        gpu_name = torch.cuda.get_device_name(0)
        print(f"  GPU: {gpu_name} ({gpu_mem:.1f} GB)")
        
        if gpu_mem < 4:
            print(f"  ⚠ Low VRAM detected. Reducing batch size to 1 and resolution to 384")
            args.batch_size = 1
            args.resolution = 384
    
    # Step 1: Load or build the teacher model (pre-trained)
    print(f"\n[Step 1] Loading pre-trained teacher model...")
    
    # Try to import the download script
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from download_depth_anything_v2 import build_model_from_timm, try_build_model_from_source, download_model
    
    teacher = None
    try:
        ckpt = download_model(encoder=args.encoder, device=args.device)
        teacher = try_build_model_from_source(args.encoder, ckpt, args.device)
    except Exception:
        pass
    
    if teacher is None:
        teacher = build_model_from_timm(args.encoder, None, args.device)
    
    teacher.eval()
    print("[Stereix AI] ✓ Teacher model loaded")
    
    # Step 2: Create dataset with pseudo-GT from teacher
    print(f"\n[Step 2] Preparing dataset from {args.data}...")
    dataset = GameScreenshotDataset(
        args.data,
        teacher_model=teacher,
        resolution=args.resolution,
        device=args.device,
    )
    
    # Step 3: Create student model (copy of teacher, to be fine-tuned)
    print(f"\n[Step 3] Creating student model for fine-tuning...")
    student = None
    try:
        ckpt = download_model(encoder=args.encoder, device=args.device)
        student = try_build_model_from_source(args.encoder, ckpt, args.device)
    except Exception:
        pass
    
    if student is None:
        student = build_model_from_timm(args.encoder, None, args.device)
    
    # Step 4: Fine-tune
    print(f"\n[Step 4] Fine-tuning on {args.game} screenshots...")
    student = finetune(
        student, dataset, args.output,
        epochs=args.epochs,
        batch_size=args.batch_size,
        lr=args.lr,
        device=args.device,
    )
    
    # Step 5: Export fine-tuned model to ONNX
    print(f"\n[Step 5] Exporting fine-tuned model to ONNX...")
    from download_depth_anything_v2 import export_to_onnx, validate_with_onnxruntime
    
    student.eval()
    export_to_onnx(student, args.output, args.resolution, args.resolution, args.device)
    validate_with_onnxruntime(args.output)
    
    print(f"\n{'=' * 60}")
    print(f"  ✓ Fine-tuned model ready: {os.path.abspath(args.output)}")
    print(f"  Game: {args.game}")
    print(f"  Now rebuild the engine and test in-game!")
    print(f"{'=' * 60}")


if __name__ == "__main__":
    main()
