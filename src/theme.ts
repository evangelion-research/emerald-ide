import "./styles/themes.css";

export const THEMES = ["emerald", "tokyo", "mono", "paper"] as const;
export type ThemeName = (typeof THEMES)[number];

const STORAGE_KEY = "emerald-ide.theme";

export function loadTheme(): ThemeName {
  try {
    const t = localStorage.getItem(STORAGE_KEY);
    if (t && (THEMES as readonly string[]).includes(t)) return t as ThemeName;
  } catch {
    /* storage unavailable */
  }
  return "emerald";
}

export function applyTheme(name: ThemeName): void {
  document.documentElement.dataset.theme = name;
  try {
    localStorage.setItem(STORAGE_KEY, name);
  } catch {
    /* ignore */
  }
}

export function initThemeSelect(select: HTMLSelectElement): void {
  for (const t of THEMES) {
    const opt = document.createElement("option");
    opt.value = t;
    opt.textContent = t;
    select.appendChild(opt);
  }
  select.value = loadTheme();
  applyTheme(loadTheme());
  select.addEventListener("change", () => {
    applyTheme(select.value as ThemeName);
  });
}
