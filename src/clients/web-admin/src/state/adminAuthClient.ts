// SPDX-License-Identifier: AGPL-3.0-only

import React from "react";

export const ADMIN_JWT_STORAGE_KEY = "sandal.web_admin.jwt_session";
export const RETRY_BASE_DELAY_MS = 500;
export const RETRY_MAX_DELAY_MS = 30_000;
export const RETRY_MAX_ATTEMPTS = 6;
export const CIRCUIT_FAILURE_THRESHOLD = 5;
export const CIRCUIT_OPEN_COOLDOWN_MS = 15_000;

let retryResetEpoch = 0;
let consecutiveFailures = 0;
let circuitOpenUntilMs = 0;
const pendingRetryTimers = new Set<ReturnType<typeof setTimeout>>();
const retryResetListeners = new Set<(epoch: number, reason: string) => void>();

export function computeExponentialBackoffMs(
  attempt: number,
  baseMs = RETRY_BASE_DELAY_MS,
  maxMs = RETRY_MAX_DELAY_MS,
  rand: () => number = Math.random,
): number {
  const cleanAttempt = Math.max(0, Math.min(20, Math.trunc(Number.isFinite(attempt) ? attempt : 0)));
  const cap = Math.max(1, maxMs);
  const rawExp = Math.min(cap, Math.max(1, baseMs) * Math.pow(2, cleanAttempt));
  const jitter = Math.max(0, Math.min(1, Number(rand()) || 0));
  return Math.max(1, Math.round(rawExp * (0.5 + 0.5 * jitter)));
}

export function getRetryResetEpoch(): number {
  return retryResetEpoch;
}

export function getRetryCircuitOpenUntilMs(): number {
  return circuitOpenUntilMs;
}

export function isRetryCircuitOpen(nowMs = Date.now()): boolean {
  return circuitOpenUntilMs > nowMs;
}

export function recordRetrySuccess(): void {
  consecutiveFailures = 0;
  circuitOpenUntilMs = 0;
}

export function recordRetryFailure(nowMs = Date.now()): boolean {
  consecutiveFailures += 1;
  if (consecutiveFailures >= CIRCUIT_FAILURE_THRESHOLD) {
    circuitOpenUntilMs = nowMs + CIRCUIT_OPEN_COOLDOWN_MS;
    return true;
  }
  return false;
}

export function registerPendingRetryTimer(timer: ReturnType<typeof setTimeout>): void {
  pendingRetryTimers.add(timer);
}

export function subscribeRetryReset(listener: (epoch: number, reason: string) => void): () => void {
  retryResetListeners.add(listener);
  return () => {
    retryResetListeners.delete(listener);
  };
}

export function resetAllRetryTimersAndCircuits(reason = "manual"): number {
  retryResetEpoch += 1;
  consecutiveFailures = 0;
  circuitOpenUntilMs = 0;
  for (const timer of pendingRetryTimers) {
    clearTimeout(timer);
  }
  pendingRetryTimers.clear();
  for (const listener of retryResetListeners) {
    listener(retryResetEpoch, reason);
  }
  return retryResetEpoch;
}

export interface PersistedAdminJwtSession {
  readonly token: string | null;
  readonly username: string;
  readonly role: string;
  readonly scopes: readonly string[];
  readonly bootId: string;
}

export interface AdminAuthState extends PersistedAdminJwtSession {
  readonly authRequired: boolean;
  readonly loginPromptOpen: boolean;
  readonly lastAuthError: string | null;
}

export interface AdminLoginResult {
  readonly ok: boolean;
  readonly error?: string;
  readonly token?: string;
}

const DEFAULT_SCOPES: readonly string[] = Object.freeze([
  "world:read",
  "gm:write",
  "node:write",
  "logs:read",
  "containers:control",
]);

function resolveDefaultStorage(): Storage | null {
  try {
    return typeof window !== "undefined" && window.localStorage ? window.localStorage : null;
  } catch {
    return null;
  }
}

export const DEFAULT_ADMIN_USERNAME = "Admin";

export function normalizeAdminUsername(raw?: string | null): string {
  const trimmed = (raw ?? "").trim();
  return !trimmed || trimmed.toLowerCase() === "admin" ? DEFAULT_ADMIN_USERNAME : trimmed;
}

