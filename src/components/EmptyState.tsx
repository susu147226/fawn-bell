/** 空 / 异常状态（§8.2：每个空列表都要有插图 + 一句说明 + 一个主操作）。 */

import type { ReactNode } from 'react';

export interface EmptyStateProps {
  /** 插图（用线性图标即可，保持单色）。 */
  art?: ReactNode;
  title: string;
  desc?: ReactNode;
  actions?: ReactNode;
}

export default function EmptyState({ art, title, desc, actions }: EmptyStateProps) {
  return (
    <div className="empty">
      {art ? <div className="empty-art">{art}</div> : null}
      <div className="empty-title">{title}</div>
      {desc ? <div className="empty-desc">{desc}</div> : null}
      {actions ? <div className="empty-actions">{actions}</div> : null}
    </div>
  );
}
