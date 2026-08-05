/**
 * CameraViewController.js
 * Controlador OO de vistas predefinidas de câmera CAD (Isométrica, XY, XZ, YZ).
 */
export class CameraViewController {
    /**
     * @param {THREE.Camera} camera 
     * @param {OrbitControls} controls 
     */
    constructor(camera, controls) {
        this.camera = camera;
        this.controls = controls;
    }

    /**
     * Define a vista da câmera baseada na opção selecionada
     * @param {'ISO'|'XY'|'XZ'|'YZ'} viewType 
     * @param {SimulationStep} currentStep 
     */
    setView(viewType, currentStep) {
        if (!this.camera || !this.controls) return;

        let minX = 0, maxX = 120, minY = -100, maxY = 0, minZ = -35, maxZ = 35;
        if (currentStep && currentStep.nodes && currentStep.nodes.length > 0) {
            minX = Math.min(...currentStep.nodes.map(n => n.x));
            maxX = Math.max(...currentStep.nodes.map(n => n.x));
            minY = Math.min(...currentStep.nodes.map(n => n.z));
            maxY = Math.max(...currentStep.nodes.map(n => n.z));
            minZ = Math.min(...currentStep.nodes.map(n => n.y));
            maxZ = Math.max(...currentStep.nodes.map(n => n.y));
        }

        const centerX = (minX + maxX) / 2.0;
        const centerY = (minY + maxY) / 2.0;
        const centerZ = (minZ + maxZ) / 2.0;
        const target = new THREE.Vector3(centerX, centerY, centerZ);

        // Distância de câmera proporcional ao tamanho real do modelo (bounding box), em vez de
        // um offset absoluto fixo (ex. 60-260 unidades) calibrado só para a escala do
        // Exemplo_01a (~100-160m). Para um modelo bem maior (ex. Exemplo_02a, ~1800m de lâmina
        // d'água), um offset fixo posiciona a câmera praticamente dentro da geometria,
        // renderizando só uma fatia minúscula e distorcida -- parece uma linha "dobrada/caída"
        // mesmo quando as coordenadas em si estão corretas (ver mapa_classes_anflex_estatica.md).
        const span = Math.max(maxX - minX, maxY - minY, maxZ - minZ, 10.0);
        const dist = span * 0.9;

        // O far plane da câmera também precisa acompanhar a escala do modelo, senão partes
        // dele ficam cortadas (clipping) em modelos grandes.
        this.camera.far = Math.max(2000, dist * 6.0);
        this.camera.updateProjectionMatrix();

        if (viewType === 'ISO') {
            this.camera.position.set(centerX + dist * 0.55, centerY + dist * 0.55, centerZ + dist * 1.3);
        } else if (viewType === 'XY') {
            // Vista Superior
            this.camera.position.set(centerX, centerY + dist * 1.8, centerZ + 0.001);
        } else if (viewType === 'XZ') {
            // Vista de Elevação / Perfil
            this.camera.position.set(centerX, centerY, centerZ + dist * 1.8);
        } else if (viewType === 'YZ') {
            // Vista Frontal Transversal
            this.camera.position.set(centerX + dist * 2.1, centerY, centerZ);
        }

        this.controls.target.copy(target);
        this.controls.update();
    }
}
