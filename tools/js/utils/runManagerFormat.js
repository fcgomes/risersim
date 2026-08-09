/**
 * runManagerFormat.js
 * Small, dependency-free formatting/fetch helpers shared by the run manager's pages
 * (dashboard.js and project.js) -- extracted here because both files defined byte-identical
 * copies independently. A single shared module means one place to keep in sync with the backend
 * (run_server.py) and one typed surface if this project is ever ported to TypeScript (every
 * function here is a pure function with no DOM/class state, which maps directly to typed
 * exports without any restructuring).
 */

/** Human-readable label per run status, matching the values `risersim_projects.py`/
 * `run_worker.py` write into `run.json["status"]`. STATUS_LABELS keys must stay in sync
 * with the `status` values used across `risersim_projects.py`, `run_worker.py`, and the
 * `.badge-<status>` CSS rules in `css/dashboard.css`. */
export const STATUS_LABELS = {
    pending: 'Na fila',
    running: 'Executando',
    converged: 'Convergiu',
    failed: 'Falhou',
    aborted: 'Abortada',
};

/**
 * Renders a colored status badge (`<span class="badge badge-<status>">`).
 * @param {string} status One of `STATUS_LABELS`' keys (or any other string -- falls back to
 * displaying it verbatim, so an unrecognized status still shows *something* instead of breaking).
 * @returns {string} HTML markup for the badge.
 */
export function statusBadge(status) {
    const label = STATUS_LABELS[status] || status || 'desconhecido';
    return `<span class="badge badge-${status}">${label}</span>`;
}

/**
 * Renders a rounder "pill" status badge with a leading dot (`.status-pill` in css/dashboard.css) --
 * same colors as {@link statusBadge}, used in run tables/lists instead of the compact uppercase tag.
 * @param {string} status One of `STATUS_LABELS`' keys (or any other string, same fallback as statusBadge).
 * @returns {string} HTML markup for the pill.
 */
export function statusPill(status) {
    const label = STATUS_LABELS[status] || status || 'desconhecido';
    return `<span class="badge badge-${status} status-pill"><span class="dot"></span>${label}</span>`;
}

/**
 * Formats an ISO 8601 timestamp (as written by the backend's `_now_iso()`) for display.
 * @param {string|null|undefined} iso
 * @returns {string} Localized (pt-BR) date/time (day/month/year, hour:minute -- seconds omitted,
 * not useful at a glance in a list), an em dash for a missing value, or the raw string back if it
 * fails to parse (never throws).
 */
export function formatDate(iso) {
    if (!iso) return '—';
    try {
        return new Date(iso).toLocaleString('pt-BR', {
            day: '2-digit', month: '2-digit', year: 'numeric', hour: '2-digit', minute: '2-digit',
        });
    } catch (e) { return iso; }
}

/**
 * Formats an ISO 8601 timestamp the way the project cards' "Criado em"/"Última atividade" rows
 * do: `hoje, HH:MM` / `ontem, HH:MM` for the last two days, `DD/MM/AAAA` (no time) further back --
 * a summary card cares whether something happened recently, not its exact minute from a week ago.
 * @param {string|null|undefined} iso
 * @returns {string} As described above, an em dash for a missing value, or the raw string back
 * if it fails to parse (never throws).
 */
export function formatRelativeDate(iso) {
    if (!iso) return '—';
    let d;
    try { d = new Date(iso); } catch (e) { return iso; }
    if (Number.isNaN(d.getTime())) return iso;

    const now = new Date();
    const startOfDay = (date) => new Date(date.getFullYear(), date.getMonth(), date.getDate());
    const diffDays = Math.round((startOfDay(now) - startOfDay(d)) / 86400000);
    const time = d.toLocaleTimeString('pt-BR', { hour: '2-digit', minute: '2-digit' });

    if (diffDays === 0) return `hoje, ${time}`;
    if (diffDays === 1) return `ontem, ${time}`;
    return d.toLocaleDateString('pt-BR', { day: '2-digit', month: '2-digit', year: 'numeric' });
}

/**
 * Escapes a value for safe interpolation into an HTML string template.
 * @param {*} str Any value (coerced to string; `null`/`undefined` become `''`).
 * @returns {string} `str` with `& < > " '` replaced by their HTML entities.
 */
export function escapeHtml(str) {
    return String(str ?? '').replace(/[&<>"']/g, (c) => ({
        '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
    }[c]));
}

