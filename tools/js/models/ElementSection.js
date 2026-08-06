/**
 * ElementSection.js
 * Encapsula um elemento de viga do JSON de ENTRADA (não deformado, pré-solução) -- conectividade
 * de nós + propriedades de seção brutas, como lidas pelo ModelBuilder (não escalares de
 * resultado já resolvido, que é o que BeamElement3D representa).
 */
export class ElementSection {
    /**
     * @param {number} id
     * @param {number} node1Id - 1-indexed, igual ao JSON (node1_id).
     * @param {number} node2Id - 1-indexed, igual ao JSON (node2_id).
     * @param {object} sectionProperties - E, G, A, D_outer, D_inner, Ca, EI, weight_wet_kNm (o que estiver presente).
     */
    constructor(id, node1Id, node2Id, sectionProperties = {}) {
        this.id = id;
        this.node1Id = node1Id;
        this.node2Id = node2Id;
        this.sectionProperties = sectionProperties;
    }

    /**
     * Chave de agrupamento -- elementos com propriedades de seção idênticas colapsam na mesma
     * linha da tabela em vez de uma linha por elemento (modelos reais têm centenas de elementos,
     * a maioria compartilhando a mesma seção).
     * @returns {string}
     */
    get groupKey() {
        const sp = this.sectionProperties;
        return [sp.E, sp.G, sp.A, sp.D_outer, sp.D_inner, sp.Ca, sp.EI, sp.weight_wet_kNm]
            .map(v => (v === undefined || v === null) ? '' : v)
            .join('|');
    }
}
