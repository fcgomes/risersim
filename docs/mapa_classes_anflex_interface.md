# Mapa da interface gráfica do ANFLEX real (`trunk/interfaces/src`) — construção de modelo, AML/PML, DAT/XML

> Documento de referência, produzido lendo o código-fonte real da interface gráfica do ANFLEX
> (read-only, `trunk/interfaces/src`, aplicação IUP/Lua/tolua, ~662 arquivos num único diretório
> flat) para complementar [`mapa_classes_anflex_estatica.md`](mapa_classes_anflex_estatica.md)
> (que mapeia o solver, `trunk/src`). Cobre: como o modelo é montado interativamente
> (`cAppModel`), como o `.aml`/`.pml` é lido e escrito, e como o `.dat`/`.mov` e o `.xml`/`.h5` são
> exportados a partir do modelo — os formatos que o `risersim` consome via
> `tools/run_from_aml.py`/`tools/xml_h5_reader.py`.

## Por que este documento existe

A investigação do bug de convergência solo+corrente (ver documento do solver) já tinha ido tão
fundo no lado do `trunk/src` que a próxima fonte de discrepância possível só podia estar de um lado
que ainda não tínhamos lido: como os dados que alimentam o solver são de fato produzidos. Esta
leitura já encontrou uma lacuna real e concreta no pipeline atual do `risersim` — ver a seção final
"Lacunas encontradas".

## `cAppModel` — container raiz do lado da interface (`anfmodel.h`/`.cpp`)

`class cAppModel : public cObject` (`anfmodel.h:81`) é o equivalente, do lado da interface, do
`cDomain` do solver — mas pensado para edição interativa, não para simulação. Guarda coleções por
conceito de domínio, todas `cCollection<T>` (`anfmodel.h:282-302`):

```
colSoil<cSoil>, colReference<cReference>, colConnection<cConnection>, colNodalMass<cNodalMass>,
colMaterial<cMaterial>, colFunction<cFunction>, colLine<cLine>, colPipeline<cPipeline>,
colBuoy<cBuoy>, colTendon<cTendon>, colCurrent<cCurrent>, colWave<cWave>, colNode<cNode>,
colLoadCase<cLoadCase>, colAnalysis<cAnalysis>, colRAO<cRAO>, colScalar<cScalar>,
colTimeHistory<cTimeHistory>, colRefSys<cRefSys>, colSNCurves<cSNCurve>, colContactRegion<cContactRegion>
```

mais `cGlobalData global` (`anfmodel.h:334`) e `cAssembly assembly` (`anfmodel.h:335`, **uma única
instância por modelo**, não por caso de análise — ver seção "cAssembly vs cAnalysis" abaixo).

**IDs**: `cAppModel::m_next_id` começa em `MIN_OBJ_ID=100` (`anfmodel.h:55,227`). Todo `push_X`
(soil, current, analysis...) passa por `cAppModel::push_obj_ref<T>` (`anfmodel.cpp:2011-2026`): se
o objeto lido do AML já tinha um `id>0` explícito (campo `X.ID` no arquivo), esse id é preservado e
`m_next_id` é ajustado para não colidir; senão, um novo id é gerado. Busca por label usa
`cCollection<T>::handle()`/`repeated()` (`collection.h:89,112`).

**Padrão de edição**: a GUI/Lua manipula wrappers `cXxxLua` (`cLineLua`, `cSoilLua`, `cCurrentLua`,
`cAnalysisLua`, etc.) via `create_X`/`get_X`/`del_X`; o método privado `cAppModel::update(cObjectLua*)`
(`anfmodel.h:1108`, despachando para `update_line`/`update_soil`/`update_current`/`update_analysis`/
etc., `anfmodel.h:134-151`) sincroniza de volta para os objetos de domínio reais guardados nas
coleções acima. Existem dois modelos globais paralelos em runtime — `ANF_PRE` (edição, `.aml`) e
`ANF_POS` (pós-processamento/resultados, `.pml`) — `anfmodel.h:271`.

## `cAml` — leitura de AML/PML (`aml.h`/`.cpp`)

