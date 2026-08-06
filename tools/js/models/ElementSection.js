/**
 * ElementSection.js
 * Wraps a beam element from the INPUT JSON (undeformed, pre-solution) -- node connectivity +
 * raw section properties, as read by ModelBuilder (not the solved-result scalars that
 * BeamElement3D represents).
 */
export class ElementSection {
    /**
     * @param {number} id
     * @param {number} node1Id - 1-indexed, same as the JSON (node1_id).
     * @param {number} node2Id - 1-indexed, same as the JSON (node2_id).
     * @param {object} sectionProperties - E, G, A, D_outer, D_inner, Ca, EI, weight_wet_kNm (whatever is present).
     */
    constructor(id, node1Id, node2Id, sectionProperties = {}) {
        this.id = id;
        this.node1Id = node1Id;
        this.node2Id = node2Id;
        this.sectionProperties = sectionProperties;
    }

    /**
     * Grouping key -- elements with identical section properties collapse into the same table
     * row instead of one row per element (real models have hundreds of elements, most sharing
     * the same section).
     * @returns {string}
     */
    get groupKey() {
        const sp = this.sectionProperties;
        return [sp.E, sp.G, sp.A, sp.D_outer, sp.D_inner, sp.Ca, sp.EI, sp.weight_wet_kNm]
            .map(v => (v === undefined || v === null) ? '' : v)
            .join('|');
    }
}
