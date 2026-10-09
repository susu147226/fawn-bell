/**
 * 类型图标（§8.2：类型列用单色线性图标 + 文字，语义色不得复用为装饰色）。
 *
 * P0 只用 lucide-react 的基础线性图标，全部继承 `currentColor`；
 * 图标名已按 lucide-react 1.53.0 的导出面核对过（没有 FileVideo / FileAudio）。
 */

import { Box, File, FileImage, FileText, Film, Music, type LucideIcon } from 'lucide-react';

import type { Kind } from './types';

export const KIND_ICON: Record<Kind, LucideIcon> = {
  image: FileImage,
  video: Film,
  audio: Music,
  '3d': Box,
  doc: FileText,
  other: File,
};

export function KindIcon({ kind, size = 14 }: { kind: Kind; size?: number }) {
  const Icon = KIND_ICON[kind];
  return <Icon className="kind-icon" size={size} strokeWidth={1.75} aria-hidden />;
}
