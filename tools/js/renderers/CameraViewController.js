/**
 * CameraViewController.js
 * Controlador OO de vistas predefinidas de câmera CAD (Isométrica, XY, XZ, YZ).
 */
export class CameraViewController {
    /**
     * @param {THREE.Camera} camera
     * @param {OrbitControls} controls
     * @param {Riser3DRenderer|null} renderer - opcional; se informado, o enquadramento reusa
     *   renderer.computeSceneBounds() (mesma caixa/margens que o renderer efetivamente desenha)
     *   em vez de recalcular a caixa envolvente por conta própria -- evita as duas lógicas de
     *   margem divergirem com o tempo.
     */
    constructor(camera, controls, renderer = null) {
        this.camera = camera;
        this.controls = controls;
        this.renderer = renderer;
    }

    /**
     * Define a vista da câmera baseada na opção selecionada
     * @param {'ISO'|'XY'|'XZ'|'YZ'} viewType
     * @param {SimulationStep} currentStep
     * @param {number|null} seabedDepth - Se informado, garante que o enquadramento cubra até o
     *   fundo do mar, mesmo que os nós deste passo não cheguem lá.
     * @param {number|null} waterSurfaceZ - Idem, para a superfície do mar.
     */
    setView(viewType, currentStep, seabedDepth = null, waterSurfaceZ = null) {
        if (!this.camera || !this.controls) return;

        const bounds = this._computeBounds(currentStep, seabedDepth, waterSurfaceZ);
        const target = new THREE.Vector3(bounds.centerX, bounds.centerY, bounds.centerZ);

        // Direção câmera->alvo de cada vista predefinida. XY leva uma componente residual em Z
        // (não exatamente vertical) só para o produto vetorial com o "up" do mundo não degenerar
        // numa vista de cima perfeitamente reta.
        let dir;
        let fillFraction = 0.7;
        if (viewType === 'XY') { dir = new THREE.Vector3(0, -1, -0.001); fillFraction = 0.55; }
        else if (viewType === 'XZ') { dir = new THREE.Vector3(0, 0, -1); fillFraction = 0.55; }
        else if (viewType === 'YZ') { dir = new THREE.Vector3(-1, 0, 0); fillFraction = 0.55; }
        else dir = new THREE.Vector3(-0.55, -0.55, -1.3); // ISO (padrão)
        dir.normalize();

        // Vistas ortogonais (olhando reto ao longo de um eixo) projetam a caixa envolvente no seu
        // corte EXATO (o retângulo mínimo mesmo) -- já a ISO, sendo uma vista diagonal, projeta
        // uma "sombra" da caixa inteira, que é sempre maior que qualquer corte ortogonal. Com a
        // mesma fração-alvo de preenchimento, isso deixa XY/XZ/YZ visualmente mais "coladas" no
        // modelo que a ISO -- por isso uma folga maior (fillFraction menor) só pras ortogonais.
        this._frameBounds(target, bounds, dir, fillFraction);
    }

    /**
     * Reenquadra (zoom extents) o modelo inteiro SEM mudar a direção de vista atual -- ao
     * contrário de setView(), que pula para uma vista predefinida. Útil depois de orbitar/dar
     * zoom manualmente e perder o modelo de vista.
     * @param {SimulationStep} currentStep
     * @param {number|null} seabedDepth
     * @param {number|null} waterSurfaceZ
     */
    fitToModel(currentStep, seabedDepth = null, waterSurfaceZ = null) {
        if (!this.camera || !this.controls) return;
        const bounds = this._computeBounds(currentStep, seabedDepth, waterSurfaceZ);
        const target = new THREE.Vector3(bounds.centerX, bounds.centerY, bounds.centerZ);

        const dir = new THREE.Vector3();
        this.camera.getWorldDirection(dir); // mantém o ângulo de vista atual

        this._frameBounds(target, bounds, dir);
    }

