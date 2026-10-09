/**
 * 主题三档（§9.3：浅色 / 深色 / 跟随系统）。
 *
 * 只切换 `document.documentElement[data-theme]`，色值全部来自 tokens.css；
 * 这里不出现任何色值（§12.4②）。
 */

import { useEffect, useState } from 'react';

export type ThemePref = 'light' | 'dark' | 'system';
export type ResolvedTheme = 'light' | 'dark';

export const THEME_LABEL: Record<ThemePref, string> = {
  light: '浅色',
  dark: '深色',
  system: '跟随系统',
};

const STORAGE_KEY = 'luling.theme';
const QUERY = '(prefers-color-scheme: dark)';

function readPref(): ThemePref {
  try {
    const raw = window.localStorage.getItem(STORAGE_KEY);
    if (raw === 'light' || raw === 'dark' || raw === 'system') return raw;
  } catch {
    /* 存储不可用：用跟随系统 */
  }
  return 'system';
}

function systemTheme(): ResolvedTheme {
  return window.matchMedia(QUERY).matches ? 'dark' : 'light';
}

export interface Theme {
  pref: ThemePref;
  resolved: ResolvedTheme;
  setPref: (t: ThemePref) => void;
}

export function useTheme(): Theme {
  const [pref, setPrefState] = useState<ThemePref>(readPref);
  const [sys, setSys] = useState<ResolvedTheme>(systemTheme);

  useEffect(() => {
    const mq = window.matchMedia(QUERY);
    const onChange = () => setSys(mq.matches ? 'dark' : 'light');
    mq.addEventListener('change', onChange);
    return () => mq.removeEventListener('change', onChange);
  }, []);

  const resolved: ResolvedTheme = pref === 'system' ? sys : pref;

  useEffect(() => {
    document.documentElement.dataset.theme = resolved;
  }, [resolved]);

  const setPref = (t: ThemePref) => {
    setPrefState(t);
    try {
      window.localStorage.setItem(STORAGE_KEY, t);
    } catch {
      /* 忽略 */
    }
  };

  return { pref, resolved, setPref };
}
