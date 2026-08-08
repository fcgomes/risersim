"""
risersim_runner.py
===================
Lógica compartilhada de orquestração do pipeline riserSim, extraída de `run_from_aml.py` (o
workflow manual original, CLI-only) para que o novo "gerenciador de rodadas" (`run_server.py` /
`run_worker.py`, ver docs/roadmap.md Eixo 3b) possa reaproveitá-la em vez de duplicá-la.

Funções aqui:
- `find_executable()`: localiza o binário compilado `risersim_test_main`.
- `discover_xml_h5_examples()`: varre `trunk/exemplos/` procurando pastas `<nome>_analysis/` com
  um par XML+H5 real exportado pelo ANFLEX (o requisito de `xml_h5_reader.py`) -- hoje só 7 dos
  ~30 exemplos têm essa pasta (ver docs/mapa_aml_exemplos_e_web_interface.md).
- `build_config_from_xml_h5()`: compila o JSON de simulação (schema que `ModelBuilder` consome) a
  partir de um par XML+H5, reaproveitando `xml_h5_reader.py` tal e qual.
- `run_simulation_subprocess()`: invoca o binário C++ com o mesmo padrão de `subprocess` que
  `run_from_aml.py` já usava (stdout/stderr redirecionáveis para um arquivo de log, cwd no
  diretório de saída).
"""

import shutil
import subprocess
import platform
import sys
from pathlib import Path

_SCRIPT_DIR = Path(__file__).parent.resolve()
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))


def find_executable(custom_path: str = None) -> Path:
    """Localiza o executável risersim_test_main. Mesma lógica de sempre (ver run_from_aml.py
    histórico): caminho customizado > locais típicos de build CMake > PATH do sistema."""
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


def discover_xml_h5_examples(exemplos_root):
    """Varre `exemplos_root` procurando pastas `<nome>_analysis/` com um par XML+H5 real
    exportado (o único formato de entrada suportado no Phase 1 do gerenciador de rodadas -- ver
    docs/roadmap.md Eixo 3b e docs/mapa_aml_exemplos_e_web_interface.md).

    Descoberta por conteúdo, não por convenção de nome fixa: a maioria dos exemplos usa
    `<nome>_A1.xml`/`.h5`, mas pelo menos um (`Manifold`) usa `<nome>_An1.xml`/`.h5` -- em vez de
    assumir o sufixo "_A1" (como `run_from_aml.py` faz para o caso de uso CLI single-file), aqui
    procuramos o único XML "principal" da pasta (excluindo os `..._results_static/dynamic.xml`
    secundários, que são saídas de pós-processamento do ANFLEX, não entradas de modelo) e
    pareamos com o H5 de mesmo nome-base.

    Retorna uma lista de dicts `{id, name, xml_path, h5_path, aml_path, source_dir}`, ordenada
    por id. `id` é o caminho relativo (com "/" mesmo no Windows) da pasta do exemplo em relação a
    `exemplos_root` -- único mesmo quando o nome-base colide entre exemplos diferentes (ex.:
    "Sombra/EfeitoSombra" vs. "SemSombra/EfeitoSombra"). `aml_path` é `None` quando o `.aml`
    correspondente não está ao lado da pasta `_analysis/` (não deveria ocorrer nos exemplos reais,
    mas o `.aml` é só usado para dois metadados opcionais -- ver `build_config_from_xml_h5` --,
    não é estritamente necessário).
    """
    exemplos_root = Path(exemplos_root)
    results = []
    if not exemplos_root.is_dir():
        return results

    for analysis_dir in sorted(exemplos_root.rglob('*_analysis')):
        if not analysis_dir.is_dir():
            continue
        stem = analysis_dir.name[: -len('_analysis')]

        xml_candidates = [p for p in analysis_dir.glob('*.xml') if '_results_' not in p.name.lower()]
        if len(xml_candidates) != 1:
            continue
        xml_path = xml_candidates[0]
        h5_path = xml_path.with_suffix('.h5')
        if not h5_path.is_file():
            continue

        aml_path = analysis_dir.parent / f"{stem}.aml"

        rel_dir = analysis_dir.parent.relative_to(exemplos_root)
        example_id = "/".join(rel_dir.parts)

        results.append({
            "id": example_id,
            "name": stem,
            "xml_path": str(xml_path),
            "h5_path": str(h5_path),
            "aml_path": str(aml_path) if aml_path.is_file() else None,
            "source_dir": str(analysis_dir.parent),
        })

    results.sort(key=lambda e: e["id"])
    return results


