/**
 * 关闭软件时的三选项提醒（执行版 §7.2）。
 *
 * 条文：变更集非空时关闭，三个选项**必须并列且写清后果，不设默认焦点，不响应回车**。
 * 因此这里刻意做三件事：不自动 focus 任何按钮、不绑定 Enter、每个选项下面写清「会发生什么」。
 *
 * 边界说明：`提交并关闭` 需要提交执行（生成计划 → 最终预览 → 二次确认 → 落盘），
 * 那套机制属 P6；本阶段它**如实标注未实现并禁用**，而不是假装能提交。
 */

import { useState } from 'react';

export interface CloseDialogProps {
  count: number;
  onSaveAndClose: () => void;
  onDiscardAndClose: () => void;
  onCancel: () => void;
}

function Option({
  title,
  desc,
  disabled,
  danger,
  onClick,
}: {
  title: string;
  desc: string;
  disabled?: boolean;
  danger?: boolean;
  onClick?: () => void;
}) {
  return (
    <div style={{ marginBottom: 'var(--spacing-4)' }}>
      <button className={danger ? 'btn danger' : 'btn'} type="button" disabled={disabled} onClick={onClick}>
        {title}
      </button>
      <div className="tbd" style={{ marginTop: 'var(--spacing-1)' }}>
        {desc}
      </div>
    </div>
  );
}

export default function CloseDialog({ count, onSaveAndClose, onDiscardAndClose, onCancel }: CloseDialogProps) {
  const [confirmDiscard, setConfirmDiscard] = useState(false);

  return (
    <div
      style={{
        position: 'fixed',
        inset: 0,
        zIndex: 40,
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'center',
        background: 'var(--bg)',
      }}
    >
      <div
        style={{
          maxWidth: 'calc(var(--thumb-size) * 5)',
          background: 'var(--surface)',
          border: '1px solid var(--border)',
          borderRadius: 'var(--radius-lg)',
          padding: 'var(--spacing-5)',
          boxShadow: 'var(--shadow-3)',
        }}
      >
        <div className="detail-title">还有 {count} 项未提交变更</div>
        <p className="tbd">
          这些改动**还没有落到磁盘**，素材文件目前仍是原样。请选一个，再关闭：
        </p>

        <Option
          title="提交并关闭"
          desc="先走完整提交流程（最终预览 + 二次确认）再退出。提交执行属 P6，本阶段尚未实现 —— 因此这里不可选。"
          disabled
        />
        <Option
          title="保存草稿并关闭"
          desc="草稿留在本地库里，真实文件不变；下次启动会恢复并提示「上次有 N 项未提交变更」。"
          onClick={onSaveAndClose}
        />
        {confirmDiscard ? (
          <div style={{ marginBottom: 'var(--spacing-4)' }}>
            <div className="notice warn">
              <div className="notice-body">
                <div className="notice-title">确定放弃这 {count} 项变更？</div>
                <ul className="notice-list">
                  <li>变更集会被清空，**真实文件不会发生任何变化**（本来也没动过）。</li>
                  <li>此操作不可撤销（草稿不会被保留）。</li>
                </ul>
              </div>
            </div>
            <button className="btn danger" type="button" onClick={onDiscardAndClose}>
              确认放弃并关闭
            </button>{' '}
            <button className="btn" type="button" onClick={() => setConfirmDiscard(false)}>
              再想想
            </button>
          </div>
        ) : (
          <Option
            title="放弃变更并关闭"
            desc="清空变更集，真实文件不变（会再问你一次）。"
            danger
            onClick={() => setConfirmDiscard(true)}
          />
        )}

        <div style={{ marginTop: 'var(--spacing-3)' }}>
          <button className="btn" type="button" onClick={onCancel}>
            返回（不关闭）
          </button>
        </div>
      </div>
    </div>
  );
}
