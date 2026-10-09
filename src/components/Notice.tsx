/** 提示条（§10 云占位 / 超限 / 扫描告警等一次性说明）。 */

import { CircleAlert, Info, TriangleAlert } from 'lucide-react';
import type { ReactNode } from 'react';

export type NoticeKind = 'info' | 'warn' | 'error';

export interface NoticeProps {
  kind?: NoticeKind;
  title: string;
  /** 逐条说明（可空）。 */
  lines?: string[];
  children?: ReactNode;
}

const ICON = { info: Info, warn: TriangleAlert, error: CircleAlert };

export default function Notice({ kind = 'info', title, lines, children }: NoticeProps) {
  const Icon = ICON[kind];
  const cls = kind === 'info' ? 'notice' : `notice ${kind}`;
  return (
    <div className={cls}>
      <Icon size={16} strokeWidth={1.75} aria-hidden />
      <div className="notice-body">
        <div className="notice-title">{title}</div>
        {lines && lines.length > 0 ? (
          <ul className="notice-list">
            {lines.map((l) => (
              <li key={l}>{l}</li>
            ))}
          </ul>
        ) : null}
        {children}
      </div>
    </div>
  );
}
