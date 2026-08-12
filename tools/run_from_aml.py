"""
run_from_aml.py
===============
Pipeline: ANFLEX AML -> JSON Input -> riserSim Executable (C++) -> 3D Viewer

Flow:
1. Converts ANFLEX's .aml file into a standardized JSON file (input_simulation.json) using Python (aml_reader.py).
2. Runs the compiled risersim_test_main binary pointing at the input JSON.
3. Automatically opens the 3D viewer in the browser.

Usage:
    python run_from_aml.py <file.aml> [options]

Options:
    --line-index N      Index of the line in the AML to simulate (default: 0)
    --num-elements N    Overrides the number of elements (default: from the AML)
    --static-only       Runs only the static analysis
    --offset-near       Simulates the Near offset (default: Far)
    --duration T        Dynamic analysis duration in seconds (default: 20.0)
    --dt DT             Dynamic timestep (default: 0.05)
    --no-viewer         Doesn't open the Web viewer at the end
    --out-dir DIR       Output directory for the results (default: ./risersim_results)
    --exe-path PATH     Custom path to the risersim_test_main executable

Examples:
    python risersim/tools/run_from_aml.py exemplos/Curso/Exemplo_01/Exemplo_01a/Exemplo_01a.aml
    python risersim/tools/run_from_aml.py meu_riser.aml --num-elements 80 --duration 40
"""

import os
import sys
import json
import argparse
import subprocess
import shutil
import webbrowser
import platform
from pathlib import Path

# Adds the script's directory to sys.path to import aml_reader
_SCRIPT_DIR = Path(__file__).parent.resolve()
sys.path.insert(0, str(_SCRIPT_DIR))

from aml_reader import ANFLEXAMLReader
# find_executable() and compiling the config from XML+H5 were extracted into
# risersim_runner.py, also reused by the new run manager (run_server.py/run_worker.py, see
# docs/roadmap.md, Axis 3b) -- don't duplicate this logic here.
from risersim_runner import find_executable, build_config_from_xml_h5


