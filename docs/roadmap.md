# Roadmap do `risersim`

> Documento de planejamento, não de investigação (diferente dos três `mapa_*.md`). Consolida os
> seis objetivos que o usuário definiu em 2026-08-07 — resolver o bug da estática, rodar a análise
> dinâmica, implementar itens faltantes, interface de entrada de dados, interface de controle de
> simulação (projetos/disparo/acompanhamento) e pós-processamento — em eixos com dependências e uma
> ordem sugerida. Não é uma decisão fechada; serve de base para decidir, um eixo de cada vez, o que
> planejar em detalhe a seguir.

## Eixo 1 — Confiabilidade do motor (bloqueante para qualquer resultado em que se possa confiar)

### 1a. Fechar o bug estático solo+corrente

Já exaustivamente investigado em [`mapa_classes_anflex_estatica.md`](mapa_classes_anflex_estatica.md).
Causa isolada com precisão (chattering de contato+atrito sob carga lateral, "parede" repentina por
volta da iteração 20-30, várias correções de fidelidade já aplicadas empurram o limiar mas não
fecham o gap). Três técnicas identificadas e **ainda não implementadas**:

1. Line search direcional (tipo Armijo, usando derivada direcional em vez de só a norma do
   resíduo) — ataca a causa mais diretamente, mas é a mais complexa de implementar corretamente.
2. Limitar o passo por nó/GDL em vez de um fator escalar global — mais simples que Armijo, pode
   conter oscilações localizadas sem afetar nós bem-comportados.
3. Suavizar a transição liga/desliga do contato vertical (hoje só a *penetração* é suavizada, o
   cruzamento `pen>0`/`pen<=0` continua uma chave dura) — ataca a descontinuidade na raiz, mas
   muda o comportamento físico do contato, exige validação cuidadosa.

**Recomendação**: testar a técnica escolhida primeiro no caso isolado de 33 elementos
(`risersim_diag_isolated_segment`, o caso mais pequeno e determinístico já isolado) antes do
Exemplo_01a completo — ciclo de iteração muito mais rápido.

### 1b. Investigar a análise dinâmica

Nunca recebeu o mesmo nível de investigação que a estática. O que já se sabe: não converge em
vários passos mesmo no modelo sintético simples (sem solo/corrente reais) — confirmado como
problema pré-existente, não uma regressão introduzida por nenhuma mudança já feita. Fidelidade
conhecidamente mais baixa que a estática num ponto específico: o movimento prescrito do topo em
`DynamicAnalysis` (onda harmônica) ainda usa a técnica direta antiga (`top_node->disp = ...` fora
do sistema, nó excluído do loop de Newton) — a técnica de penalidade (`PrescribedMotion`) só foi
migrada para a estática (Passo 7 do roadmap de modernização, já concluído).

**Recomendação**: aplicar a mesma metodologia que funcionou para a estática — isolar um caso mínimo
(malha sintética pequena, sem solo/corrente), comparar diretamente contra `cDynamicAnalysis`/
`cDynamicIntegrator` do ANFLEX real (`trunk/src`), buscando lacunas de fidelidade da mesma forma
que se achou a rotação total-vs-local na estática. Não depende de 1a estar fechado (o modelo
sintético já converge na estática) — pode rodar em paralelo.

## Eixo 2 — Pipeline de dados (desbloqueia mais casos de teste reais, baixo risco)

### 2a. Religar `aml_reader.py` ao schema real do `ModelBuilder`

Achado em [`mapa_aml_exemplos_e_web_interface.md`](mapa_aml_exemplos_e_web_interface.md): hoje, 23
dos 30 exemplos disponíveis (todos sem pasta `_analysis/` com XML/H5 pré-exportado) rodam
silenciosamente com o modelo sintético de fallback — `aml_reader.py` produz um schema que
`ModelBuilder` nunca entende. Consertar isso (gerar `model.nodes`/`model.elements` a partir dos
segmentos já extraídos, resolver corrente/solo por ID como `xml_h5_reader.py` já faz) é um trabalho
bem-escopado e de baixo risco (não toca o C++, só o parser Python), e destrava candidatos reais
novos ao bug solo+corrente — em especial `DNV_Check.aml` (solo e corrente confirmados casando por
ID, caminho estático).

### 2b. Suporte a múltiplas zonas de solo por segmento (opcional, avaliar sob demanda)

Achado no `Boiao/P52_Boiao.aml` (uma linha atravessando 3 solos diferentes ao longo do próprio
comprimento) — nem `aml_reader.py` nem `xml_h5_reader.py` correlacionam isso hoje, ambos assumem
solo único global. Só vale a pena se um caso de teste real desse tipo entrar em uso.

## Eixo 3 — Interfaces (podem começar em paralelo aos eixos 1-2, escopadas ao que o motor já suporta)

### 3a. Interface de entrada de dados

