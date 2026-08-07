# Estudo aprofundado do AML/XML em exemplos variados + desenho de arquivo para a futura interface web

> Documento de referência. Continua [`mapa_classes_anflex_interface.md`](mapa_classes_anflex_interface.md)
> (que mapeou as classes reais de `trunk/interfaces/src`) olhando a diversidade dos ~30 exemplos
> `.aml` disponíveis em `trunk/exemplos/`, com dois objetivos: (1) procurar recursos do AML real
> ainda não mapeados que possam se relacionar ao bug de convergência solo+corrente ainda em aberto
> ([`mapa_classes_anflex_estatica.md`](mapa_classes_anflex_estatica.md)); (2) reunir os fatos para
> decidir o formato de arquivo de uma futura interface web de entrada de dados.

## Achado central: `aml_reader.py` produz um schema desconectado do `ModelBuilder` real

`risersim/tools/run_from_aml.py` escolhe entre dois caminhos: se existe uma pasta
`<nome>_analysis/` com `<nome>_A1.xml`+`.h5` já exportados pelo ANFLEX real, usa
`xml_h5_reader.py`; senão, cai no fallback `aml_reader.py`, que lê o `.aml` puro por conta própria.
Só **7 dos 30 exemplos** têm essa pasta exportada (Exemplo_01a, Exemplo_02a, Exemplo_03 Estática/
Dinâmica, Manifold, Sombra, SemSombra) — os outros 23 dependem do `aml_reader.py`.

`aml_reader.py` (605 linhas) faz um parse ingênuo linha-a-linha do texto (`_parse_file()`,
`aml_reader.py:44-72`): qualquer linha `%...` inicia uma tag, as seguintes até a próxima `%` são
seus dados — sem entender aninhamento real (`BEGIN`/`END`). Extrai global, um único solo (sempre o
**último** `%SOIL` do arquivo, não resolvido por ID), material (deriva E/G/A/I/J de EA/EI/GJ),
linhas múltiplas (`%LINE`/`%LINE.SEGMENT.MESH`/`.MATERIAL_ID`/`.BUOY_ID`), correntes e ondas
múltiplas, e parâmetros de análise. `to_risersim_config()` (`aml_reader.py:432-542`) monta tudo
isso num dict achatado: `{"beam_props", "geometry", "seabed", "offsets", "wave", "current",
"rayleigh", "simulation_options"}`.

**O problema**: `risersim::ModelBuilder::load_from_json()` (`risersim/src/model_builder.cpp:102-125`)
só sabe ler `j["model"]["nodes"]`/`j["model"]["elements"]` — não existe nenhum código ali que leia
`beam_props`/`geometry`/`seabed` do jeito que `aml_reader.py` os produz. Se essas chaves não
existirem (que é sempre o caso do output de `aml_reader.py`), o `if` correspondente simplesmente
não entra, e `model.nodes`/`model.elements` ficam vazios — **sem erro, sem exceção**. O
`ModelBuilder` cai então no modelo sintético de fallback (a catenária parabólica de 40 elementos).