/**
 * `fetch()` wrapper that parses the JSON body and turns a non-2xx response into a thrown
 * `Error` -- every REST call in the run manager goes through this instead of raw `fetch()`, so
 * error handling (`try`/`catch` + a human-readable message) is consistent everywhere.
 * @param {string} url
 * @param {RequestInit} [options]
 * @returns {Promise<*>} The parsed JSON body.
 * @throws {Error} With the backend's `{"error": "..."}` message when available, otherwise
 * `HTTP <status>`.
 */
export async function fetchJSON(url, options) {
    const res = await fetch(url, options);
    if (!res.ok) {
        let detail = '';
        try { detail = (await res.json()).error || ''; } catch (e) { /* ignore -- body wasn't JSON */ }
        throw new Error(detail || `HTTP ${res.status}`);
    }
    return res.json();
}

/**
 * Renders the "where this model came from" panel: source lines (pre-discovered example path, or
 * "manual upload") + download links for the real source files and the compiled JSON -- the exact
 * content of project.js's "ORIGEM DO MODELO" panel, extracted here so preprocessor_app.js's
 * "Origem" tab (shown in place of the manual-upload controls when a project is loaded, see
 * preprocessor_app.js::applyProjectMode) can render the same information without duplicating it.
 * @param {string} projectId
 * @param {object} source `project.source` as returned by the API (xml_path/h5_path/aml_path,
 * relative to the project dir, plus example_id or kind:"upload").
 * @returns {{linesHtml: string, downloadsHtml: string}}
 */
export function modelSourceHtml(projectId, source) {
    const src = source || {};
    const lines = [];
    if (src.example_id) lines.push(`<div class="kv-line"><span class="k">Exemplo:</span>${escapeHtml(src.example_id)}</div>`);
    else if (src.kind === 'upload') lines.push(`<div class="kv-line"><span class="k">Origem:</span>📤 Upload manual</div>`);
    if (src.xml_path) lines.push(`<div class="kv-line"><span class="k">Arquivo:</span>${escapeHtml(src.xml_path.split('/').pop())}</div>`);
    const linesHtml = lines.join('') || '<div class="kv-line">—</div>';

    const downloads = [];
    for (const key of ['xml_path', 'h5_path', 'aml_path']) {
        const path = src[key];
        if (!path) continue;
        const filename = path.split('/').pop();
        const url = `/api/projects/${encodeURIComponent(projectId)}/source/${encodeURIComponent(filename)}?download=1`;
        downloads.push(`<a class="link-btn" href="${url}">⬇ ${escapeHtml(filename)}</a>`);
    }
    downloads.push(`<a class="link-btn" href="/api/projects/${encodeURIComponent(projectId)}/input?download=1">⬇ JSON compilado</a>`);
    return { linesHtml, downloadsHtml: downloads.join('') };
}

/** Thrown by {@link createRun} on a 409 (a terminal run with the same model_hash already exists)
 * -- carries the existing run's id/status so the caller can offer "run anyway" (retry with
 * `force: true`) instead of just failing. */
export class DuplicateRunError extends Error {
    constructor(runId, status) {
        super(`Já existe uma simulação terminada (${runId || '?'}, status: ${status || '?'}) com o mesmo modelo.`);
        this.name = 'DuplicateRunError';
        this.runId = runId;
        this.status = status;
    }
}

/**
 * `POST /api/projects/<id>/runs` -- extracted from project.js::createRun so
 * preprocessor_app.js's "Nova Simulação" button (in its Simulações tab) can trigger the same dedup-aware
 * creation flow without duplicating the 409 handling. Doesn't touch any button/dialog state --
 * callers own their own confirm-and-retry UI (see project.js::createRun for the pattern: catch
 * `DuplicateRunError`, confirm with the user, call again with `force: true`).
 * @param {string} projectId
 * @param {{force?: boolean}} [opts]
 * @returns {Promise<object>} The created run.
 * @throws {DuplicateRunError} On a 409 (duplicate model_hash).
 * @throws {Error} On any other non-2xx response.
 */
export async function createRun(projectId, { force = false } = {}) {
    const res = await fetch(`/api/projects/${encodeURIComponent(projectId)}/runs`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ force }),
    });
    if (res.status === 409) {
        const body = await res.json().catch(() => ({}));
        throw new DuplicateRunError(body.run_id, body.status);
    }
    if (!res.ok) {
        let detail = '';
        try { detail = (await res.json()).error || ''; } catch (e) { /* ignore */ }
        throw new Error(detail || `HTTP ${res.status}`);
    }
    return res.json();
}