Arquitetura já desenhada em `mapa_aml_exemplos_e_web_interface.md`: dois JSONs (um "de interface",
próximo ao modelo lógico — linha com segmentos referenciando material/solo/corrente por nome; um
"de simulação", o schema atual e inalterado que `ModelBuilder` já lê), com um compilador JS
rodando no navegador. Falta: desenhar o formato concreto do JSON de interface, construir o
formulário (reaproveitando a casca de UI já existente — `TabPanel`, `PanelResizer`, `ThemeToggle`,
toolbar de câmera 3D — hoje só usada para visualização, nunca para edição), escrever o compilador.
Escopo inicial deve ficar restrito ao que o motor já resolve hoje: uma linha só, um material de
seção genérico, sem boias/tendões/turret/ruptura (ver Eixo 5).

### 3b. Interface de controle de simulação (projetos, disparo, acompanhamento)

A peça arquitetural mais nova de todo o roadmap — hoje não existe nenhum backend real, só
`tools/dev_server.py` (servidor de arquivos estáticos) e execução manual do binário C++ via
Docker/CLI. Precisa de desenho próprio antes de qualquer código, cobrindo pelo menos:
- Onde e como um "projeto" (modelo + histórico de rodadas) é persistido — sistema de arquivos é
  provavelmente suficiente para começar, sem precisar de banco de dados.
- Como uma simulação é de fato disparada a partir do navegador — precisa de um serviço/API por trás
  (o navegador não pode invocar `risersim_test_main` diretamente); decidir se roda local, em
  Docker, ou outro lugar.
- Como o progresso é acompanhado em tempo real — o solver já imprime por iteração/passo no stdout
  (`std::cout` com `std::endl`, ver `static_analysis.cpp`), então captura/streaming desse output
  (polling de arquivo de log ou algo como Server-Sent Events) é o caminho mais direto, sem precisar
  instrumentar o C++ para emitir eventos estruturados.

**Recomendação**: tratar como uma rodada de planejamento própria (Explore + Plan) quando chegar a
vez, dado que introduz infraestrutura nova (backend) que hoje não existe.

### 3c. Pós-processamento

Já existe uma base sólida (`posprocessor.html`, viewer 3D, gráficos Plotly). O trabalho principal
aqui é integrar com o conceito de "projeto/rodada" do item 3b (carregar resultados de uma rodada
específica do histórico, em vez de sempre apontar para um arquivo fixo) — depende de 3b existir
primeiro para fazer sentido completo, mas a base de visualização já não precisa de retrabalho.

## Backlog de recursos faltantes do motor (não bloqueante — puxar sob demanda)

Achados documentados em `mapa_aml_exemplos_e_web_interface.md`, nenhum implementado: múltiplas
linhas com corpo flutuante compartilhado (`Multilinhas.aml`), boias/tendões como entidade própria
(hoje só existe `BuoyancyModule`/`BendRestrictor`, modificadores locais não lidos por
`ModelBuilder::load_from_json`), conexões articuladas tipo flexjoint/drilljoint, turret com
movimento prescrito 6-GDL por caso de carga (`Turret.aml`), ruptura de elemento em tempo de
execução dinâmico (`Ruptura.aml`), verificação de código DNV como pós-processamento
(`DNV_Check.aml`). Cada um só vale a pena quando um caso de teste real concreto precisar dele — não
faz sentido implementar especulativamente.

## Ordem sugerida (não é obrigatória — ponto de partida pra decidir)

1. **1a** (bug estático) — é o item de maior incerteza e mais tempo já investido; fechar destrava
   confiança em qualquer resultado real do motor.
2. **2a** (religar `aml_reader.py`) — barato, baixo risco, roda em paralelo a 1a, e abre mais um
   caso de teste real (`DNV_Check`) que pode ajudar a validar 1a quando fechado.
3. **1b** (dinâmica) — pode começar em paralelo a 1a/2a (não depende de nenhum dos dois no caso
   sintético), mas convergir a estática primeiro dá uma base mais sólida pra rodar dinâmica em
   cima de modelos reais depois.
4. **3a** (entrada de dados) — pode começar o desenho/construção em paralelo a tudo acima, já que é
   front-end puro e não depende do motor mudar; só precisa ficar escopado ao que o motor já resolve.
5. **3b** (controle de simulação) — a peça que mais se beneficia de vir depois, já que introduz
   infraestrutura nova; faz mais sentido desenhar quando já houver mais clareza sobre 1a/1b (o que
   "acompanhar uma simulação" precisa mostrar depende de quão bem-comportado o solver já está).
6. **3c** (pós-processamento) — reboca 3b.
7. **Backlog de recursos faltantes** — sob demanda, conforme cada item vire necessário pra um caso
   de teste real específico.

## Ver também

- [`mapa_classes_anflex_estatica.md`](mapa_classes_anflex_estatica.md) — investigação do solver e
  do bug de convergência solo+corrente (Eixo 1a).
- [`mapa_classes_anflex_interface.md`](mapa_classes_anflex_interface.md) — mapa da interface
  gráfica real do ANFLEX (base para o Eixo 3a).
- [`mapa_aml_exemplos_e_web_interface.md`](mapa_aml_exemplos_e_web_interface.md) — censo de
  exemplos, lacuna do `aml_reader.py` (Eixo 2a), e arquitetura de dois JSONs (Eixo 3a).
