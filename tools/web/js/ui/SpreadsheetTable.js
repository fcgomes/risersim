/**
 * SpreadsheetTable.js
 * Reusable spreadsheet-style editable table for the "JSON de interface" editor (editor_app.js) --
 * wraps Tabulator (MIT, `tabulator-tables`, loaded as a global `Tabulator` via CDN script tag in
 * editor.html, same pattern as Three.js) so every catalog tab (soils/materials/currents/waves/
 * load_cases/analyses, plus a line's segments) gets the same real spreadsheet behavior instead of
 * each tab hand-rolling its own table:
 *   - Paste a block copied from Excel/Sheets straight into a cell (Tabulator's built-in clipboard
 *     module -- `clipboard: true, clipboardPasteAction: "range"`).
 *   - Stays smooth with hundreds/thousands of rows (Tabulator's row virtualization) -- THE reason
 *     this wraps an existing library instead of a hand-rolled table: virtualizing scroll well
 *     ourselves (recycling DOM nodes without losing focus/state, no jank) is a much bigger and
 *     easier-to-get-subtly-wrong undertaking than paste parsing ever was.
 *   - Per-column configurable unit for numeric fields that have one (`type: 'unit'`) -- the
 *     underlying data stays in SI, only display/edit go through a conversion.
 *
 * `items` is the SAME array the caller already owns (e.g. `this.model.materials`) -- every row is
 * that array's actual element object, mutated in place; there is no separate copy kept in sync.
 * Add/delete go through the returned handle so both the array and the on-screen table move
 * together in one call.
 */

const UNIT_GROUPS = {
    length: { m: 1, mm: 0.001, in: 0.0254, ft: 0.3048 },
    forcePerLength: { 'kN/m': 1, 'kgf/m': 0.00980665, 'lbf/ft': 0.0145939 },
    force: { kN: 1, kgf: 0.00980665, tf: 9.80665, lbf: 0.00444822 },
};

function parseNumberList(str) {
    return String(str || '')
        .split(',')
        .map(s => parseFloat(s.trim()))
        .filter(n => !Number.isNaN(n));
}

/** `{key, label, type: 'unit', group, getUnit, setUnit, decimals?}` -- see the module docstring. */
function unitColumn(col) {
    const { key: field, label, group, getUnit, setUnit, decimals = 3 } = col;
    return {
        field, title: label, hozAlign: 'right', widthGrow: 1,
        titleFormatter(_cell, _params, onRendered) {
            const wrap = document.createElement('div');
            wrap.className = 'th-unit-wrap';
            const labelSpan = document.createElement('span');
            labelSpan.textContent = label;
            const select = document.createElement('select');
            select.className = 'th-unit-select';
            Object.keys(UNIT_GROUPS[group]).forEach(u => {
                const opt = document.createElement('option');
                opt.value = u;
                opt.textContent = u;
                if (u === getUnit()) opt.selected = true;
                select.appendChild(opt);
            });
            wrap.appendChild(labelSpan);
            wrap.appendChild(select);
            onRendered(() => {
                // Without this, clicking the dropdown also triggers Tabulator's column sort.
                select.addEventListener('mousedown', (e) => e.stopPropagation());
                select.addEventListener('change', () => setUnit(select.value));
            });
            return wrap;
        },
        formatter(cell) {
            const v = cell.getValue();
            if (v === null || v === undefined || v === '') return '';
            return (v / UNIT_GROUPS[group][getUnit()]).toFixed(decimals);
        },
        // A custom editor (not Tabulator's built-in "number") -- needed so what the user TYPES is
        // read back in the current display unit and converted to SI before being stored, same as
        // the formatter does for display.
        editor(cell, onRendered, success, cancel) {
            const input = document.createElement('input');
            input.type = 'number';
            input.step = 'any';
            const raw = cell.getValue();
            const factor = UNIT_GROUPS[group][getUnit()];
            input.value = (raw === null || raw === undefined) ? '' : (raw / factor);
            input.style.width = '100%';
            input.style.boxSizing = 'border-box';
            onRendered(() => { input.focus(); input.select(); });
            const finish = () => {
                const val = input.value === '' ? null : parseFloat(input.value);
                success(val === null || Number.isNaN(val) ? null : val * factor);
            };
            input.addEventListener('blur', finish);
            input.addEventListener('keydown', (e) => {
                if (e.key === 'Enter') finish();
                else if (e.key === 'Escape') cancel();
            });
            return input;
        },
    };
}

