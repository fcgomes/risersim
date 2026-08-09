/**
 * ConfirmDialog.js
 * In-page confirmation dialog, styled like the rest of the interface -- replaces
 * `window.confirm()` (the browser's native popup, which read as visually disconnected from the
 * rest of the UI) for the run manager's destructive actions (deleting a run/project, re-running
 * a duplicate model). Reuses the same `.modal-backdrop`/`.modal`/`.modal-actions` visual pattern
 * already used by the "New Project" modal (see css/dashboard.css and dashboard.html) instead of
 * inventing a second modal style -- only the inner content changes.
 *
 * The element is created once (lazily, on first use) and reused on every subsequent call, same
 * as the dashboard's static modal -- no markup of its own needed on each HTML page.
 */

let backdrop = null;

function ensureDialog() {
    if (backdrop) return backdrop;
    backdrop = document.createElement('div');
    backdrop.className = 'modal-backdrop';
    backdrop.innerHTML = `
        <div class="modal">
            <h3 id="confirm-dialog-title"></h3>
            <p class="confirm-dialog-message" id="confirm-dialog-message"></p>
            <div class="modal-actions">
                <button class="btn secondary" id="confirm-dialog-cancel-btn" type="button"></button>
                <button class="btn danger" id="confirm-dialog-ok-btn" type="button"></button>
            </div>
        </div>
    `;
    document.body.appendChild(backdrop);
    return backdrop;
}

/**
 * @param {{title?: string, message: string, confirmLabel?: string, cancelLabel?: string|null, danger?: boolean}} opts
 *   `cancelLabel: null` hides the Cancel button (used by `alertDialog` below) -- in that mode
 *   there's no "decline", so clicking outside/Esc/OK all resolve as `true`.
 * @returns {Promise<boolean>} true if the user confirmed (or dismissed an `alertDialog` that has
 *   no Cancel button); false if they actually cancelled (button, click outside, or Esc WITH
 *   Cancel visible).
 */
export function confirmDialog({ title = 'Confirmar ação', message, confirmLabel = 'Confirmar', cancelLabel = 'Cancelar', danger = true } = {}) {
    const el = ensureDialog();
    el.querySelector('#confirm-dialog-title').textContent = title;
    el.querySelector('#confirm-dialog-message').textContent = message;
    const okBtn = el.querySelector('#confirm-dialog-ok-btn');
    const cancelBtn = el.querySelector('#confirm-dialog-cancel-btn');
    okBtn.textContent = confirmLabel;
    okBtn.className = danger ? 'btn danger' : 'btn';
    const hasCancel = cancelLabel !== null;
    cancelBtn.style.display = hasCancel ? '' : 'none';
    if (hasCancel) cancelBtn.textContent = cancelLabel;

    return new Promise((resolve) => {
        const cleanup = (result) => {
            el.classList.remove('open');
            okBtn.removeEventListener('click', onOk);
            cancelBtn.removeEventListener('click', onCancel);
            el.removeEventListener('click', onBackdropClick);
            document.removeEventListener('keydown', onKeydown);
            resolve(result);
        };
        // No Cancel button (alertDialog) means there's no way to decline -- any way of closing
        // it counts as "true" (just an acknowledged notice, not a yes/no decision).
        const onOk = () => cleanup(true);
        const onCancel = () => cleanup(false);
        const onBackdropClick = (e) => { if (e.target === el) cleanup(!hasCancel); };
        const onKeydown = (e) => { if (e.key === 'Escape') cleanup(!hasCancel); };

        okBtn.addEventListener('click', onOk);
        cancelBtn.addEventListener('click', onCancel);
        el.addEventListener('click', onBackdropClick);
        document.addEventListener('keydown', onKeydown);
        el.classList.add('open');
        okBtn.focus();
    });
}

/**
 * In-page notice with no way to decline -- replaces `window.alert()` (the browser's native
 * popup) for error/status messages (e.g. "Failed to delete project: ..."). Reuses `confirmDialog`
 * above with the Cancel button hidden.
 * @param {{title?: string, message: string, okLabel?: string, danger?: boolean}} opts
 * @returns {Promise<void>} resolves once the user dismisses the notice.
 */
export async function alertDialog({ title = 'Aviso', message, okLabel = 'OK', danger = false } = {}) {
    await confirmDialog({ title, message, confirmLabel: okLabel, cancelLabel: null, danger });
}