export function loadAdminSession(storage: Pick<Storage, "getItem"> | null = resolveDefaultStorage()): PersistedAdminJwtSession {
  try {
    const raw = storage?.getItem(ADMIN_JWT_STORAGE_KEY);
    if (!raw) {
      return { token: null, username: DEFAULT_ADMIN_USERNAME, role: "GM_ARCHITECT", scopes: DEFAULT_SCOPES, bootId: "" };
    }
    const parsed = JSON.parse(raw) as Partial<PersistedAdminJwtSession> & { readonly boot_id?: string };
    return {
      token: typeof parsed.token === "string" && parsed.token.length > 0 ? parsed.token : null,
      username: normalizeAdminUsername(parsed.username),
      role: typeof parsed.role === "string" && parsed.role.length > 0 ? parsed.role : "GM_ARCHITECT",
      scopes: Array.isArray(parsed.scopes) ? parsed.scopes.map(String) : DEFAULT_SCOPES,
      bootId: typeof parsed.bootId === "string" ? parsed.bootId : typeof parsed.boot_id === "string" ? parsed.boot_id : "",
    };
  } catch {
    return { token: null, username: DEFAULT_ADMIN_USERNAME, role: "GM_ARCHITECT", scopes: DEFAULT_SCOPES, bootId: "" };
  }
}

export function saveAdminSession(
  session: PersistedAdminJwtSession,
  storage: Pick<Storage, "setItem"> | null = resolveDefaultStorage(),
): void {
  try {
    storage?.setItem(ADMIN_JWT_STORAGE_KEY, JSON.stringify(session));
  } catch {
    // Ignore storage quota or private browsing errors
  }
}

const hydrated = loadAdminSession();
let currentState: AdminAuthState = {
  ...hydrated,
  authRequired: false,
  loginPromptOpen: false,
  lastAuthError: null,
};

const listeners = new Set<(state: AdminAuthState) => void>();

export function getAdminAuthState(): AdminAuthState {
  return currentState;
}

export function getActiveAdminJwtToken(): string | null {
  return currentState.token;
}

export function isUnauthenticatedViewOnly(): boolean {
  return Boolean(currentState.authRequired || currentState.loginPromptOpen);
}

export function useRetryResetEpoch(): number {
  const useSync = (
    React as unknown as {
      useSyncExternalStore?: <T>(sub: (cb: () => void) => () => void, getSnap: () => T) => T;
    }
  ).useSyncExternalStore;
  return typeof useSync === "function"
    ? useSync((cb) => subscribeRetryReset(() => cb()), getRetryResetEpoch)
    : getRetryResetEpoch();
}

export function setAdminAuthState(patch: Partial<AdminAuthState>): AdminAuthState {
  currentState = { ...currentState, ...patch };
  for (const listener of listeners) {
    listener(currentState);
  }
  return currentState;
}

export function subscribeAdminAuth(listener: (state: AdminAuthState) => void): () => void {
  listeners.add(listener);
  return () => {
    listeners.delete(listener);
  };
}

export function clearAdminSession(storage: Pick<Storage, "removeItem"> | null = resolveDefaultStorage()): void {
  try {
    storage?.removeItem(ADMIN_JWT_STORAGE_KEY);
  } catch {
    // Ignore storage removal errors
  }
  setAdminAuthState({ token: null, bootId: "", authRequired: true, loginPromptOpen: true });
  resetAllRetryTimersAndCircuits("logout");
}

export const clearAdminJwtToken = clearAdminSession;
export const logout = clearAdminSession;

export function refreshAdminSession(storage: Pick<Storage, "getItem"> | null = resolveDefaultStorage()): AdminAuthState {
  const loaded = loadAdminSession(storage);
  setAdminAuthState({
    ...loaded,
    authRequired: loaded.token ? false : currentState.authRequired,
    loginPromptOpen: loaded.token ? false : currentState.loginPromptOpen,
    lastAuthError: null,
  });
  resetAllRetryTimersAndCircuits("refresh");
  return currentState;
}

export function handleUnauthorizedStatus(status: number, reason?: string): void {
  if (status !== 401 && status !== 403) return;
  clearAdminSession();
  const detail = reason?.trim() || (status === 401 ? "Session expired or server restarted (401). Please sign in." : "Insufficient permissions (403). Please sign in.");
  setAdminAuthState({
    token: null,
    authRequired: true,
    loginPromptOpen: true,
    lastAuthError: detail,
  });
}

export function buildAdminAuthHeaders(existing?: HeadersInit): Record<string, string> {
  const headers: Record<string, string> = {};
  if (existing) {
    if (Array.isArray(existing)) {
      for (const [k, v] of existing) headers[k] = v;
    } else if (typeof Headers !== "undefined" && existing instanceof Headers) {
      existing.forEach((v, k) => {
        headers[k] = v;
      });
    } else {
      Object.assign(headers, existing as Record<string, string>);
    }
  }
  const token = currentState.token;
  if (token && !headers.Authorization && !headers.authorization) {
    headers.Authorization = `Bearer ${token}`;
  }
  return headers;
}

export function appendAdminAccessTokenToUrl(url: string, explicitToken?: string | null): string {
  const token = explicitToken !== undefined ? explicitToken : currentState.token;
  if (!token || !url || url.includes("access_token=")) return url;
  const sep = url.includes("?") ? "&" : "?";
  return `${url}${sep}access_token=${encodeURIComponent(token)}`;
}