    /**
     * Caixa envolvente (coordenadas Three.js: Y-three = Z-riser/profundidade), já com as mesmas
     * margens que o renderer usa pra desenhar o wireframe/planos ambientais -- delega pra
     * renderer.computeSceneBounds() quando disponível, pra a câmera sempre enquadrar exatamente o
     * que está sendo desenhado (não só a linha fina dos nós). Sem o renderer, cai numa versão sem
     * margem (só nós + solo/superfície) como default de segurança.
     */
    _computeBounds(currentStep, seabedDepth, waterSurfaceZ) {
        const nodes = (currentStep && currentStep.nodes) || [];

        if (this.renderer && typeof this.renderer.computeSceneBounds === 'function') {
            const b = this.renderer.computeSceneBounds(nodes, seabedDepth, waterSurfaceZ);
            if (b) {
                return {
                    ...b,
                    centerX: (b.minX + b.maxX) / 2.0, centerY: (b.minY + b.maxY) / 2.0, centerZ: (b.minZ + b.maxZ) / 2.0,
                };
            }
        }

        let minX = 0, maxX = 120, minY = -100, maxY = 0, minZ = -35, maxZ = 35;
        if (nodes.length > 0) {
            minX = Math.min(...nodes.map(n => n.x));
            maxX = Math.max(...nodes.map(n => n.x));
            minY = Math.min(...nodes.map(n => n.z));
            maxY = Math.max(...nodes.map(n => n.z));
            minZ = Math.min(...nodes.map(n => n.y));
            maxZ = Math.max(...nodes.map(n => n.y));
        }
        if (Number.isFinite(seabedDepth) && Math.abs(seabedDepth) < 1.0e5) {
            minY = Math.min(minY, seabedDepth);
            maxY = Math.max(maxY, seabedDepth);
        }
        if (Number.isFinite(waterSurfaceZ)) {
            minY = Math.min(minY, waterSurfaceZ);
            maxY = Math.max(maxY, waterSurfaceZ);
        }
        return {
            minX, maxX, minY, maxY, minZ, maxZ,
            centerX: (minX + maxX) / 2.0, centerY: (minY + maxY) / 2.0, centerZ: (minZ + maxZ) / 2.0,
        };
    }

    /**
     * Posiciona a câmera olhando de `target` na direção `dir` (câmera->alvo, já normalizada), a
     * uma distância calculada para que a caixa envolvente `bounds` caiba inteira no frustum --
     * projeta os 8 cantos da caixa nos eixos direita/cima da câmera e resolve a distância mínima
     * que satisfaz o FOV vertical E o FOV horizontal (== FOV vertical * aspect), em vez de um
     * multiplicador fixo -- um fator fixo (ex. `span * 0.9`) sub-dimensiona a distância sempre
     * que o canvas fica mais estreito que alto (ex. painel de dados largo ocupando a tela), daí o
     * modelo cortado nas bordas ("fica pra fora do quadro").
     * @param {THREE.Vector3} target
     * @param {{minX,maxX,minY,maxY,minZ,maxZ}} bounds
     * @param {THREE.Vector3} dir - direção câmera->alvo, normalizada
     * @param {number} fillFraction - fração do quadro que o modelo deve ocupar (1.0 = tocando a
     *   borda). Menor = mais folga ao redor.
     */
    _frameBounds(target, bounds, dir, fillFraction = 0.7) {
        const { minX, maxX, minY, maxY, minZ, maxZ } = bounds;

        let worldUp = new THREE.Vector3(0, 1, 0);
        if (Math.abs(dir.dot(worldUp)) > 0.999) worldUp = new THREE.Vector3(0, 0, 1);
        const right = new THREE.Vector3().crossVectors(dir, worldUp).normalize();
        const camUp = new THREE.Vector3().crossVectors(right, dir).normalize();

        const corners = [
            [minX, minY, minZ], [maxX, minY, minZ], [minX, maxY, minZ], [minX, minY, maxZ],
            [maxX, maxY, minZ], [maxX, minY, maxZ], [minX, maxY, maxZ], [maxX, maxY, maxZ],
        ];
        let halfW = 5.0, halfH = 5.0;
        const rel = new THREE.Vector3();
        corners.forEach(([x, y, z]) => {
            rel.set(x - target.x, y - target.y, z - target.z);
            halfW = Math.max(halfW, Math.abs(rel.dot(right)));
            halfH = Math.max(halfH, Math.abs(rel.dot(camUp)));
        });

        const fovRad = THREE.MathUtils.degToRad(this.camera.fov);
        const distV = halfH / Math.tan(fovRad / 2.0);
        const distH = halfW / (Math.tan(fovRad / 2.0) * Math.max(this.camera.aspect, 0.01));
        // distV/distH acima já são a distância EXATA de enquadramento (modelo tocando a borda,
        // 100% do quadro) -- para o modelo ocupar só uma fração do quadro (folga visível em
        // volta, espaço pra rótulos/legendas), a distância precisa ser 1/fillFraction vezes maior.
        const margin = 1.0 / fillFraction;
        const dist = Math.max(distV, distH, 5.0) * margin;

        this.camera.far = Math.max(2000, dist * 6.0);
        this.camera.updateProjectionMatrix();

        // Alinha camera.up ao "cima de tela" usado no cálculo de halfH acima -- sem isso,
        // OrbitControls.update() chama object.lookAt(target) usando o camera.up ANTERIOR (que
        // pode ficar quase paralelo à nova direção de vista, ex. na troca pra vista de topo XY),
        // um caso degenerado pro lookAt e o resultado fica com orientação inconsistente com o
        // enquadramento calculado.
        this.camera.up.copy(camUp);
        this.camera.position.copy(target).addScaledVector(dir, -dist);
        this.controls.target.copy(target);
        this.controls.update();
    }