### Dispatch table

`cAml` mantém `map<string, sInput> fmap` (`aml.h:49`), `sInput = {label; bool (cAml::*read_method)();}`.
O construtor só chama `set_read_callbacks()` **se `mode == MODE_READ`** (`aml.cpp:77-78`) — nota
importante, ver "`cAml` é read-only na prática" abaixo. `set_read_callbacks()` (`aml.cpp:105-178`)
popula ~70 entradas mapeando tags textuais (`"CURRENT"`, `"SOIL"`, `"GLOBAL"`, `"ASSEMBLY.BEGIN"`,
`"ANALYSIS_CASE"`...) para ponteiros de método `read_X`.

`readAML()` (`aml.cpp:182-283`) faz um loop simples: lê a próxima tag até `END`, despacha pro
`read_X` correspondente se reconhecida. **Tags desconhecidas são silenciosamente ignoradas** (sem
erro/warning) — fonte potencial de perda silenciosa de dados entre versões. Ao final, chama
`fixAML()` (`aml.cpp:287-395`) — cadeia de migrações condicionadas à versão do arquivo (`fix_soil`,
`fix_refsys`, `fix_dnv_material`, `fix_turret_wave_angle`, etc.) que corrige dados de arquivos
antigos automaticamente, antes de qualquer análise rodar.

### Campos relevantes ao bug de convergência

**`read_current`** (`aml.cpp:992-1035`, leitura real em `current.cpp:406-620`): cria o `cCurrent`
já apontando por padrão para as funções de rampa globais `StaTfDef`/`DynTfDef`
(`model->colFunction.ref_handle(STAT_TF_DEF/DYN_TF_DEF)`, macros de `function.h:15-16`), a menos
que o AML tenha `CURRENT.TIME_FUNCTION[_ID]`/`CURRENT.TIME_FUNCTION.DYNAMIC[.ID]` explícitos
(`current.cpp:557-577`) apontando pra outra função. Perfil espacial (`CURRENT.DEPTH`/`.VELOCITY`/
`.ANGLE|AZIMUTH|DIRECTION`, arrays paralelos) é lido separadamente — não confundir com a função de
rampa temporal, que só escala esse perfil no tempo.

A função default `StaTfDef` tem pontos fixos `(0,0),(1,0),(11,1)` (`function.cpp:302-304`) —
corrente zerada até t=1, rampa linear até 100% em t=11. `DynTfDef` é constante `(0,1),(10800,1)`
(`function.cpp:297-298`). O ponto `t=11` casa com o `TOTAL_TIME` default de 11.0 tanto em
`cAssembly::reset()` quanto em `cAnalysis::reset_static()` — ou seja, por padrão a corrente sobe
de 0 a 100% exatamente ao longo da janela de assembly.

**`read_soil`** (`aml.cpp:2787-2828`, leitura real em `soil.cpp:333+`): campos e defaults
(`cSoil::reset()`, `soil.cpp:109-153`):

| Tag AML | Campo (`soil.h:164-169`) | Default |
|---|---|---|
| `SOIL.FRICTION_SPRING.ON/OFF` | `friction` (bool) | `true` |
| `SOIL.COUPLED` / `SOIL.UNCOUPLED` | `m_coupled` | `false` (uncoupled) |
| `SOIL.PYTZ` | `m_pytz` (curvas custom, mutuamente exclusivo com `m_coupled`) | `false` |
| `SOIL.DEFLECTION.AXIAL` | `att[AXIAL_DEFLECTION]` | **0.03** |
| `SOIL.DEFLECTION.LATERAL` | `att[LATERAL_DEFLECTION]` | **0.001** |
| `SOIL.FRICTION.AXIAL` | `att[AXIAL_FRICTION]` | **0.2** |
| `SOIL.FRICTION.LATERAL` | `att[LATERAL_FRICTION]` | **0.5** |
| `SOIL.SPRING.STIFFNESS` | `att[SPRING_STIFFNESS]` | **100.0** |