/** `{key, label, type: 'number', decimals?}` -- plain numeric field, no unit conversion. */
function numberColumn(col) {
    const { key: field, label, decimals = 3 } = col;
    return {
        field, title: label, hozAlign: 'right', widthGrow: 1, editor: 'number',
        formatter(cell) {
            const v = cell.getValue();
            return (v === null || v === undefined || v === '') ? '' : Number(v).toFixed(decimals);
        },
    };
}

/** `{key, label, type: 'select', options: () => [{value, label}]}` -- `options()` is called fresh
 * on every render/edit, so a referenced catalog (e.g. materials for a segment's `material_id`)
 * can change between renders without this column needing to know about that structurally. */
function selectColumn(col) {
    const { key: field, label, options } = col;
    return {
        field, title: label, widthGrow: 1,
        formatter(cell) {
            const value = cell.getValue();
            const match = options().find(o => String(o.value) === String(value));
            const span = document.createElement('span');
            span.textContent = match ? match.label : (value ?? '');
            return span;
        },
        editor(cell, onRendered, success, cancel) {
            const select = document.createElement('select');
            select.style.width = '100%';
            options().forEach(o => {
                const opt = document.createElement('option');
                opt.value = o.value;
                opt.textContent = o.label;
                if (String(o.value) === String(cell.getValue())) opt.selected = true;
                select.appendChild(opt);
            });
            onRendered(() => select.focus());
            select.addEventListener('change', () => {
                const raw = select.value;
                success(/^-?\d+$/.test(raw) ? parseInt(raw, 10) : raw);
            });
            select.addEventListener('blur', () => cancel());
            return select;
        },
    };
}

/** `{key, label, type: 'checkbox'}`. */
function checkboxColumn(col) {
    return { field: col.key, title: col.label, hozAlign: 'center', widthGrow: 1, editor: 'tickCross', formatter: 'tickCross' };
}

/** `{key, label, type: 'list'}` -- comma-separated number array in one cell (e.g. a current's
 * velocity profile). */
function listColumn(col) {
    const { key: field, label } = col;
    return {
        field, title: label, widthGrow: 1,
        formatter(cell) {
            const span = document.createElement('span');
            span.textContent = (cell.getValue() || []).join(', ');
            return span;
        },
        editor(cell, onRendered, success, cancel) {
            const input = document.createElement('input');
            input.type = 'text';
            input.style.width = '100%';
            input.style.boxSizing = 'border-box';
            input.value = (cell.getValue() || []).join(', ');
            onRendered(() => { input.focus(); input.select(); });
            const finish = () => success(parseNumberList(input.value));
            input.addEventListener('blur', finish);
            input.addEventListener('keydown', (e) => {
                if (e.key === 'Enter') finish();
                else if (e.key === 'Escape') cancel();
            });
            return input;
        },
    };
}

/** `{key, label, type: 'text'}` (also the default when `type` is omitted). */
function textColumn(col) {
    return { field: col.key, title: col.label, editor: 'input', widthGrow: col.widthGrow ?? 1 };
}

/** `{key, label, type: 'custom', getValue: (item) => number|string, setValue: (item, value) =>
 * void, decimals?}` -- for a field Tabulator's own dot-notation field resolution can't reach
 * cleanly, e.g. one slot of an array (a line's `top_position_m[0]`) rather than a plain object
 * property. `key` still needs to be a unique string (Tabulator's internal bookkeeping wants a
 * `field` on every column), but reads/writes go through `getValue`/`setValue` against the whole
 * row object (`cell.getData()`), not through `key` itself -- Tabulator ends up also writing the
 * committed value onto `data[key]` as a side effect of how its editor API works, which is a
 * harmless unused property (same idea as the synthetic `id` backfill segments get). */
function customColumn(col) {
    const { key: field, label, getValue, setValue, decimals } = col;
    return {
        field, title: label, hozAlign: 'right', widthGrow: 1,
        formatter(cell) {
            const v = getValue(cell.getData());
            if (v === null || v === undefined || v === '') return '';
            return decimals != null ? Number(v).toFixed(decimals) : String(v);
        },
        editor(cell, onRendered, success, cancel) {
            const input = document.createElement('input');
            input.type = 'number';
            input.step = 'any';
            const v = getValue(cell.getData());
            input.value = (v === null || v === undefined) ? '' : v;
            input.style.width = '100%';
            input.style.boxSizing = 'border-box';
            onRendered(() => { input.focus(); input.select(); });
            const finish = () => {
                const val = input.value === '' ? null : parseFloat(input.value);
                setValue(cell.getData(), val === null || Number.isNaN(val) ? null : val);
                success(val);
            };
            input.addEventListener('blur', finish);
            input.addEventListener('keydown', (e) => {
                if (e.key === 'Enter') finish();
                else if (e.key === 'Escape') cancel();
            });
            return input;
        },
    };
}