def build_config_from_xml_h5(xml_path, h5_path, aml_path=None, duration=None, dt=None, static_only=False):
    """Compila o JSON de simulação (schema que `ModelBuilder::load_from_json` consome) a partir
    de um par XML+H5 real, usando `xml_h5_reader.py` tal e qual (não reimplementado aqui).

    Extraído do branch `use_xml_h5` de `run_from_aml.py` (o pipeline CLI original) para que tanto
    o CLI quanto o novo gerenciador de rodadas (`run_server.py`) montem o config exatamente do
    mesmo jeito.

    `aml_path`, se informado, alimenta dois metadados que só existem no texto `.aml`/`.pml`, não
    no XML/H5 (ver `xml_h5_reader.extract_assembly_flag`/`extract_static_convergence_criterium`):
    o flag real `%ASSEMBLY.USING` e o critério de convergência estática real
    (`%ANALYSIS_CASE.STATIC.CONVERGENCE_CRITERIUM`/`MAX_UNBALANCED`). Sem `aml_path`, o config
    fica sem esses campos e o risersim usa os próprios defaults seguros (comportamento idêntico a
    antes desta extração).

    `duration`/`dt`, se informados (não `None`), sobrescrevem a duração/passo de tempo dinâmicos
    lidos do XML -- mesma semântica de `--duration`/`--dt` no CLI original (só sobrescreve quando
    o usuário realmente pediu, para não mascarar os valores reais do XML com um default fixo).
    `static_only=True` desliga a análise dinâmica inteira (mesma semântica de `--static-only`).
    """
    from xml_h5_reader import ANFLEXXmlH5Reader, extract_assembly_flag, extract_static_convergence_criterium

    reader = ANFLEXXmlH5Reader(str(xml_path), str(h5_path))
    try:
        config = reader.to_risersim_json()
    finally:
        reader.close()

    if aml_path:
        assembly_flag = extract_assembly_flag(str(aml_path))
        if assembly_flag is not None:
            config['analysis_options']['static']['use_assembly_phase'] = assembly_flag

        enable_unbalanced, max_unbalanced = extract_static_convergence_criterium(str(aml_path))
        if enable_unbalanced:
            config['analysis_options']['static']['enable_unbalanced_criteria'] = True
            if max_unbalanced is not None:
                config['analysis_options']['static']['unbalanced_force_tol'] = max_unbalanced
                config['analysis_options']['static']['unbalanced_moment_tol'] = max_unbalanced

    if duration is not None:
        config['analysis_options']['dynamic']['duration_s'] = duration
    if dt is not None:
        config['analysis_options']['dynamic']['dt_s'] = dt
    if static_only:
        config['analysis_options']['dynamic']['enabled'] = False

    return config


def run_simulation_subprocess(exe_path, input_json_path, out_dir, stdout_file=None):
    """Invoca o binário `risersim_test_main` -- mesmo padrão de `subprocess` que `run_from_aml.py`
    já usava (`[exe, input_json, out_dir]`, cwd=out_dir), com o adicional opcional de redirecionar
    stdout/stderr para um arquivo (usado pelo `run_worker.py`: `stdout.log` é a fonte de verdade de
    progresso ao vivo de uma rodada, já que o binário C++ imprime por iteração via
    `std::cout`/`std::endl`, sem precisar de nenhuma mudança no C++).

    `stdout_file`: caminho (str/Path) para onde redirecionar stdout+stderr combinados, ou `None`
    para herdar o stdout/stderr do processo Python chamador (comportamento do CLI original).

    Retorna o código de saída do processo.
    """
    cmd = [str(exe_path), str(input_json_path), str(out_dir)]
    if stdout_file is not None:
        with open(stdout_file, 'w', encoding='utf-8') as log_f:
            result = subprocess.run(cmd, cwd=str(out_dir), stdout=log_f, stderr=subprocess.STDOUT)
    else:
        result = subprocess.run(cmd, cwd=str(out_dir))
    return result.returncode