Note bem: **`SOIL.DEFLECTION.AXIAL/LATERAL`** é o nome real da tag — o que a documentação do
solver chama de "limite elástico" do atrito, aqui é nomeado "deflection" (o deslocamento de
referência da mola antes de deslizar). `read_soil` não emite warning em redefinição de label
(diferente de `read_current`, que emite).

**`read_global`** (`aml.cpp:1720-1916`) popula `model->global` (`cGlobalData`): `GLOBAL.SEABED.*`
(geometria/profundidade/azimute/declividade do leito), `GLOBAL.GRAVITY`,
`GLOBAL.{STEEL,WATER}_SPECIFIC_WEIGHT`, `GLOBAL.WATER_VISCOSITY`. Tem lógica de correção
retroativa própria: AML antigo com declividade ≠0 mas sem `GLOBAL.SEABED.HORIZONTAL` é assumido
`INCLINED` e reconstruído a partir da normal calculada (`aml.cpp:1734-1777`).

## `cAssembly` vs `cAnalysis` — dois orçamentos de convergência paralelos e independentes

Este é o achado de maior interesse arquitetural desta leitura. O ANFLEX real tem **dois objetos
separados**, cada um com seu próprio conjunto de parâmetros de convergência — não um único
conjunto compartilhado entre a fase de assembly e a análise estática real:

**`cAssembly`** (`assembly.h`/`.cpp`, uma instância **global ao modelo**, `cAppModel::assembly`) —
bloco `%ASSEMBLY.BEGIN...END`, lido em `aml.cpp:785-883`, defaults em `assembly.cpp:118-137`:

| Campo | Default |
|---|---|
| `using_status` (`%ASSEMBLY.USING`) | **false** |
| `TOTAL_TIME` | 11.0 |
| `TIME_STEP` | 1.0 |
| `MAX_ITERATION` | **20** |
| `ERROR_TOLERANCE` | 0.001 |
| `USE_MAX_UNBALANCED_FORCE` | YES (força=1.0) |
| `USE_MAX_UNBALANCED_MOMENT` | NO (momento=1.0) |
| `DISABLE_SOIL_FRICTION_SPRINGS` | NO — flag dedicada pra desligar as molas de atrito **só durante o assembly** |

**`cAnalysis`** (`analysis.h`/`.cpp` — **arquivo diferente do `analysis.h`/`.cpp` de `trunk/src`,
cuidado ao citar**, uma instância **por caso de análise**, ligada a um `LoadCase`) — bloco
`%ANALYSIS_CASE.STATIC.*`, lido em `analysis.cpp:434-530`, defaults em `analysis.cpp:306-323`:

| Campo | Default |
|---|---|
| `CRITERIUM` | `DISP_AND_FORCE` |
| `TOTAL_TIME` | 11.0 |
| `TIME_STEP` | 1.0 |
| `MAX_ITERATION` | **40** |
| `ERROR_TOLERANCE` | 0.001 |
| `USE_MAX_UNBALANCED_FORCE` | YES (força=1.0) |
| `USE_MAX_UNBALANCED_MOMENT` | NO (momento=1.0) |

Achado colateral de migração: se `TIME_STEP` lido for `<1.0` (formato antigo, fração 0-1), é
**descartado e forçado para 1.0** com warning (`analysis.cpp:452-477`) — silenciosamente, antes
até de qualquer exportação.

**Por que isso importa para o `risersim`**: `StaticAnalysis::solve()`
([`static_analysis.cpp:199-227`](../src/static_analysis.cpp)) roda as duas fases (assembly →
static) reusando os **mesmos** `max_iter_per_step`/`tol` para as duas — não o orçamento
`MAX_ITERATION=20`/`ERROR_TOLERANCE` próprio do assembly real. Para o Exemplo_01a isso é
irrelevante (`%ASSEMBLY.USING.FALSE`, a fase de assembly nunca roda), mas é uma lacuna de
fidelidade real para qualquer modelo com assembly ligado — ver "Lacunas encontradas" abaixo.

## `cCurrent` (`current.h`/`.cpp`)

