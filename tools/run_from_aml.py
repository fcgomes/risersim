"""
run_from_aml.py
===============
Pipeline ANFLEX AML → JSON Input → Executável riserSim (C++) → Visualizador 3D

Fluxo:
1. Converte o arquivo .aml do ANFLEX para um arquivo JSON padronizado (input_simulation.json) usando Python (aml_reader.py).
2. Executa o binário compilado risersim_test_main apontando para o JSON de entrada.
3. Abre automaticamente o visualizador 3D no navegador.

Uso:
    python run_from_aml.py <arquivo.aml> [opções]

Opções:
    --line-index N      Índice da linha no AML a simular (default: 0)
    --num-elements N    Sobrescreve o número de elementos (default: do AML)
    --static-only       Executa apenas a análise estática
    --offset-near       Simula offset Near (default: Far)
    --duration T        Duração da análise dinâmica em segundos (default: 20.0)
    --dt DT             Passo de tempo dinâmico (default: 0.05)
    --no-viewer         Não abre o visualizador Web no final
    --out-dir DIR       Diretório de saída dos resultados (default: ./risersim_results)
    --exe-path PATH     Caminho customizado para o executável risersim_test_main

Exemplos:
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

# Adiciona o diretório do script ao sys.path para importar aml_reader
_SCRIPT_DIR = Path(__file__).parent.resolve()
sys.path.insert(0, str(_SCRIPT_DIR))

from aml_reader import ANFLEXAMLReader


def find_executable(custom_path: str = None) -> Path:
    """Localiza o executável risersim_test_main."""
    exe_name = 'risersim_test_main.exe' if platform.system() == 'Windows' else 'risersim_test_main'
    
    if custom_path:
        p = Path(custom_path)
        if p.is_file():
            return p
        
    # Locais típicos de compilação CMake
    root = _SCRIPT_DIR.parent
    candidates = [
        root / 'build' / 'bin' / exe_name,
        root / 'build' / 'bin' / 'Release' / exe_name,
        root / 'build' / 'bin' / 'Debug' / exe_name,
        root / 'build' / exe_name,
        root / 'build' / 'Release' / exe_name,
        Path('.') / 'build' / 'bin' / exe_name,
        Path('.') / 'build' / 'bin' / 'Release' / exe_name,
    ]

    for c in candidates:
        if c.is_file():
            return c

    found = shutil.which(exe_name)
    if found:
        return Path(found)

    return None


def main():
    parser = argparse.ArgumentParser(
        description='Pipeline ANFLEX AML → JSON Input → riserSim Engine',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )
    parser.add_argument('aml_file', help='Caminho para o arquivo .aml do ANFLEX')
    parser.add_argument('--line-index', type=int, default=0, help='Índice da linha a simular')
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

    # 1. Converter AML para estrutura de dados
    print(f"\n📖 Lendo arquivo AML: {aml_path}")
    reader = ANFLEXAMLReader(str(aml_path))
    reader.summary()

    config = reader.to_risersim_config(line_index=args.line_index)

    if args.num_elements:
        config['geometry']['num_elements'] = args.num_elements

    if args.offset_near:
        config['offsets'] = {
            'near_m': config['offsets'].get('near_m', 10.0),
            'far_m': 0.0
        }

    # Adiciona amortecimento Rayleigh de segurança se for 0 no AML para evitar divergência
    ray = config.get('rayleigh', {})
    config['rayleigh'] = {
        'alpha': max(ray.get('alpha', 0.0), 0.05),
        'beta': max(ray.get('beta', 0.0), 0.005)
    }

    config['simulation_options'] = {
        'static_only': args.static_only,
        'duration_s': args.duration,
        'dt_s': args.dt
    }

    # 2. Criar diretório de saída e salvar o JSON de entrada
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    input_json_path = out_dir / 'input_simulation.json'
    with open(input_json_path, 'w', encoding='utf-8') as f:
        json.dump(config, f, indent=2, ensure_ascii=False)
    print(f"✅ Arquivo de entrada JSON gerado com sucesso: {input_json_path}")

    # 3. Localizar e executar o binário C++
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

    # 4. Copiar os resultados para o visualizador e abrir se solicitado
    if not args.no_viewer:
        viewer_path = _SCRIPT_DIR / 'viewer_3d.html'
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