def main():
    parser = argparse.ArgumentParser(
        description='Pipeline ANFLEX AML → JSON Input → riserSim Engine',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )
    parser.add_argument('aml_file', help='Caminho para o arquivo .aml do ANFLEX')
    parser.add_argument('--line-index', type=int, default=0, help='Índice da linha a simular')
    parser.add_argument('--load-case-id', type=int, default=None,
                         help='ID real do %%LOAD_CASE a usar (só caminho .aml puro -- '
                              'um .aml pode ter vários, ex. "Near"/"Far"; sem isso, usa o primeiro '
                              'do arquivo). Ignorado no caminho XML+H5 (ver --force-aml-path), que já '
                              'corresponde a um só load case (o que foi de fato exportado/rodado no '
                              'ANFLEX real).')
    parser.add_argument('--force-aml-path', action='store_true',
                         help='Força o caminho .aml puro (malha sintética, reader.to_risersim_json) '
                              'mesmo quando existe uma pasta "<nome>_analysis" com XML+H5 exportado. '
                              'Necessário pra rodar com --load-case-id um %%LOAD_CASE que não foi o '
                              'exportado (o XML+H5 real corresponde só a um %%LOAD_CASE por vez -- '
                              'ver docs/roadmap.md, eixo 2a).')
    parser.add_argument('--num-elements', type=int, default=None, help='Sobrescreve o número de elementos')
    parser.add_argument('--static-only', action='store_true', help='Executa apenas análise estática')
    parser.add_argument('--offset-near', action='store_true', help='Usa offset Near em vez de Far')
    parser.add_argument('--duration', type=float, default=20.0, help='Duração dinâmica (s)')
    parser.add_argument('--dt', type=float, default=0.05, help='Passo de tempo (s)')
    parser.add_argument('--no-viewer', action='store_true', help='Não abre o visualizador')
    parser.add_argument('--out-dir', default='./risersim_results', help='Diretório de saída')
    parser.add_argument('--exe-path', default=None, help='Caminho para o risersim_test_main')

    args = parser.parse_args()

    aml_path = Path(args.aml_file)
    if not aml_path.exists():
        print(f"❌ Arquivo AML não encontrado: {aml_path}")
        sys.exit(1)

    # 1. Convert the AML or XML+H5 into a data structure
    # Checks whether there's an associated _analysis folder containing XML + H5
    parent_dir = aml_path.parent
    base_name = aml_path.stem
    analysis_dir = parent_dir / f"{base_name}_analysis"
    xml_file = analysis_dir / f"{base_name}_A1.xml"
    h5_file = analysis_dir / f"{base_name}_A1.h5"

    use_xml_h5 = xml_file.is_file() and h5_file.is_file() and not args.force_aml_path
    config = {}

    if use_xml_h5:
        print(f"\n📦 Detectada pasta de análise XML+H5. Importando modelo robusto:")
        print(f"   XML: {xml_file}")
        print(f"   H5:  {h5_file}")

        # Overrides command-line parameters only when the user actually asked for it
        # (same semantics as before: without explicit --duration/--dt, uses the XML's real value).
        duration_override = args.duration if '--duration' in sys.argv else None
        dt_override = args.dt if '--dt' in sys.argv else None

        config = build_config_from_xml_h5(
            xml_file, h5_file, aml_path=aml_path,
            duration=duration_override, dt=dt_override,
            static_only=args.static_only,
        )

        static_opts = config['analysis_options']['static']
        if 'use_assembly_phase' in static_opts:
            # The real %ASSEMBLY.USING only exists in the .aml/.pml text, not in the XML/H5 -- it
            # controls whether the real ANFLEX runs the assembly phase (artificial stiffness at
            # every step + a separate static phase) or just a single ramp (see
            # mapa_classes_anflex_estatica.md).
            print(f"   %ASSEMBLY.USING real: {static_opts['use_assembly_phase']}")
        if static_opts.get('enable_unbalanced_criteria'):
            # The real %ANALYSIS_CASE.STATIC.CONVERGENCE_CRITERIUM/MAX_UNBALANCED -- if the real
            # ANFLEX combines the displacement criterion with a maximum unbalanced force/moment
            # cap ('DISP_AND_FORCE'), turns on the same escape valve risersim already had
            # implemented but had never read from the real AML (see mapa_classes_anflex_estatica.md).
            print(f"   %ANALYSIS_CASE.STATIC.CONVERGENCE_CRITERIUM real: DISP_AND_FORCE "
                  f"(max_unbalanced={static_opts.get('unbalanced_force_tol')})")

        # Overrides command-line parameters if given
        if args.num_elements:
            print("⚠️ Nota: O número de elementos é fixado pela malha XML+H5 e não será sobrescrito.")

    else:
        print(f"\n📖 Lendo arquivo AML: {aml_path}")
        reader = ANFLEXAMLReader(str(aml_path))
        reader.summary()

        # reader.to_risersim_json() (not the older to_risersim_config()) is what actually
        # produces the schema ModelBuilder::load_from_json reads -- see docs/roadmap.md item 2a
        # and aml_reader.py's own docstring for the two methods. --num-elements is threaded
        # straight into mesh synthesis (there's no separate 'geometry' dict to patch after the
        # fact anymore, the mesh is already built by the time this call returns). --load-case-id
        # picks which %LOAD_CASE (current+wave pairing) to resolve from when the .aml defines
        # more than one (e.g. 'Near'/'Far') -- defaults to the first in the file, same as before
        # this flag existed, when not given.
        config = reader.to_risersim_json(
            line_index=args.line_index, num_elements_override=args.num_elements,
            load_case_id=args.load_case_id,
        )

        # Same override semantics as the XML+H5 branch above (build_config_from_xml_h5): only
        # overrides duration/dt/dynamic-enabled when the user actually passed the corresponding
        # flag, so as not to mask the AML's own real values with the CLI's fixed defaults.
        if '--duration' in sys.argv:
            config['analysis_options']['dynamic']['duration_s'] = args.duration
        if '--dt' in sys.argv:
            config['analysis_options']['dynamic']['dt_s'] = args.dt
        if args.static_only:
            config['analysis_options']['dynamic']['enabled'] = False

        # Safety-net Rayleigh damping if the AML computed zero (avoids divergence in the dynamic
        # phase) -- same floor as before this change, just applied to the new schema's location.
        ray = config['analysis_options']['dynamic']['rayleigh_damping']
        ray['alpha'] = max(ray.get('alpha', 0.0), 0.05)
        ray['beta'] = max(ray.get('beta', 0.0), 0.005)

    # 2. Create the output directory and save the input JSON
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    input_json_path = out_dir / 'input_simulation.json'
    with open(input_json_path, 'w', encoding='utf-8') as f:
        json.dump(config, f, indent=2, ensure_ascii=False)
    print(f"✅ Arquivo de entrada JSON gerado com sucesso: {input_json_path}")

    # 3. Locate and run the C++ binary
    exe = find_executable(args.exe_path)
    if not exe:
        print("\n⚠️ Binário 'risersim_test_main' não encontrado.")
        print("   Por favor, compile o projeto C++ com o CMake:")
        print("     cmake -B build risersim/ && cmake --build build --config Release")
        print(f"\n   O arquivo de entrada foi gerado em: {input_json_path}")
        sys.exit(1)

    print(f"\n🚀 Executando riserSim engine: {exe}")
    cmd = [str(exe), str(input_json_path), str(out_dir)]

    try:
        result = subprocess.run(cmd, cwd=str(out_dir))
        if result.returncode != 0:
            print(f"\n❌ A simulação falhou (código de erro: {result.returncode})")
            sys.exit(result.returncode)
    except Exception as e:
        print(f"\n❌ Falha ao executar o processo: {e}")
        sys.exit(1)

    # 4. Copy the results to the viewer and open it if requested
    if not args.no_viewer:
        viewer_path = _SCRIPT_DIR / 'web' / 'posprocessor.html'
        if viewer_path.exists():
            root_dir = _SCRIPT_DIR.parent
            for res_file in ['catenary_results.json', 'catenary_results.h5']:
                src = out_dir / res_file
                if src.exists():
                    shutil.copy2(src, root_dir / res_file)

            print(f"\n🌐 Abrindo visualizador Web 3D: {viewer_path}")
            webbrowser.open(viewer_path.as_uri())

    print("\n🎉 Simulação concluída com sucesso!")


if __name__ == '__main__':
    main()