Consequência: **hoje, rodar `run_from_aml.py` em qualquer um dos 23 exemplos sem pasta
`_analysis/` não simula o modelo real** — produz um JSON que o motor C++ ignora silenciosamente e
roda com um modelo sintético desconectado dos dados do `.aml`. `risersim/tools/js/services/
InputLoaderService.js:53-68` já documentava esse sintoma do lado do visualizador JS ("este arquivo
NÃO será reconhecido por risersim... cai silenciosamente no modelo sintético padrão"); a leitura do
C++ confirma que a mesma frase vale literalmente para o motor real, não só para o visualizador.
Não é uma lacuna de "faltam alguns campos" — é a função de conversão nunca ter sido religada ao
schema que o consumidor de fato espera.

**O que falta para religar** (não implementado nesta rodada): reescrever
`aml_reader.py::to_risersim_config()` para gerar a malha (`model.nodes`/`model.elements`) a partir
dos segmentos já extraídos (`%LINE.SEGMENT.MESH`), e resolver referências por ID (corrente/solo por
`%LOAD_CASE.CURRENT_ID`/`%LINE.SEGMENT.SOIL_ID`) do mesmo jeito que `xml_h5_reader.py` já faz
(`_find_chosen_current()`, `xml_h5_reader.py:262-294`) — hoje `aml_reader.py` sempre pega
`lines[0]`/`waves[0]`/`currents[0]` por ordem de aparição no arquivo, sem correlação nenhuma.

**Também não lidos por `aml_reader.py`**, mas já cobertos pelo caminho XML (via os dois helpers que
leem o `.aml` bruto como suplemento — não do XML — ver `mapa_classes_anflex_interface.md`):
`%ASSEMBLY.USING.TRUE/FALSE` (`extract_assembly_flag()`) e
`%ANALYSIS_CASE.STATIC.CONVERGENCE_CRITERIUM`/`.MAX_UNBALANCED` (`extract_static_convergence_criterium()`)
— justamente os dois campos mais centrais à investigação de convergência estática já feita.

## Tags `%` de topo escritas mas nunca lidas de volta

Censo dos 30 `.aml` (685 tags de raiz distintas no total) comparado contra a dispatch table real de
leitura (`interfaces/src/aml.cpp:105-178`, ~68 entradas). A maioria das tags "ausentes" do mapa de
leitura são, na verdade, aninhadas dentro de outro bloco (ex. `%TIME_SERIES_LOAD` dentro de
`%FLOATING.LOADS.*`) — confirmadas lendo o código, não são lacuna. Três famílias, porém, são
genuinamente órfãs: escritas pelo exportador atual (`ioSaveProject`/`anfout.cpp`, ver
`mapa_classes_anflex_interface.md`) mas **sem nenhum leitor**, nem no `fmap` nem aninhado:

| Tag órfã | Escrita em | Leitura | Situação |
|---|---|---|---|
| `%OPTION.BEGIN...END` (`.ELASTIC_DEFORMATION.*`, `.SOIL.COUPLED/UNCOUPLED`, `.BINARY_OUTPUT.*`, `.NODE_REORDER.*`) | Nenhum `fprintf` encontrado — puramente legado | Nenhuma (`cAml::readAML()` pula a linha silenciosamente, o bloco inteiro some) | Achado grave, ver seção seguinte |
| `%ANFLEX.SOLVER.VERSION`/`.RELEASE` | `anfout.cpp:564,566` | Ausente do `fmap` (só `ANFLEX.MULTILINE.VERSION`/`ANFLEX.PML.CREATION.*`/`ANFLEX.PRE_PROCESSOR.*` são lidos) | Metadado de versão, não deveria afetar física |
| `%OUTPUT.BEGIN` (`.PRINT.*` incl. `.PRINT.LIMIT_STATE` do DNV check, `.CURVE.*`, `.ENVELOPE.*`) | `output.cpp:3034` | `cOutput` não tem nenhum método de leitura para essas subtags (`%RESULTS.*`/`%TABS.*`, que estão no `fmap`, dispacham para outros objetos — resultados carregados, não configuração de impressão) | Configuração de impressão/relatório, não deveria afetar a física do solver |

## Achado grave: a migração `%OPTION.SOIL.COUPLED` → `%SOIL.COUPLED` está morta no código atual

O achado mais concreto desta rodada, com implicação direta sobre uma premissa já registrada em
[`mapa_classes_anflex_estatica.md`](mapa_classes_anflex_estatica.md). Existe uma variável estática
`s_temp_soil_coupled` (`interfaces/src/aml.cpp:65`, inicializada em `-1`) e uma função de migração
`cAml::fix_soil()` (`aml.cpp:3749-3760`, comentário original: `/* read from aml - old version. Set
to all soils. */`) que aplicaria esse valor a todos os solos do modelo **se** `s_temp_soil_coupled
>= 0`. Busca exaustiva em todo `interfaces/src` por qualquer atribuição a essa variável fora do
inicializador: **não encontrada**. A tag legada `%OPTION.SOIL.COUPLED`/`.UNCOUPLED` (dentro do
bloco órfão `%OPTION.*` acima) nunca é sequer lida (está fora do `fmap`), então nem chegaria a
popular essa variável de qualquer forma — o caminho de migração está morto em pelo menos dois
pontos independentes.

Confirmado também: **nenhum dos 30 exemplos usa a tag moderna `%SOIL.COUPLED`/`%SOIL.UNCOUPLED`**
(ausente do censo completo de 685 tags) — só a legada `%OPTION.SOIL.COUPLED` aparece, e só em
`exemplos/Sombra/EfeitoSombra.aml:245` (dentro de um bloco `%OPTION.BEGIN...END`,
`Sombra/EfeitoSombra.aml:241-249`, ausente em `SemSombra/EfeitoSombra.aml`). O construtor de
`cSoil` define `m_coupled = false` por padrão (`soil.cpp:112`).

**Conclusão**: com o código-fonte de `interfaces/src` disponível hoje, todo solo em todo exemplo
carregaria como `uncoupled`, independente da intenção do `%OPTION.SOIL.COUPLED` legado. Isso afeta
diretamente uma premissa já registrada — ver a nota de cautela adicionada em
`mapa_classes_anflex_estatica.md`.

## Exemplos estudados: recursos não suportados pelo `risersim` hoje

Seis arquivos lidos em profundidade (mais Boiao, por sugestão própria durante a pesquisa). Todos
muito maiores que o Exemplo_01a (1.270 a 16.794 linhas, a maior parte `%DISPLAY_SETTINGS`/`%CAMERA`).

### `Multilinhas/Multilinhas.aml` (10.719 linhas) — corpo flutuante compartilhado

7 linhas (`%LINE.ID`, `Multilinhas.aml:2772-2778`), todas presas ao **mesmo** `%FLOATING.SHIP`
(ID 2701, `Multilinhas.aml:2388-2492`) via 7 `%CONNECTION.BEGIN` distintos
(`Multilinhas.aml:2494-2708`, `%CONNECTION.LOCAL_COORDINATES` diferentes por linha) e
`%LINE.CONNECTION_ID` apontando para o ponto local correspondente. O acoplamento entre linhas não é
linha-linha direto — é um corpo rígido de 6 GDL compartilhado. `%SOIL` (1 bloco, ID 1) e `%CURRENT`
(8 blocos) presentes, só um `%LOAD_CASE.CURRENT_ID`=2785 usado
(`Multilinhas.aml:9007-9008`) — candidato a solo+corrente, mas correlação de ID do solo não
confirmada linha a linha nesta rodada. **`RiserModel` não tem conceito de "linha" nem de corpo
flutuante compartilhado** (`nodes`/`elements` são listas planas únicas por modelo, ver
`model.hpp:108-149`) — não suportado hoje.

### `Sombra/EfeitoSombra.aml` vs `SemSombra/EfeitoSombra.aml` — não são candidatos solo+corrente

`diff` completo entre os dois confirma que a diferença semântica central é mesmo
`%ANALYSIS_CASE.STATIC.WAKE_EFFECT` (só em Sombra, `Sombra/EfeitoSombra.aml:1089-1090`, tag
autodescritiva sem valor, mesmo padrão de `%ASSEMBLY.USING.TRUE/FALSE`) — nunca extraída pelo
`risersim` hoje. Achado que corrige a hipótese inicial desta investigação: **nem Sombra nem
SemSombra combinam solo real com corrente** — busca por `%SOIL` (sem sufixo) nos dois arquivos: zero
ocorrências. Existe sim `%LINE.SEGMENT.SOIL_ID=1` duas vezes em cada arquivo
(`Sombra/EfeitoSombra.aml:853,969`; `SemSombra/EfeitoSombra.aml:801,917`) — uma **referência
pendurada** a um solo nunca definido no arquivo (não achado em nenhum outro lugar, incluindo
`%GROUPS.BEGIN...END`, vazio nos dois). O par testa sombreamento de corrente sobre linha(s) sem
contato de solo modelado — descartado como candidato ao bug solo+corrente.

### `Turret/Turret.aml` (6.122 linhas) — movimento de topo 6-GDL por caso de carga

`%FLOATING.TURRET` (`Turret.aml:1279-1295`, corpo `'SETTEBELLO'`) — mastro rotativo de FPSO ancorado
por turret (weathervaning), separado do casco. Dentro de `%FLOATING.LOADS.STATIC`/`.DYNAMIC`
(`Turret.aml:1302-1708`), cada caso de carga tem seu próprio `%TIME_SERIES_LOAD` com
`.AMPLITUDES`/`.FUNCTIONS`/`.REFERENCE_SYSTEM 'LOCAL_FLOATING'`/`.ROTATION_ANGLE 'RODRIGUES'` — um
movimento prescrito 6-GDL por caso de carga bem mais rico que o `VesselOffset::Near/Far` do
`risersim` hoje (offset escalar simplificado, já documentado como lacuna em
`mapa_classes_anflex_estatica.md`). 9 linhas, `%SOIL`+`%CURRENT` presentes.

### `DNV/DNV_Check.aml` (1.270 linhas) — candidato solo+corrente mais forte

Título `'SCR'`. `%SOIL.ID 112` (linha 253, `'Solo_P8'`) casa **exatamente** com
`%LINE.SEGMENT.SOIL_ID` (`DNV_Check.aml:744-746`, formato "contagem + valores": `1` segmento, valor
`112`); `%CURRENT` presente (linha 824). "DNV check" = parâmetros de material para verificação de
código (`%MATERIAL.DNV.SAFETY_CLASS`/`.DESIGN_MINIMUM_PRESSURE`/`.OVALIZATION`/
`.MANUFACTURING_FACTOR`/`.YIELD_STRESS`/`.ULTIMATE_STRESS`, `DNV_Check.aml:455-471`), alimentando
`%OUTPUT.PRINT.LIMIT_STATE 1` (`DNV_Check.aml:1091-1092`) — um **pós-processamento** (relatório de
estado limite, provavelmente DNV-OS-F201) sobre os resultados já resolvidos, não altera a malha nem
a física do solver. **Candidato mais forte a um segundo caso de teste real solo+corrente**, estático
— mas sem pasta `_analysis/` exportada, não testável no pipeline hoje sem religar `aml_reader.py`.

### `Ruptura/Ruptura.aml` (10.266 linhas) — ruptura de elemento, só dinâmico

`%ANALYSIS_CASE.TYPE 'time_domain'` — não tem caminho estático. `%SOIL.ID 1266` (`'Solo_EG12'`)
casa com `%LINE.SEGMENT.SOIL_ID` (`Ruptura.aml:9163-9166`, mesmo formato contagem+valores),
`%CURRENT` presente — terceiro candidato solo+corrente, mas descartado para testar o bug **estático**
por não ter caminho estático nenhum. "Ruptura" = `%LOAD_CASE.DYNAMIC.RUPTURE.ELEMENT`
(`'EG0070060'`) + `.RUPTURE.TIME` (`t=14.5s`, `Ruptura.aml:9809-9815`) — remoção de elemento durante
a integração no tempo, um tipo de não-linearidade topológica **exclusivamente dinâmica**, não
mapeada em nenhum documento anterior. Não suportado pelo `risersim` hoje (não encontrado nenhum
conceito de remoção/ruptura de elemento em `risersim/include`/`risersim/src`).

### `Boiao/P52_Boiao.aml` (16.794 linhas) — caso de produção real, solo multi-zona

Título `'P52 - Bóia para 2800m'`. 32 linhas, 89 nós diretos, **3 blocos `%SOIL` distintos** (IDs
112/113/114, solos espacialmente diferentes — não um solo uniforme como o Exemplo_01a), 2
`%CURRENT`, `%FLOATING.PLATFORM` (mesmo padrão de `%TIME_SERIES_LOAD` do Turret). Achado mais
relevante: cruzando `%LINE.SEGMENT.SOIL_ID` das 32 linhas, várias têm 2-3 segmentos com IDs de solo
diferentes **na mesma linha** (`P52_Boiao.aml:12064-13034`) — uma única linha atravessando zonas de
solo diferentes ao longo do próprio comprimento. **Nem `aml_reader.py` nem `xml_h5_reader.py`
correlacionam `SOIL_ID` por segmento com o bloco `%SOIL` correspondente hoje** — ambos assumem
implicitamente um único solo global (confirmado: `xml_h5_reader.py` usa `.//Soils/Solo/...`, um
XPath que sempre pega o primeiro/único nó `Solo`, ver `mapa_classes_anflex_interface.md`).

## Decisão de arquitetura: JSON de interface vs. JSON de simulação

Pergunta original: o arquivo salvo por uma futura interface web de entrada de dados pode ser "o
mesmo" que já é enviado para a simulação? A resposta, refinada com a pergunta certa que o próprio
usuário levantou, não é "AML vs. JSON" — é **um JSON só vs. dois JSONs**, espelhando o padrão real
AML→XML do ANFLEX.

### Por que dois, não um

- O JSON que `risersim::ModelBuilder` consome hoje (`model.nodes`/`model.elements` já **malhados**
  em coordenadas 3D concretas, valores ambientais já **resolvidos** — atrito axial/lateral direto,
  perfil de corrente já em profundidade×velocidade×ângulo) é estruturalmente equivalente ao
  **XML/H5** do ANFLEX real, não ao `.aml`. É a ponta "pré-mastigada" da cadeia real — a mesma
  malha final que o solver de fato itera, não o modelo lógico editável.
- O `risersim` já tem, na prática, **dois "compiladores" paralelos** que produzem esse mesmo JSON de
  simulação a partir de fontes bem diferentes: `xml_h5_reader.py` (a partir do XML+H5 real,
  malhando e resolvendo referências) e `aml_reader.py` (a partir do `.aml` puro — hoje desconectado
  do schema real, ver achado central acima, mas com a mesma intenção). Uma interface web de
  entrada de dados seria naturalmente um **terceiro compilador**: em vez de ler XML/H5 ou `.aml`,
  leria os dados digitados pelo usuário num formato mais próximo do modelo lógico do `cAppModel`
  real (linha + segmentos, cada um referenciando material/solo/corrente **por nome**, não já
  resolvido em coordenadas) e produziria o mesmo JSON de simulação como saída.
- Manter essa separação evita qualquer mudança no C++ (`ModelBuilder`/`Simulation`, o consumidor já
  estável e testado, 343+ asserts) e reaproveita toda a validação/aviso de valor-default já
  construída em torno do formato de simulação (`model_builder.cpp`'s `value_warn`,
  `InputLoaderService.js`) — o "compilador" da interface web só precisaria emitir o mesmo schema
  que os outros dois já emitem, não um schema novo que o C++ precisaria aprender a ler.

### Recomendação (não implementada — para quando a interface for de fato desenhada)

- **JSON de simulação**: continua sendo o schema atual, sem mudança nenhuma
  (`model.nodes`/`model.elements`, `environmental`, `analysis_options`) — o único formato que o C++
  precisa entender, hoje alimentado por dois caminhos (XML/H5 real, AML puro) e futuramente por um
  terceiro (interface web).
- **JSON de interface**: um formato ainda a desenhar, mais próximo do modelo lógico real (linha com
  segmentos referenciando material/solo/corrente por nome/label, como `cAppModel` real faz) —
  natural para o usuário preencher incrementalmente, sem precisar entender malha ou resolução de
  referência.
- **Compilador**: provavelmente JavaScript, rodando no próprio navegador junto ao formulário (sem
  round-trip a um backend) — gera o JSON de simulação a partir do JSON de interface no momento de
  salvar, mesmo papel que `xml_h5_reader.py`/`aml_reader.py` já desempenham hoje.

### Ordem de dependência de dados (para o desenho futuro do formulário)

Confirmada lendo a ordem real de gravação do ANFLEX (`ioSaveProject`/`anfout.cpp:592-620`, ver
`mapa_classes_anflex_interface.md`) — um formulário em etapas precisaria seguir a mesma ordem de
catálogos-base-antes-de-referenciadores:

```
solo, função (rampa), material  →  linha (referencia os três)  →  corrente (referencia função)
nós/conexões  →  linha/malha  →  condições de contorno
análise  →  caso de carga (referencia análise)
```

### Nota de escopo

Mesmo com essa arquitetura resolvida, o motor C++ do `risersim` hoje só cobre uma fração pequena do
ANFLEX real: **uma linha só** (`RiserModel::nodes`/`elements` são listas planas únicas, sem conceito
de `Line`, confirmado em `model.hpp:108-149`), **um tipo de elemento genérico**
(`CorotationalBeam3D`, sem as ~12 subclasses de `MATERIAL.*` do ANFLEX real — conector, drilljoint,
flexjoint, rigid joint/tube, stiffener, stress joint, multilayer), **sem boias/tendões** como
entidade própria (existe `BuoyancyModule`/`BendRestrictor`, mas são modificadores locais de um
elemento existente, não lidos por `ModelBuilder::load_from_json` — confirmado por grep, só
acessíveis via binding Python direto), **sem casos de carga múltiplos**, **sem ruptura de
elemento**. O "JSON de interface" (e o formulário que o preencheria) precisaria nascer com escopo
igualmente restrito — uma linha, sem boias/tendões/turret/ruptura — e crescer só se/quando o motor
crescer. Isso não muda a decisão de arquitetura (dois JSONs continua certo mesmo num escopo
pequeno), só limita o que um primeiro formulário poderia cobrir.

## Ver também

- [`mapa_classes_anflex_interface.md`](mapa_classes_anflex_interface.md) — mapa das classes reais
  da interface gráfica (`cAppModel`, `cAml`, `save-dat.cpp`).
- [`mapa_classes_anflex_estatica.md`](mapa_classes_anflex_estatica.md) — investigação do solver e
  do bug de convergência solo+corrente ainda em aberto (contém a nota de cautela sobre Exemplo_02a
  motivada por este documento).
