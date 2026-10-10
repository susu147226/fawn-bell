/** 面包屑（§8.1 中栏顶部：素材根 → … → 当前文件夹）。 */

import { ChevronRight } from 'lucide-react';

export interface BreadcrumbProps {
  /** 素材根显示名（磁盘目录名）。 */
  rootName: string;
  /** `breadcrumbOf()` 的产物；首项 rel 为空串代表素材根。 */
  crumbs: { label: string; rel: string }[];
  current: string;
  onNavigate: (rel: string) => void;
}

export default function Breadcrumb({ rootName, crumbs, current, onNavigate }: BreadcrumbProps) {
  return (
    <div className="crumb">
      {crumbs.map((c, i) => {
        const isLast = i === crumbs.length - 1;
        return (
          <span key={c.rel === '' ? '\u0000root' : c.rel} className="stat">
            {i > 0 ? <ChevronRight className="crumb-sep" size={14} strokeWidth={1.75} aria-hidden /> : null}
            <button
              className={isLast ? 'crumb-item current' : 'crumb-item'}
              type="button"
              onClick={() => onNavigate(c.rel)}
              disabled={c.rel === current}
             
            >
              {c.label === '' ? rootName : c.label}
            </button>
          </span>
        );
      })}
    </div>
  );
}