function buildColumn(col) {
    switch (col.type) {
        case 'unit': return unitColumn(col);
        case 'number': return numberColumn(col);
        case 'select': return selectColumn(col);
        case 'checkbox': return checkboxColumn(col);
        case 'list': return listColumn(col);
        case 'custom': return customColumn(col);
        default: return textColumn(col);
    }
}

/**
 * @param {string|HTMLElement} container Selector or element Tabulator should mount into.
 * @param {Array<object>} items Live array (mutated in place) -- each element needs an integer
 * `id` (Tabulator's row index defaults to that field).
 * @param {Array<{key: string, label: string, type?: string, [x: string]: any}>} columns Content
 * columns -- an ID column and a delete-row column are added automatically, don't include them.
 * `key` may use dot notation for a nested field (e.g. `"static.steps"`) -- Tabulator resolves
 * that natively, both for display and for edits.
 * @param {{onDelete?: (item: object) => void, onChange?: () => void, selectable?: boolean|number,
 * onRowSelected?: (item: object) => void}} [callbacks] `onDelete` fires after a row is removed
 * from `items` AND from the table. `onChange` fires after any cell edit commits. `selectable`
 * turns on click-to-highlight rows (pass `1` for "at most one row at a time", radio-button style
 * -- e.g. the Linhas tab picking which line's Segmentos table to show); `onRowSelected` fires with
 * the newly-selected row's data, from both a real click AND a programmatic `handle.selectRow(id)`.
 * @returns {{addRow: (item: object) => void, selectRow: (id: number) => void, refresh: () => void,
 * destroy: () => void, table: object}}
 */
export function createSpreadsheetTable(container, items, columns, { onDelete, onChange, selectable, onRowSelected } = {}) {
    const tableColumns = [
        { field: 'id', title: 'ID', width: 55, hozAlign: 'right', editable: false },
        ...columns.map(buildColumn),
        {
            title: '', width: 36, hozAlign: 'center', headerSort: false, editable: false,
            formatter: () => '🗑',
            cellClick: (_e, cell) => {
                const item = cell.getRow().getData();
                const idx = items.indexOf(item);
                if (idx !== -1) items.splice(idx, 1);
                cell.getRow().delete();
                onDelete && onDelete(item);
            },
        },
    ];

    const table = new Tabulator(container, {
        data: items,
        layout: 'fitDataFill',
        placeholder: 'Nenhum item ainda.',
        columns: tableColumns,
        clipboard: true,
        clipboardPasteAction: 'range',
        selectable: selectable ?? false,
    });

    table.on('cellEdited', () => { onChange && onChange(); });
    if (onRowSelected) table.on('rowSelected', (row) => onRowSelected(row.getData()));

    // Tabulator measures its container's width when it's built -- if that happens while the
    // container is `display:none` (any tab other than the one active when this table is first
    // created), it's born at zero width and never recovers on its own once the tab becomes
    // visible (same failure class as the Plotly charts elsewhere in this app, which need an
    // explicit resize call after their tab is shown -- see preprocessor_app.js::setBottomTab()).
    // A ResizeObserver catches the display:none -> visible transition generically, for whichever
    // tab this table happens to live in, without the caller needing to know or coordinate it.
    const containerEl = typeof container === 'string' ? document.querySelector(container) : container;
    let lastWidth = containerEl.clientWidth;
    const resizeObserver = new ResizeObserver(() => {
        if (containerEl.clientWidth > 0 && lastWidth === 0) table.redraw(true);
        lastWidth = containerEl.clientWidth;
    });
    resizeObserver.observe(containerEl);

    return {
        table,
        addRow(item) {
            items.push(item);
            table.addData([item]);
        },
        /** Highlights row `id` as selected (only meaningful with `selectable` on) -- also fires
         * `onRowSelected`, same as a real click. Silently does nothing if the row isn't rendered
         * yet (e.g. called synchronously right after construction, before Tabulator's own async
         * build finishes) -- callers doing that should wait for the table's `tableBuilt` event
         * (`handle.table.on('tableBuilt', ...)`) instead. */
        selectRow(id) {
            try { table.selectRow(id); } catch { /* row not found/not built yet -- ignore */ }
        },
        /** Forces every cell to re-run its formatter -- for when something a formatter reads
         * (e.g. a referenced catalog's name, via a `select` column's `options()`) changed in a
         * DIFFERENT table and this one has no way to know on its own. */
        refresh() {
            table.redraw(true);
        },
        destroy() {
            resizeObserver.disconnect();
            table.destroy();
        },
    };
}