`vector<sCurrentProfile> profile` (perfil espacial velocidade×profundidade×ângulo,
`current.h:37-46`) + `tFunctionRef *m_static_tf_ref, *m_dynamic_tf_ref` (`current.h:68`) —
referências diretas a `cFunction` na coleção `colFunction` (mecanismo genérico
`tFunctionRef`/`tFunctionRefTempID` usado em todo o modelo para referências cruzadas por id/label,
resolvidas em `fixAML()` após a leitura completa). Exportação XML
(`current.cpp:732-764`, `export_xml()`): grava só `static_function_id`/`dynamic_function_id` (a
referência, não os pontos) + `factor=1.0` (hardcoded) + o perfil como dado tabular pesado
(`E_LIGHT_DATA`).

## `cSoil` (`soil.h`/`.cpp`)

Atributos em `list<sSoilAtt> att` (chave-string → real, `soil.h:61`, chaves em `soil.h:164-169`).
Booleans `friction`/`m_coupled`/`m_pytz` — `m_coupled`/`m_pytz` mutuamente exclusivos por
construção (`soil.cpp:358-371,402-405`). Não há classe "solo acoplado" separada — é o mesmo
`cSoil`, mesmos 6 atributos numéricos, só a flag `m_coupled` muda; a diferença de comportamento
físico fica inteiramente do lado do solver (`trunk/src`).

## Escrita de AML/PML: `cAml` é read-only na prática

Busca completa por `new cAml(...)` em todo `interfaces/src`: só 3 instanciações, **todas em
`MODE_READ`** (`main.cpp:352,357` para `.cfg` de preferências; `p3d.cpp:2418`; `cb-main.cpp:2972`,
o ponto de entrada canônico do carregamento do modelo). Nenhuma usa `MODE_WRITE` — o enum existe
(`aml.h:163`) mas é vestigial, a classe não tem nenhum método `write_*`.