export async function loginWithPassphrase(
  username: string,
  passphrase: string,
  fetchImpl?: typeof fetch,
): Promise<AdminLoginResult> {
  const cleanUser = username.trim() || DEFAULT_ADMIN_USERNAME;
  const cleanPass = passphrase.trim();
  if (!cleanPass) {
    setAdminAuthState({ lastAuthError: "Passphrase is required." });
    return { ok: false, error: "Passphrase is required." };
  }
  const rawFetch = fetchImpl ?? (typeof globalThis.fetch === "function" ? globalThis.fetch.bind(globalThis) : undefined);
  if (typeof rawFetch !== "function") {
    return { ok: false, error: "Fetch API unavailable." };
  }
  try {
    const res = await rawFetch("/api/v1/admin/auth/login", {
      method: "POST",
      headers: { "Content-Type": "application/json", Accept: "application/json" },
      body: JSON.stringify({ username: cleanUser, passphrase: cleanPass }),
    });
    const body = (await res.json().catch(() => ({}))) as Record<string, unknown>;
    if (!res.ok) {
      const errMsg = String(body.detail ?? body.error ?? body.title ?? `Authentication failed (HTTP ${res.status})`);
      setAdminAuthState({ authRequired: true, loginPromptOpen: true, lastAuthError: errMsg });
      return { ok: false, error: errMsg };
    }
    const token = typeof body.token === "string" ? body.token : typeof body.access_token === "string" ? body.access_token : "";
    const resolvedUser = normalizeAdminUsername(typeof body.username === "string" && body.username.length > 0 ? body.username : cleanUser);
    const role = typeof body.role === "string" && body.role.length > 0 ? body.role : "GM_ARCHITECT";
    const scopes = Array.isArray(body.scopes) ? body.scopes.map(String) : DEFAULT_SCOPES;
    const bootId = typeof body.boot_id === "string" ? body.boot_id : typeof body.bootId === "string" ? body.bootId : "";
    saveAdminSession({ token, username: resolvedUser, role, scopes, bootId });
    setAdminAuthState({
      token,
      username: resolvedUser,
      role,
      scopes,
      bootId,
      authRequired: false,
      loginPromptOpen: false,
      lastAuthError: null,
    });
    resetAllRetryTimersAndCircuits("login");
    return { ok: true, token };
  } catch (err) {
    const msg = (err as Error).message || "Network error during login.";
    setAdminAuthState({ lastAuthError: msg });
    return { ok: false, error: msg };
  }
}

const INTERCEPTOR_SYMBOL = Symbol.for("sandal.web_admin.fetch_interceptor_installed");

export function installGlobalAdminAuthFetchInterceptor(
  target: { fetch?: typeof fetch; [INTERCEPTOR_SYMBOL]?: boolean } = globalThis as unknown as { fetch?: typeof fetch },
): void {
  if (!target || typeof target.fetch !== "function" || target[INTERCEPTOR_SYMBOL]) return;
  const baseFetch = target.fetch.bind(target);
  const wrappedFetch: typeof fetch = async (input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
    const urlStr = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url;
    const isApiCall = urlStr.includes("/api/v1/");
    const isLoginCall = urlStr.includes("/api/v1/admin/auth/login");
    const nextInit: RequestInit = isApiCall ? { ...(init ?? {}), headers: buildAdminAuthHeaders(init?.headers) } : (init ?? {});
    const res = await baseFetch(input, nextInit);
    if (isApiCall && !isLoginCall && (res.status === 401 || res.status === 403)) {
      handleUnauthorizedStatus(res.status);
    }
    return res;
  };
  Object.defineProperty(wrappedFetch, INTERCEPTOR_SYMBOL, { value: true });
  target[INTERCEPTOR_SYMBOL] = true;
  target.fetch = wrappedFetch;
}

installGlobalAdminAuthFetchInterceptor();

if (typeof window !== "undefined" && typeof window.addEventListener === "function") {
  window.addEventListener("online", () => {
    resetAllRetryTimersAndCircuits("window-online");
  });
  window.addEventListener("focus", () => {
    resetAllRetryTimersAndCircuits("window-focus");
  });
}

export const useAdminAuthStore = Object.assign(
  function useAdminAuthSelector<U = AdminAuthState>(selector?: (state: AdminAuthState) => U): U {
    const useSync = (
      React as unknown as {
        useSyncExternalStore?: <T>(sub: (cb: () => void) => () => void, getSnap: () => T) => T;
      }
    ).useSyncExternalStore;
    const snap = typeof useSync === "function" ? useSync(subscribeAdminAuth, getAdminAuthState) : getAdminAuthState();
    return selector ? selector(snap) : (snap as unknown as U);
  },
  {
    getState: getAdminAuthState,
    setState: setAdminAuthState,
    subscribe: subscribeAdminAuth,
    loginWithPassphrase,
    clearAdminSession,
    refreshAdminSession,
    resetAllRetryTimersAndCircuits,
    handleUnauthorizedStatus,
  },
);