    /** Aproxima/afasta a câmera ao longo da linha atual até o alvo, sem mudar de vista. */
    zoom(factor) {
        if (!this.camera || !this.controls) return;
        const offset = new THREE.Vector3().subVectors(this.camera.position, this.controls.target);
        const newLen = Math.max(0.5, offset.length() * factor);
        offset.normalize().multiplyScalar(newLen);
        this.camera.position.copy(this.controls.target).add(offset);
        this.controls.update();
    }

    /** Passo equivalente a N "ticks" da roda do mouse no OrbitControls: getZoomScale() lá é
     *  Math.pow(0.95, zoomSpeed) por tick -- reproduzido aqui pra o botão dar um passo familiar
     *  de orbitar/dar zoom manualmente na cena, só que mais perceptível que um único tick. */
    _wheelStepScale(ticks = 3) {
        const zoomSpeed = (this.controls && this.controls.zoomSpeed) || 1.0;
        return Math.pow(0.95, zoomSpeed * ticks);
    }

    zoomIn() { this.zoom(this._wheelStepScale()); }
    zoomOut() { this.zoom(1.0 / this._wheelStepScale()); }

    /**
     * Zoom "por janela" (tipo AutoCAD/CAD): recebe um retângulo em coordenadas normalizadas de
     * tela (NDC, -1..1, Y para cima) e reenquadra a câmera nele, mantendo a mesma direção de
     * vista atual (não pula para ISO/XY/etc, só aproxima/recentraliza no recorte escolhido).
     * Interseta os 4 raios dos cantos do retângulo com o plano focal (perpendicular à direção
     * da câmera, passando pelo alvo atual) e escala a distância câmera-alvo pela fração de tela
     * que o retângulo ocupava -- aproximação heurística mais simples que _frameBounds() (não
     * reprojeta cantos, só escala a distância atual pela fração de tela do retângulo).
     * @param {{x1:number,y1:number,x2:number,y2:number}} rectNDC
     */
    zoomToWindow(rectNDC) {
        if (!this.camera || !this.controls) return;

        const fraction = Math.max(Math.abs(rectNDC.x2 - rectNDC.x1), Math.abs(rectNDC.y2 - rectNDC.y1)) / 2.0;
        if (fraction < 0.01) return; // clique sem arrastar de verdade -- ignora

        const dir = new THREE.Vector3();
        this.camera.getWorldDirection(dir);
        const plane = new THREE.Plane().setFromNormalAndCoplanarPoint(dir, this.controls.target);

        const raycaster = new THREE.Raycaster();
        const corners = [
            { x: rectNDC.x1, y: rectNDC.y1 }, { x: rectNDC.x2, y: rectNDC.y1 },
            { x: rectNDC.x1, y: rectNDC.y2 }, { x: rectNDC.x2, y: rectNDC.y2 },
        ];
        const points = [];
        corners.forEach(c => {
            raycaster.setFromCamera(c, this.camera);
            const pt = new THREE.Vector3();
            if (raycaster.ray.intersectPlane(plane, pt)) points.push(pt);
        });
        if (points.length === 0) return;

        const newTarget = points.reduce((acc, p) => acc.add(p), new THREE.Vector3()).multiplyScalar(1.0 / points.length);

        const oldOffset = new THREE.Vector3().subVectors(this.camera.position, this.controls.target);
        const oldDistance = oldOffset.length();
        const newDistance = Math.max(0.5, oldDistance * fraction);
        const offsetDir = oldOffset.normalize();

        this.controls.target.copy(newTarget);
        this.camera.position.copy(newTarget).add(offsetDir.multiplyScalar(newDistance));
        this.controls.update();
    }
}