Quem de fato escreve `.aml`/`.pml` é uma função livre em `anfout.cpp`, sem passar por `cAml`:
`bool ioSaveProject(const string& fullname, cAppModel* model)` (`anfout.cpp:542-642`) escreve
diretamente com `fprintf`, chamando `.save(FILE*, cAnflog*)` em cada coleção/objeto, numa ordem
fixa e documentada como sensível a efeitos colaterais (`anfout.cpp:590-591`: *"nao altere a ordem
de gravacao dos dados"*). Cada classe de domínio implementa `read()`/`save()` simétricos — mesmas
tags, textualmente. Chamada pelo menu File > Save (`dg-main.cpp:781`), por `ExportPMLFile`
(`export.cpp:119`), e pelo modo de linha de comando `-e`/`-export` (`command_line.cpp:199`).

**Ciclo de vida real, modo batch** (`command_line.cpp:190-229`, o caminho de produção usado para
rodar análises sem GUI):
```
LoadFile(aml_name, ...)                                    // lê .aml via cAml::readAML()
if (data_status == DATA_EDITED)                             // fixAML() alterou algo (migração)
    ioSaveProject(aml_name, appCurrentModel)                 // RESSALVA O .aml ORIGINAL, sobrescrevendo!
ExportDatFiles(appCurrentModel, ..., is_xml, ...)            // só então exporta .dat/.xml
```
**Cautela para comparações futuras**: se a leitura de um `.aml` antigo disparar qualquer correção
de `fixAML()`, o arquivo original em disco é sobrescrito com a versão corrigida *antes* de gerar o
`.dat`/`.xml` — o texto do `.aml` que efetivamente gerou uma dada exportação pode não ser
bit-a-bit idêntico ao arquivo original se ele vier de uma versão mais antiga do ANFLEX.

## DAT vs XML: dois formatos de saída, uma única classe (`cSaveDatFile`, `save-dat.cpp`, ~12500 linhas)

`cSaveDatFile(cAppModel *m)` tem dois conjuntos paralelos de métodos: `save_*(FILE *f, ...)`
(texto DAT/MOV, orquestrado por `save()`, `save-dat.cpp:6873`) e `save_*_xml(cAppGauge*,
cWriterNode*, ...)` (árvore XML, orquestrado por `save_xml()`, `save-dat.cpp:7124`). São caminhos
**mutuamente exclusivos escolhidos manualmente na UI** (`dg-run.cpp:483`, dois botões de "Run"
separados: `bt_run_anflex_xml` vs. o botão DAT normal) — não há lógica automática que decida qual
usar. Achado direto no código: `dg-run.cpp:482` tem um comentário do próprio time do ANFLEX —
`//TODO (thyago, 2013-10-09) Needs to change when only xml remain` — XML é o formato "vencedor"
planejado, DAT está em fase de eliminação.

### Achado mais importante desta seção: o `.DAT` não carrega parâmetros de convergência

`save_static_analysis(FILE *f, bool assembly)` (`save-dat.cpp:3312-3355`) e
`save_assembly_analysis(FILE *f)` (`save-dat.cpp:12293-12331`) têm o **corpo inteiro comentado**
(`//`). Nenhuma linha `NR/TOTALT/STEP/NITER/TOL/ICRIT/...` chega a ser escrita — **o `.DAT` texto
não contém critério de convergência, tolerância, número máximo de iterações, nem
`MAX_UNBALANCED_FORCE`/`MOMENT`, para nenhuma das duas fases**. Confirma que a escolha já feita
pelo `risersim` (ler XML/HDF5 via `tools/run_from_aml.py`/`xml_h5_reader.py`, nunca o `.DAT`) é a
correta — qualquer ferramenta que dependesse do `.DAT` para esses parâmetros estaria lendo um
arquivo sem essa informação.

### `%ASSEMBLY.USING` no XML: presença/ausência de nó, não um atributo

Não existe nenhum campo booleano equivalente no XML. Em vez disso: `<Assembly>` só é criado dentro
de `<LoadCase>` (`save_loading_cases_xml`, `save-dat.cpp:7410`) e dentro de `<AnalysisData>`
(`save_analysis_data_xml`, `save-dat.cpp:10822`) **se** `m_model->assembly.get_using_status()` for
verdadeiro — senão o nó inteiro é omitido. O `risersim` já lida com isso corretamente, mas por uma
via diferente: `xml_h5_reader.py::extract_assembly_flag()` lê o token literal
`%ASSEMBLY.USING.TRUE/FALSE` direto do texto `.aml`/`.pml` (não do XML) — funciona porque a
convenção textual é autodescritiva, mas vale registrar que, se algum dia se quiser ler isso *só*
do XML (sem o `.aml` em mãos), a forma correta é checar
`root.find("AnalysisData/Assembly") is not None`, não procurar um atributo.

### `save_analysis_data_xml` (`save-dat.cpp:10810-11030`)

```
<AnalysisData reorderer="reverse_cuthill_mckee|sloan">
  <Assembly>              <!-- só existe se assembly.get_using_status()==true -->
    <ConvergenceCriterium .../>
  </Assembly>
  <Static ...>
    <ConvergenceCriterium .../>
  </Static>
</AnalysisData>
```
Bloco `Static` (`10875-10924`): `total_time`, `time_step`, `num_max_iter`, e dentro de
`ConvergenceCriterium`: `convergence_translation_tol=ERROR_TOLERANCE`,
`convergence_rotation_tol=ERROR_TOLERANCE*10`, `convergence_force_tol=ERROR_TOLERANCE*10`,
`convergence_moment_tol=ERROR_TOLERANCE*100`, `forces="yes"` sse `CRITERIUM≠DISP_ONLY`,
`use_max_force`/`max_b_abs_force_value`/`use_max_moment`/`max_b_abs_moment_value`. Todos derivados
diretamente de `cAnalysis`, mesmos campos que alimentam o `.aml` — **não há divergência de
conteúdo entre AML e XML aqui**, só de serialização. Já confirmado 1:1 com o Exemplo_01a real
(`CONVERGENCE_CRITERIUM='DISP_AND_FORCE'`, `MAX_UNBALANCED=1.0`) no documento do solver.

### Corrente: DAT inlina, XML referencia por ID

`save_currents_xml` (`10714-10761`) só grava `static_function_id`/`dynamic_function_id` — os
pontos reais da rampa ficam em `Functions/Function<n>/Points` (`save_functions_xml`,
`10625-10710`), como dado pesado no `.h5`. Reconstruir a rampa a partir do XML/H5 exige uma junção
por ID (ler `static_function_id` → achar `Function<n>` com esse `id` → ler os pontos no `.h5`) —
**não é um valor inline**. `save_current` (DAT, `4190-4285`) faz o oposto: resolve a função e
escreve os pontos **inline** na seção `$ FUNCAO TEMPO ASSOCIADA`. `risersim` já implementa a junção
correta (`xml_h5_reader.py::extract_current_ramp()`), confirmado lendo o código atual.

### Solo coupled/uncoupled: por-solo no XML, só o primeiro solo no DAT

XML (`save_soils_xml`, `10305-10363`): por `cSoil`, individualmente, grava
`<coupled>yes|no</coupled>` (**tag real confirmada lendo o XML de verdade do Exemplo_01a**,
`Exemplo_01a_A1.xml:790` — `<coupled>no</coupled>`, não `soil_model` como uma leitura só do
código-fonte sugeriria; o nome do atributo pode ter mudado entre a versão do `save-dat.cpp` atual
e a versão que gerou esse XML de referência, então **confiar no arquivo real, não só no código**),
mais `axial_friction`/`lateral_friction`/`axial_elastic_deflection_limit`/
`lateral_elastic_deflection_limit`/`vertical_stiffness` como floats diretos (confirmado,
`Exemplo_01a_A1.xml:784-800`).

DAT (`save_seabed`, `3088-3207` + `save_global`, `2259-2409`): os floats de atrito/deflexão são
equivalentes por solo (`DESEAX/DESELT/FCAX/FCLT`, `3183-3195`), mas o flag coupled/uncoupled **não
é por-solo** — é codificado só como o **sinal** de um único inteiro global `NBOT` em `save_global`
(`2267-2277`), usando **apenas o primeiro solo da coleção**, com o próprio comentário do código
confirmando a limitação (*"this variable will only [be] consulted [from] the first soil of the
list"*). Modelos com múltiplos solos de tipos diferentes não são representáveis no `.DAT` — mais
um motivo para XML ser a fonte de verdade.

## Lacunas encontradas no pipeline atual do `risersim`

| Lacuna | Afeta o Exemplo_01a? | Status |
|---|---|---|
| `xml_h5_reader.py` nunca extraía `axial_friction`/`lateral_friction`/`*_elastic_deflection_limit`/`coupled` do XML real — só um `friction_coeff` isotrópico | **Sim** — valores reais confirmados bem diferentes entre si (axial 0,92/0,03m vs. lateral 0,95/0,2779m) | **Corrigido nesta rodada** — ver `mapa_classes_anflex_estatica.md`, seção final, para o resultado do reteste |
| `StaticAnalysis::solve()` reusa o orçamento (`max_iter_per_step`/`tol`) da fase estática para a fase de assembly, em vez do orçamento próprio de `cAssembly` real (`MAX_ITERATION=20` default, tolerância/critério próprios); `xml_h5_reader.py` não extrai `AnalysisData/Assembly/...` nenhum | Não — `%ASSEMBLY.USING.FALSE` no Exemplo_01a, a fase nunca roda | Documentado, não corrigido — candidato a próximo passo se algum exemplo com assembly ligado precisar ser reproduzido |
| Extração de solo (`.//Soils/Solo/...`) sempre pega só o primeiro/único nó `Solo` — sem suporte a múltiplos solos | Não — Exemplo_01a só tem um solo | Aceitável por ora (replica a mesma limitação que o próprio `.DAT` real tem) |

## Ver também

- [`mapa_classes_anflex_estatica.md`](mapa_classes_anflex_estatica.md) — mapa do solver
  (`trunk/src`), a investigação original do bug de convergência solo+corrente.
