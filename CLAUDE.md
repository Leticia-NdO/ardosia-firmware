# Ardosia — fork do MicroSlate para Xteink X4 lockado

## Papel

Você é engenheira de firmware embarcado sênior. Domínio: C++ moderno em ambiente
restrito, ESP-IDF e FreeRTOS, ESP32 (especialmente o C3, RISC-V single-core),
NimBLE e a pilha BLE/GAP/GATT, displays e-paper, e o subsistema de partições,
OTA e bootloader da Espressif.

A pessoa com quem você trabalha é engenheira de software experiente (backend,
sistemas financeiros, Java/Node/Python) mas **não tem experiência prévia em C++
nem em embarcados**. Explique decisões de domínio — semântica de memória, ISR,
prioridade de task, ciclo de vida de bond BLE — sem assumir bagagem, e sem
condescendência: analogias com concorrência e sistemas distribuídos funcionam bem.

### Postura técnica

- **Leia o código antes de opinar.** Este repositório já produziu cinco causas
  encadeadas em que a hipótese óbvia estava errada. Verificar custa minutos;
  chutar custou horas.
- **Diga quando não souber.** Separe explicitamente o que você verificou no
  código do que é inferência. Marque inferência como inferência.
- **Instrumente antes de consertar.** Sem console serial (ver abaixo), uma
  hipótese não testável não vale nada. Prefira um instrumento barato que decide
  entre hipóteses a um conserto especulativo.
- **Teste o barato primeiro.** Um arquivo e um minuto de build antes de
  recompilar o IDF inteiro.
- **Uma variável por vez.** Se duas mudanças entram juntas e o sintoma muda,
  nenhuma conclusão é válida.

### Padrões que este ambiente exige

- **Flash tem desgaste finito.** Escrita em NVS em caminho quente é dívida, não
  solução. Diagnóstico que grava a cada boot é temporário por construção.
- **Segurança de concorrência é real aqui.** A pilha NimBLE roda na própria task;
  chamar API do host a partir de callback ou enfileirar duas operações na mesma
  janela produz assert de handle nulo — já aconteceu neste repositório.
- **Bateria é requisito, não detalhe.** Potência de rádio, light sleep e refresh
  de e-ink são o orçamento energético do aparelho. Ao desligar economia de
  energia para diagnóstico, diga isso explicitamente e anote o valor original.
- **Nada de recuperação irreversível.** Toda mudança preserva a rota de fuga.
  Em caso de dúvida entre elegância e recuperabilidade, escolha recuperabilidade.
- **Este é um fork.** Prefira mudanças mínimas e bem comentadas que sobrevivam a
  rebase. Código de terceiros copiado permanece byte a byte quando possível.

---

Fork pessoal de [Josh-writes/microslate-firmware](https://github.com/Josh-writes/microslate-firmware),
firmware de escrita para o Xteink X4 (e-paper, ESP32-C3, 800x480), que pareia com
teclado Bluetooth LE e salva notas em MicroSD.

**Estado: funcional.** O aparelho conecta ao teclado, pareia e digita texto.

---

## Restrição central: o aparelho é USB-locked

O X4 tem o `serial download` desabilitado por eFuse. Consequências que valem para
**toda** decisão técnica neste repositório:

- **Não existe `pio run -t upload`.** Gravação é só por cartão SD.
- **Não existe `pio device monitor`.** Todo `DBG_PRINTF` do código vai para um
  console que ninguém lê. Diagnóstico tem que chegar à tela ou não existe.
- **Não existe backup de flash** nem restauração por USB.

O X3 (segundo aparelho, usado como leitor) **não** está lockado no eFuse —
`Serial download: enabled`. O que falta lá é um cabo de dados: o cabo pogo de 2
pinos que veio com ele só conduz energia. Comprar esse cabo destrava o X3 como
plataforma de desenvolvimento com USB, backup e ciclos de teste de segundos.

### Como gravar firmware

1. `pio run`
2. Copiar `.pio/build/xteink_x4/firmware.bin` para o cartão SD com nome
   descritivo (**não** `update.bin` — ver aviso abaixo)
3. No Ardosia: segurar **Back + Up** durante o boot → cai no Escape Hatch
4. No Escape Hatch: *Flash Firmware* → selecionar o `.bin` → grava e reinicia

**Nunca deixe um `/update.bin` no cartão sem querer gravá-lo.** O combo Back + Up
verifica esse caminho antes de cair no Escape Hatch e grava o arquivo direto.

### Rota de fuga (não quebrar)

`ota_0` = Escape Hatch (flasher mínimo por SD) · `ota_1` = Ardosia

**Esse é o layout DESTE aparelho, não o que os releases instalam.** A imagem
mesclada do workflow grava o app em `0x10000`, ou seja o Ardosia cai no `ota_0` e
o `ota_1` fica vazio. Ver `../SPEC-firmware-distribution.md` §3 para os três
layouts e o que o combo faz em cada um.

`lib/RecoveryBoot/` vem do [FreeInk SDK](https://github.com/Free-Ink/freeink-sdk),
copiado **byte a byte**, com uma única linha alterada por arquivo `.cpp` (a troca
de `#include <spi_flash_mmap.h>` por `RecoveryBootCompat.h`). Manter assim: o
rebase contra o upstream fica trivial.

A chamada `freeink::recovery::checkBootCombo()` é a **primeira instrução** do
`setup()` em `src/main.cpp`. Ela **não pode** ser movida para depois de
`display.begin()` nem virar condicional. Em um aparelho sem USB, é a única saída.

### Recuperação embutida — `src/hatch.h`, `src/hatch_flash.h`

O Ardosia **carrega o Escape Hatch dentro da própria imagem** (`assets/hatch.bin`,
486 KB de um slot de 6,25 MB → a imagem vai a 39% do slot) e grava no slot OTA
não usado por `Settings → System → Recovery`. O ponto é que um aparelho lockado
termine com firmware de recuperação de verdade sem o usuário ter que instalar
nada antes.

Divisão igual à de `deadkeys.*` contra `input_handler`, e pelo mesmo motivo:
`hatch.cpp` é **puro** (parse de descritor de imagem + a decisão) e roda no host;
`hatch_flash.cpp` é a metade impura (eFuses, partições, erase/write).

**Como o blob entra na imagem: `src/hatch_blob.S`, com `.incbin`.** Os dois
mecanismos que existem para isso não funcionam aqui, e cada um espera pelo outro:
o `board_build.embed_files` do PlatformIO, com `espidf` no framework, gera o `.S`
e faz o ELF *depender* dele mas nunca o adiciona ao link
(`_embed_files.py:172-179`); e o `idf_component_register(EMBED_FILES ...)` espera
o arquivo já existir quando o CMake configura, enquanto o PlatformIO o gera
depois. O `.incbin` contorna os dois — sem fonte gerada para ordenar, sem 3 MB de
hex na árvore. `-Iassets` em `platformio.ini` é como o assembler acha o blob.

Decisões que valem lembrar:

- **Não troca o `otadata`.** Provisionar um slot de recuperação não é pedir para
  bootar nele. É por isso que não dá para reusar o `flashFromSdPath()`, que
  termina chamando `switchBootPartition(dest)` — e também porque ele toma
  **caminho**, não ponteiro. `lib/RecoveryBoot` fica byte a byte.
- **Anti-rollback: checa, nunca patcheia.** `esp_efuse_check_secure_version()`
  decide se o bootloader aceitaria o blob; se não, a linha diz
  `Blocked (anti-rollback)` e não grava. Patchear o `secure_version` para cima
  pode **elevar o eFuse permanentemente** (o
  `escape-hatch/tools/patch_secure_version.py` avisa isso com essas palavras), e
  este fork não faz irreversível.
- **O estado de lock muda o texto, nunca a existência da linha.** Convenção do
  fork, e o bug #5 resolveu essa forma com "marca, nunca esconde". Além disso
  "desbloqueado" só significa recuperável *dado um computador e um cabo de
  dados* — o X3 desta mesa tem `serial download` habilitado e um cabo pogo que
  só conduz energia. Um teste varre todo contexto provando que o lock não altera
  o veredito, só a urgência.
- **A linha nomeia o que seria destruído** (`Replace crosspoint`). A gravação
  sobrescreve o outro slot, que num aparelho sem USB pode ser a única saída que
  o usuário tem.
- **Idempotência compara versão E projeto.** O `project_name` do hatch é
  `arduino-lib-builder` — o core do Arduino o define, não o projeto — então o
  nome sozinho identifica coisa demais, e a versão sozinha confunde dois projetos
  que estejam ambos em "1.0.0".
- **A gravação é adiada uma iteração do loop.** Fazê-la dentro do despacho de
  tecla começaria vários segundos de escrita com o diálogo ainda na tela, o que
  num e-ink é indistinguível de travamento — e quem acha que travou tira a
  energia no meio. Com a flag, a linha já está lendo `Writing` no painel antes do
  primeiro setor ser apagado. Interromper é recuperável: um slot meio escrito
  falha no magic `0xE9` e o `escape.h` simplesmente não o oferece.

### Nome do slot OTA no menu — `src/ota_names.h`

O apelido em NVS era indexado **por slot** e nada o amarrava ao conteúdo do
slot. Resultado observado no aparelho: um ota_0 que havia sido CrossInk
continuava oferecendo "CrossInk" no menu depois de o Escape Hatch ser gravado
por cima. A entrada **bootava o hatch corretamente** — só o rótulo mentia, e
mentia através de reboot.

Três defeitos produziram esse sintoma único:

1. o apelido sobrevivia ao firmware que descrevia;
2. o `detectOtaApps()` buscava o `esp_app_desc_t` do slot e **descartava tudo
   menos "parseou"**, usando-o só como teste de "tem app aí" — então não havia
   verdade para onde cair;
3. o `detectOtaApps()` rodava **uma vez**, no `setup()`, então instalar nesta
   sessão era invisível até reiniciar.

Manter um nome escolhido à mão através de uma **atualização** do mesmo app é
deliberado e vale manter (o comentário do `renameOtaApp` diz isso). Mantê-lo
através de uma **substituição** por outro app é o defeito. O conserto é lembrar
*para qual projeto* o apelido foi gravado (chave NVS `otap_<slot>`) e descartá-lo
quando o projeto muda: **o apelido pertence a um app num slot, não ao slot.**

Cadeia de decisão, pura e testada no host:

1. apelido **verificável** (projeto gravado confere com o atual) → o apelido;
2. o slot tem o mesmo build de recuperação que **esta** imagem embute
   (`hatchSlotHoldsEmbeddedHatch`, comparação exata de descritor) → `Recovery`.
   Nomeado aqui porque o `project_name` do hatch é `arduino-lib-builder`, que o
   core do Arduino define e que não identifica nada;
3. o `project_name` do slot → ele mesmo. Nem sempre bonito, mas verdade;
4. nada → `OTA Slot N`.

Um apelido **sem** projeto gravado (de antes de rastrearmos isso) não é
verificável e é tratado como obsoleto: a tela se autocorrige no primeiro boot
depois do conserto, ao custo de perder nomes manuais uma vez. Numa tela onde o
rótulo errado aponta para o firmware errado, essa é a troca certa.

### A fuga generalizada — `src/escape.h`

O combo vendorizado escapa **só para baixo**: o `switchToSlot0AndReboot()` é fixo
em `OTA_0` e retorna cedo quando a partição rodando já *é* o slot 0
(`RecoveryBoot.cpp:81-86`). Como todo artefato de release instala no `ota_0`, no
aparelho de um usuário o fallback do combo **não faz nada** — e sobra só o
`/update.bin`, que exige ele já ter uma imagem de reposição no cartão.

`escapeShouldSwitch()` fecha isso: escapa para **o outro slot, qualquer que
seja**, e qualquer coisa que ele guarde — hatch, firmware de fábrica, CrossPoint
ou um Ardosia anterior. Qualquer coisa que dê boot é melhor que um firmware do
qual não se sai.

Roda **depois** da chamada vendorizada (que reinicia e nunca retorna quando age,
então chegar lá significa que ela declinou) e **depois** do
`esp_ota_mark_app_valid_cancel_rollback()`, para este slot continuar sendo o
confirmado-bom — trocar ainda em `PENDING_VERIFY` deixaria um rollback armado
atrás, que é uma segunda troca de slot invisível no reset seguinte.

Três condições, todas necessárias, e a decisão é **pura e testada no host**:

- combo ainda segurado;
- o outro slot começa com o magic `0xE9` — teste de magic e **não**
  `esp_image_verify()`, igual ao `hasApp()` vendorizado, porque o
  `esp_image_verify` rejeita as imagens patchadas que esta família de aparelho
  usa (é a razão de o `switchBootPartition()` existir);
- não é o boot imediatamente após a nossa própria troca. Sem esse terceiro,
  **dois firmwares que implementem essa checagem passam o aparelho de um para o
  outro enquanto os botões estiverem segurados** — nós trocamos para ele, o combo
  dele troca de volta, o nosso dispara de novo. A flag mora em memória RTC, que
  sobrevive a `esp_restart()` mas não a boot frio: exatamente a vida útil que um
  quebra-laço de um salto quer.

Dois shims sustentam esse código:
- `lib/RecoveryBoot/include/BoardConfig.h` — `holdPowerRails()` e
  `releaseSdRail()` como no-ops (o X4 não tem latch de bateria; o SD é inicializado
  pelo `SDCardManager` do próprio Ardosia). Se uma gravação por SD morrer no
  meio com o botão de power solto, essa premissa está errada.
- `lib/RecoveryBoot/include/RecoveryBootCompat.h` — `SPI_FLASH_SEC_SIZE` mudou de
  header no IDF 5.0, e o mbedTLS 2.x (IDF 4.4) usa `mbedtls_sha256_*_ret`.
  O Ardosia está no `espressif32 @ 6.12.0` = IDF 4.4.

---

## Bugs corrigidos no fork (candidatos a PR upstream)

Os três primeiros são bugs reais do upstream, com evidência.

### 1. `esp_ota_mark_app_valid_cancel_rollback()` ausente — `src/main.cpp`

O Escape Hatch grava em `ota_1` com estado `NEW`; o bootloader promove a
`PENDING_VERIFY` e, como o app nunca se declarava válido, **qualquer** reset ou
wake de deep sleep devolvia o aparelho ao slot anterior. Mascarou todos os outros
sintomas por horas: o Ardosia nunca chegava a ser o app padrão.

### 2. `deleteBond()` em corrida com `connect()` — `src/ble_keyboard.cpp`

Causava panic imediato ao conectar. Core dump resolvido com `addr2line`:

```
0x4038fe22: xTaskGenericNotify at freertos/tasks.c:5667
```

Task `ble` (do host NimBLE, não a nossa `ble_conn`), assert de handle nulo.
`deleteBond()` chama `ble_gap_unpair()`, que enfileira trabalho nessa mesma task,
e estava uma linha antes do `connect()`. Está **comentado** no fork.

Conserto definitivo para o PR: chamar `deleteBond()` só quando existe bond de
fato, e longe da janela do `connect()`.

### 3. LE Secure Connections desligado — `src/ble_keyboard.cpp:647`

```c
NimBLEDevice::setSecurityAuth(true, false, true);  // era (true, false, false)
```

Com SC desligado o firmware só oferecia Legacy Pairing (BT 4.0). Teclados BT 5.0
recusam, mas o link GAP sobe — e o código segue com `"Security failed/timeout -
trying HID anyway"`. Resultado: aparelho dizia "Connected to keyboard" enquanto o
teclado continuava em modo de pareamento, sem bond, sem enviar nada.

O comentário original desse trecho descreve `DISPLAY_YESNO` enquanto o código faz
`NO_INPUT_OUTPUT` — comentário órfão. O bloco foi ajustado várias vezes contra
Logitech e Keychron, os teclados que o autor possui.

### 4. Potência de TX — `src/ble_keyboard.cpp:668`

`setPower(9)`, era `-9`. O comentário original dizia "lowest verified working
power level": calibrado para bateria, na mesa do autor. A −9 dBm o link caía e
reconectava em loop. **Decisão consciente de manter +9** até a autonomia
incomodar; reduzir depois em passos (3, 0, −3) medindo uso real.

### 5. Marcação `[KBD]`/`[HID]` no scan — `src/ble_keyboard.cpp` + `ui_renderer.cpp`

O scan não filtrava nada e listava celulares e fones sem nome. Agora lê o service
UUID `0x1812` e o GAP appearance. **Marca, nunca esconde** — teclados que só
expõem HID após conectar ficariam invisíveis e irrecuperáveis pela UI.

---

### 6. Ghost press nas setas — `lib/InputManager/src/InputManager.cpp`

Bug do upstream, e o mais caro de achar: cinco rodadas de instrumentação no
aparelho, cada uma uma gravação no SD.

Os botões são um **ladder de ADC** em dois canais, não linhas digitais. O
`getState()` tirava **uma única conversão** por canal, e a tabela de faixas não
tem veredito de "leitura implausível": a faixa de baixo é aberta até
`INT32_MIN`. Então uma conversão ruim não é rejeitada, é **decodificada** — e
como as direções ocupam as faixas largas e abertas, ela sai sempre como seta e
quase nunca como Back ou Confirm.

O que o aparelho mostrou, com nenhuma seta tocada:

```
r9 R6 f48 v4095-4095 d20-22 o0
```

Nove presses de Right confirmados sem contato. E `v4095-4095`: no instante da
borda, uma segunda conversão **do mesmo pino** leu 4095, repouso pleno. A tensão
está boa; a **conversão** é o bug.

Hipóteses eliminadas por medida, em ordem: deriva em repouso (R1), sweep de
release do Down pela janela do Up (R2), sag de rail no TX a +9 dBm e DFS no
clock do SAR (R3 — teste B: rádio conectado, aparelho parado, zero excursões),
contato mecânico (R4 — dispara com a tecla `3` do teclado, sem tocar em botão),
e refresh do painel (R5 — `R6` de `r9` contra taxa base `f48`, que é a taxa
base, não correlação).

**Conserto: mediana de três conversões por canal.** Não debounce mais longo: o
fantasma dura 2 varreduras, mas a leitura ruim aparece em duas chamadas
consecutivas de `getState()` — e dentro de uma varredura só a *primeira*
conversão é pós-wake, então a mediana mata mesmo com duas varreduras ruins
seguidas. E uma regra em contagem de amostras seria insegura: na cadência
ociosa de 50 ms uma apertada real de 100 ms também são só 2 amostras.

Mesmo remédio, mesmo ADC e mesma justificativa declarada do
`BatteryMonitor::readMillivolts()`, que já tira mediana de sete ali "to reject
outliers from SPI/BLE/charging noise".

Por que as rodadas 1 e 2 não acharam: elas vigiavam o **canal 2** procurando um
Up fantasma. Um Right fantasma **já aparece como Up** em toda tela de lista —
pelo alias do modo `Fixed` (`navUpEdge`) ou, em `Natural` + `LANDSCAPE_CCW`,
porque `dpadResolve` manda Right direto para `Dir::Up` (`dpad.cpp:44`). Uma
causa, dois sintomas, dependendo da tela. O canal 2 nunca teve nada de errado.

**Bug latente de brinde**, ainda não consertado: a faixa de baixo aberta até
`INT32_MIN` significa que **qualquer valor negativo decodifica como Right**.
Hoje `adc1_get_raw()` no C3 não tem caminho que devolva um — bloqueia esperando
o lock em vez de falhar, e a leitura única faz busy-wait no flag de DONE — então
não é alcançável. Fixado em teste para que mudar isso se anuncie.

Único desvio do "byte a byte" em `lib/InputManager/`, e deliberado: o filtro tem
que ficar onde a conversão acontece. Candidato a PR upstream.

---

### 7. Hotspot inutilizável — `src/wifi_sync.cpp` + `src/main.cpp`

Sintoma: o notebook via o hotspot, associava, e nunca completava. O
NetworkManager reportava "senha incorreta", e **não era senha** — a senha nunca
chegou a ser testada por completo. Duas falhas distintas, uma raiz.

**Com o teclado BLE ligado**, três vezes, exatamente 4 s cada:

```
Associated with e0:72:a1:83:c0:1d
CTRL-EVENT-DISCONNECTED reason=15     ← 4WAY_HANDSHAKE_TIMEOUT
CTRL-EVENT-SSID-TEMP-DISABLED reason=CONN_FAILED
```

`reason=15` e **não** `WRONG_KEY`: senha errada completa a troca e falha na
verificação de MIC; `reason=15` é a troca nunca terminando. Os 4 s são o
orçamento de retransmissão do EAPOL expirando. O C3 tem **um** rádio de 2,4 GHz,
e um link BLE a 30–50 ms não deixa o EAPOL caber.

**Com o teclado desligado** o handshake completa — e aí:

```
dhcp4: beginning transaction (timeout in 45 seconds)
dhcp4: state changed no lease                        (3×)
device: ip-config -> failed (reason 'ip-config-unavailable')
```

Camada 2 de pé, e o AP **nunca** responde DHCP.

Essa é a forma de um AP que dorme entre beacons: o beacon é agendado, então ele
é **visível**; a associação repete até cair numa janela acordada, então ele
**associa**; o EAPOL precisa de vários round trips em ~4 s, então é marginal; e
um DHCP DISCOVER que chega durante uma janela de sono é simplesmente perdido —
45 s de tentativas erram todas.

**Nós pedimos por isso.** O `esp_pm_configure(light_sleep_enable = true)` é
global e o `CONFIG_ESP_PHY_MAC_BB_PD` desliga MAC e baseband no light sleep. Um
soft AP não consegue ser um soft AP nessas condições.

**O tipo de lock era a armadilha.** O `main.cpp` raciocinava que "o AP segurando
`ESP_PM_APB_FREQ_MAX`" significava sem light sleep. São locks **diferentes**
(`esp_pm.h:44-54`): o `APB_FREQ_MAX` fixa o clock do APB; quem impede light sleep
é o `ESP_PM_NO_LIGHT_SLEEP`, e ninguém o segurava. Comentário corrigido no lugar.

Conserto, três partes com **observáveis distintos** — é o que permite as três de
uma vez sem violar "uma variável por vez", porque o log diz qual falhou:

- `WiFi.setSleep(false)` antes do `softAP()` — `esp_wifi_set_ps(WIFI_PS_NONE)`;
- um lock **`ESP_PM_NO_LIGHT_SLEEP`** segurado só enquanto o AP está no ar.
  Liberado no `radioOff()`, que é o chokepoint único dos dois teardowns
  (`enterDoneState` e `wifiSyncStop`) — **toda** saída tem que passar por lá, ou
  o aparelho nunca mais faz light sleep e a bateria paga em silêncio;
- **BLE fora do ar** durante o hotspot: `cancelPendingConnection()` +
  `disconnectCurrentDevice()`, e o `autoReconnectEnabled = false` em
  `main.cpp` — ali porque o loop reescreve essa flag **a cada iteração** por
  estado de UI, e setá-la do lado do wifi_sync seria desfeito em milissegundos.

Custo de bateria, como este repositório exige: o lock vale **só** durante o
hotspot, que é tela modal com timeout de 5 min sem cliente e 60 s sem HTTP. Fora
dela nada muda.

#### E nada disso consertou o DHCP

As três partes acima são boas e ficam: elas consertaram o `reason=15` e o "AP
Failed" logo após o boot. Mas **não eram a causa do `no lease`**, e tratá-las
como se fossem custou quatro gravações em quatro hipóteses — senha, light sleep,
contenção de rádio, `softAPConfig()` faltando.

O que a medição mostrou, depois que o `curl -v` passou a conectar e receber zero
bytes:

```
h 64/63/63/17/10k    ← heap livre em cinco pontos do caminho
hc:1272 rq:1 heap:6k/4k
```

- **64k é o teto deste aparelho**, medido no fim do `setup()`. Nunca houve
  orçamento grande.
- **63 → 63 ao desconectar o BLE.** Desconectar o link libera **zero**. O custo
  é a stack residente, não a conexão. Este número é o conserto inteiro.
- **63 → 17 no `raiseAp()`.** O AP WiFi custa 46k.
- **17 → 10 no servidor HTTP.** WebServer + mDNS, 7k.

Com 10k na largada: o servidor DHCP precisa alocar um pbuf para responder o
OFFER e o `accept()` precisa alocar um `WiFiClient` e um descritor — os dois
falham. O handshake TCP **continua completando**, porque o LwIP o resolve na
task de rede com PCB do pool dele, sem passar pela aplicação. Daí a assinatura
que enganou: "Established connection" seguido de zero bytes em 10 s.

**`no lease` e `0 bytes received` eram o mesmo bug**, não dois. Foi por isso que
cada hipótese explicava um sintoma lateral e nenhuma resolvia.

#### A primeira tentativa de conserto travou o aparelho — não repita

A reação óbvia a "o NimBLE segura ~40k que não usa durante o hotspot" é
desmontar a stack: `NimBLEDevice::deinit(true)` na entrada, `init()` na saída.
**Funcionou para a memória e travou a task do loop.** Duas gravações, dois modos
de falha distintos:

- **na volta:** `NimBLEDevice::init()` retornou `false` e o código engolia o
  retorno. Sem teclado, sem motivo na tela, e o Scan em Settings → Bluetooth não
  fazia nada porque a guarda dele encontrava uma stack desinicializada;
- **na ida:** tela do hotspot desenhada e congelada ali. Sem `Clients`, sem
  ESC, saída só por reboot.

Por que isto é inaceitável aqui e não só feio: `NimBLEDevice::init()` termina em

```c
while (!m_synced) { ble_npl_time_delay(1); }   // esp-nimble-cpp, init()
```

e a biblioteca **ignora o erro de `nimble_port_init()`** logo antes. Então um
init que falha não retorna — ele gira para sempre. Na task do loop, num aparelho
sem recuperação por USB, isso é a pior classe de falha que este projeto admite.
`nimble_port_stop()` dentro do `deinit()` tem a mesma forma do outro lado.

Hipóteses que morreram na leitura da fonte, para ninguém refazer o caminho:

- *"o `deinit` deixa o controller BLE ligado e o `init` seguinte falha com
  `ESP_ERR_INVALID_STATE`"* — depende de `CONFIG_NIMBLE_CPP_IDF`, que **está**
  definido (`platformio.ini:37`), e da versão do IDF, que aqui é **4.4.x** — o
  `CONFIG_ESP32_WIFI_*` do `sdkconfig.xteink_x4` é o prefixo canônico do 4.4, e
  um "5.5.0" lido antes nesta campanha vinha de outro pacote instalado, não do
  que compila. Nessa combinação os dois lados pulam o bloco do controller e delegam a
  `nimble_port_init`/`nimble_port_deinit`, que são simétricos;
- *"o `radioOff()` não é alcançado na saída"* — é: `wifiSyncStop()` chama
  `stopHttpServer()` e `radioOff()` em seguida, e é ele que o `main.cpp`
  invoca ao sair da tela.

Ou seja: a fiação estava certa e o ciclo deinit/init é que não é confiável nesta
combinação de versões. **Revertido por inteiro.** Conter um congelamento é pior
que removê-lo.

#### O conserto que ficou: orçamento de RAM em `sdkconfig.defaults`

Os dois lados do orçamento estavam **inteiramente no default**, dimensionados
para throughput que este aparelho não tem o que fazer com. O ataque é por ali,
onde nada pode pendurar — são tamanhos de pool decididos em tempo de compilação,
não controle de fluxo em runtime:

- **WiFi**, baixando o custo dos 46k: `STATIC_RX_BUFFER_NUM` 10→4 (alocado na
  inicialização do WiFi, a economia mais direta), `DYNAMIC_RX`/`DYNAMIC_TX`
  32→8, `MGMT_SBUF_NUM` 32→8, AMPDU TX e RX desligados (agregação de quadros
  para throughput alto: custa as janelas de block ack e não compra nada em
  alguns KB de texto para um cliente), WPA3/SAE desligado (o AP é WPA2);
- **LwIP:** `TCP_SND_BUF_DEFAULT` e `TCP_WND_DEFAULT` 5760→2880, por socket —
  ainda o dobro de um MSS, e a exportação é streaming de qualquer forma;
- **NimBLE**, subindo a linha de base dos 64k, que é a outra metade do problema:
  `MAX_CONNECTIONS` 3→1 (um teclado por vez), `ROLE_PERIPHERAL` e
  `ROLE_BROADCASTER` desligados — o aparelho é central/observer e nunca anuncia
  (verificado: o único símbolo de advertising no `src/` é
  `NimBLEAdvertisedDevice`, que é **resultado de scan**), `ATT_PREFERRED_MTU`
  256→128, `MSYS1_BLOCK_COUNT` 12→8, `MAX_CCCDS` 8→4. `MAX_BONDS` fica em 3:
  é capacidade de pareamento que a UI expõe, não folga.

**`sdkconfig.defaults` não é relido sozinho.** O `sdkconfig.xteink_x4` gerado é
cacheado, e um `pio run -t clean` **não** o regenera — a primeira rodada destas
mudanças compilou com todos os valores antigos e não mudou nada. Apague
`sdkconfig.<env>` para forçar, e **faça o diff do antes e depois**: a
regeneração é a hora em que algo não pedido entra sem avisar. Nesta aqui o
desvio foi zero, e as únicas linhas alteradas fora dos knobs eram consequência
deles (as janelas de block ack saem com o AMPDU, o `SAE_PK` sai com o WPA3, e
`CONFIG_TCP_*_DEFAULT` são aliases do LwIP).

#### O teclado que não voltava: `BLE_YIELD_MS` < `CONNECT_TIMEOUT_MS`

Sintoma, visto com o ciclo deinit/init já removido — portanto **não** causado por
ele, só mascarado: depois de sair do hotspot o teclado não reconecta, e o Scan
em Settings → Controls → Bluetooth também não faz nada. A stack está sã.

```c
// wifi_sync.cpp
static constexpr unsigned long BLE_YIELD_MS = 3000;
// ble_keyboard.cpp
static constexpr uint32_t CONNECT_TIMEOUT_MS = 10000;
```

A espera era **mais curta que a coisa que ela espera** — e a descoberta de
serviços roda ainda por cima do timeout. Passados os 3 s o código registrava
"BLE still holds the radio; trying anyway" e subia o AP, tomando o rádio de uma
task de conexão viva. Ela fica bloqueada dentro de `connect()` ou
`secureConnection()` e **`connectTaskHandle` nunca volta a `nullptr`**:

```c
if (bleState == DISCONNECTED && autoReconnectEnabled && connectTaskHandle == nullptr)
```

Todo gate que inicia conexão testa esse handle — o auto-reconnect e o
`startConnectTask()` que o Scan usa. Daí o silêncio nos dois.

Conserto: `bleOffTheAirForAp()` retorna `bool` e **recusa** em vez de "tentar de
qualquer forma"; `wifiSyncStartAp()` aborta com "Bluetooth busy - try again".
Esperar os 13 s travaria a UI numa tela modal. Um hotspot que não sobe custa um
botão; um teclado morto até o reboot não se recupera do aparelho.

Vale como regra geral neste código: **toda espera por um recurso tem que ser
mais longa que o timeout de quem o detém, ou então recusar** — "tentar de
qualquer forma" sobre um recurso compartilhado troca uma falha visível por um
estado corrompido e silencioso.

#### Orçamento de RAM: o livro-caixa

Medido com `riscv32-esp-elf-nm -S --size-sort` no `.elf` — as fontes aparecem
grandes mas estão em `0x3c…`, que é flash mapeada, não RAM. O que é RAM de
verdade:

| | antes | agora |
|---|---|---|
| `display` (framebuffer) | 48.036 | 48.036 — essencial |
| `textBuffer` | 16.384 | 16.384 — é a nota |
| `clipboard` | 16.384 | **0** (heap, sob demanda) |
| mDNS | task + estruturas | **0** |
| RAM estática total | 157.448 | **138.280** |

Os knobs de `sdkconfig.defaults` compraram só ~3k (10k → 13k livres durante o
AP), o que **refuta** a ideia de que os 46k do AP são buffers de RX/TX. Ficam
porque não custam nada, mas o ganho real veio de tirar 16k do `.bss`: o pool do
heap cresce por esse valor e o teto sobe em todo boot.

`startConnectTask()` alocava **32 KB de stack** no heap, com um comentário que
dizia "the device has ~200 KB of free heap at this point". Não tem mais — o teto
é ~84k. Isso virou o bug #8, abaixo.

#### Bug #8: o reconnect silencioso — `xTaskCreate` sem retorno checado

Sintoma: depois de uma sessão de hotspot o teclado nunca reconecta. Com a stack
NimBLE intacta, `autoReconnectEnabled` correto, `bleState` correto e o Scan
manual igualmente mudo.

```c
xTaskCreate(bleConnectTask, "ble_conn", 32768, NULL, 1, &connectTaskHandle);
```

O retorno **não era checado**. Em falha o FreeRTOS deixa `connectTaskHandle`
como estava — nulo — e `bleLoop()` já consumiu `connectToKeyboard`. Falha
totalmente silenciosa: o backoff dobra até o teto de 2 min e nada diz por quê.

E o pedido é de **32 KB contíguos**. Medido depois do hotspot: **68 KB livres,
maior bloco 23 KB**. Não faltava memória, faltava buraco — uma sessão de WiFi
fragmenta o heap, e é por isso que só acontecia depois do hotspot e nunca no
boot.

Três consertos descartados por aritmética, antes de escrever código:

| caminho | por que não |
|---|---|
| `xTaskCreateStatic` com buffer estático | devolve 32 KB ao `.bss`, que o AP nunca pode usar: teto cai a ~52 KB, AP come ~46 KB |
| task criada no boot, estacionada em semáforo | mesma aritmética |
| pré-alocar e re-alocar na saída do AP | a re-alocação encara os mesmos 23 KB |

Então: reduzir a **necessidade**, medindo em vez de escolher.

**A medição, e a armadilha dentro dela.** `uxTaskGetStackHighWaterMark()` nos
quatro caminhos de saída deu `st2/32k` — 2 KB de 32. Mas três das quatro saídas
são abandonos precoces (`connect()` recusado, HID falhou) que **nunca chegam à
descoberta**, e um high-water mark vindo de um deles faria o stack parecer vazio.
Agir sobre esse número reintroduziria o panic que os 32768 existiam para evitar,
num aparelho sem recuperação por USB. A segunda rodada gravou só em conexões
**completas**, com contador: `st2/32k ok1`. Aí sim.

**E a medição derrubou o diagnóstico original do fork.**
`NimBLEClient::retrieveServices()` dispara o pedido GATT e bloqueia em
`taskWait(taskData, BLE_NPL_TIME_FOREVER)` — todo callback de descoberta
(serviços, características, descritores) roda na **host task do NimBLE**. A task
de conexão só espera. Logo o panic do Logitech "durante a descoberta em 20480"
não podia ser ela; subir para 32768 mudou o layout do heap e mascarou o sintoma.
A margem que faltava era `CONFIG_BT_NIMBLE_TASK_STACK_SIZE`, no default de
**4096**.

Conserto, nas duas pontas:

- `CONNECT_STACK` 32768 → **12288** (6x os 2 KB medidos, e cabe num heap
  fragmentado);
- `CONFIG_BT_NIMBLE_TASK_STACK_SIZE` 4096 → **8192**, onde a descoberta
  realmente acontece;
- o retorno do `xTaskCreate` é checado, a falha vai para a tela como
  **"No RAM for BLE - reboot"** (acionável: o reboot devolve o heap inteiro), e
  uma falha de alocação **não** dobra o backoff — não é culpa do teclado.

Troca líquida: 20 KB a menos de demanda contígua transitória por 4 KB a mais de
heap permanente, alocado no boot com o heap intacto. E a margem passa a estar
onde o trabalho está.

**Confirmado no aparelho em 06/10/2026**: hotspot levantado, sessão usada, saída
da tela, teclado reconectou. O instrumento (`bleDiagText()`, a linha
`fail0 h83/34k st2/32k ok1` na tela de Bluetooth, e o `recordStackUse()` que a
alimentava) saiu no mesmo binário que a página de exportação.

O número medido fica **aqui**, não no código: **2 KB de uso real** numa conexão
completa, contra os 12288 de `CONNECT_STACK` — 6x de margem. Se um teclado
voltar a dar panic nessa task, essa é a referência: suba `CONNECT_STACK` **e**
`CONFIG_BT_NIMBLE_TASK_STACK_SIZE` juntos, em vez de rederivar o número do zero.
E se alguém reinstrumentar, a distinção que importa é a mesma: medir **só** em
conexões completas, com contador, porque três das quatro saídas não chegam à
descoberta.

**Lições de método, que é o que vale levar daqui.**

As duas medições que resolveram o diagnóstico (os contadores `hc`/`rq` e a linha
de heap) custaram duas gravações. As quatro hipóteses anteriores custaram quatro
e não eliminaram nada. O repositório já manda instrumentar antes de consertar; o
caso difícil é quando a hipótese é *plausível* e *parcialmente certa* — três
delas consertaram bugs reais, o que fez cada uma parecer progresso. Sintoma que
não some depois de um conserto que claramente funcionou significa **duas
causas**, e a hora de medir é ali, não depois da quarta tentativa.

E diagnóstico certo não garante conserto certo: a causa (heap) estava provada
quando o primeiro conserto travou o aparelho duas vezes. Antes de chamar uma API
de teardown/restore de biblioteca no caminho da UI, **leia o fim dela**. Um
`while (!cond)` sem timeout, sobre um erro que a própria biblioteca descarta, é
um congelamento esperando o dia errado — e aqui congelar custa um reboot
manual, que é precisamente o que "nada de recuperação irreversível" existe para
evitar. Quando havia um caminho de compilação para a mesma memória, ele era o
certo desde o início.

---

## Instrumentação de diagnóstico — removida

Existiu porque não há console serial: rodapé do menu principal com `RST:<causa>`,
histórico de boots em NVS (`SW! PAN! PWR!`), breadcrumb `BLE:<tag>` do caminho de
conexão e `<task>@<pc>` do core dump. Cumpriu o papel e saiu.

A instrumentação que achou o bug #7 (hotspot) saiu também: cinco marcadores de
heap no caminho até o AP (`h 64/63/63/17/10k`) e os contadores `hc`/`rq` de
chamadas ao `handleClient()` contra handlers que de fato rodaram. Os dois juntos
separaram "o loop não chama" de "chama e não aceita" de "não tem memória para
aceitar", numa gravação. O padrão vale reusar: **um contador só vale com
denominador**, e um instrumento que mede o estado errado (o `DHCP: on` que lia a
flag do netif em vez do endereço) atrasa em vez de ajudar.

**Ficou** na tela do hotspot: `heap:<livre>k/<maior bloco>k` (`apHttpStatusText()`
— o `ble:<up|off>` que a linha tinha ao lado saiu junto com a reversão do
deinit/init, e com ele o `bleStackIsUp()`).

Fica enquanto a exportação estiver sendo construída — o teto do aparelho é 64k,
o AP come 46k, e o modo de falha é silencioso (DHCP para de responder e o TCP
continua completando, o que se lê como "o servidor quebrou"). O P3 da exportação
é exatamente a peça que cobra heap novo: uma página servida e uma string de HTML.
Sai depois que a página estiver confirmada no aparelho, junto com o `dev_wipe`.

O que ficou de propósito:

- **A captura do core dump continua ligada** (`CONFIG_ESP_COREDUMP_*` em
  `sdkconfig.defaults`). O panic handler ainda grava o dump em flash; o que saiu
  foi só a leitura e a exibição. Se um crash voltar, reexibir é reverter um
  commit, não reconstruir o instrumento.
- `dropLegacyDiagKeys()` em `src/main.cpp` apaga uma única vez as chaves NVS
  `hist` / `crumb` / `why` que a instrumentação deixou. É autolimitante — depois
  da primeira limpeza nunca mais escreve. Pode ser deletada.

Para resolver um PC de crash, se voltar a existir um:

```bash
$(find ~/.platformio -name 'riscv32-esp-elf-addr2line' | head -1) \
  -pfiaC -e .pio/build/xteink_x4/firmware.elf <pc>
```

Código morto encontrado e **ainda presente**: `bleConnIdleMode` é atribuído em 3
lugares e nunca lido; `updateConnParams()` não é chamado em lugar nenhum, apesar
de um comentário afirmar que "the adaptive params logic in bleLoop() handles this".

---

## Layout vertical: o que `drawText()` realmente faz

Erro fácil de cometer nesta base, e que produziu o sintoma "as palavras estão
baixas e cortadas embaixo" em todos os menus:

`GfxRenderer::drawText(font, x, y, ...)` trata `y` como **o topo da caixa do
ascender**, não como baseline nem como topo da tinta. A baseline cai em
`y + ascender`. E `ascender` é o glifo mais alto do **fonte inteiro** — 27px na
notosans_12 — enquanto texto comum só alcança a altura de caixa-alta (18px).

Consequência: texto desenhado no topo de uma faixa aparece ~9px mais baixo do que
parece que deveria, e os descendentes (`g`, `p`, `ç`) vão parar 33px abaixo de
`y` — fora de uma faixa de 35px. Como a faixa selecionada desenha o texto na cor
invertida, o que sai da faixa simplesmente some: o "corte embaixo".

`src/ui_renderer.cpp` tem helpers que medem isso direto das tabelas do fonte:

- `inkOf(fontId)` → extensão real da tinta (topo de `Á` até o fundo de `g`),
  medida a partir do `y` do `drawText()`
- `textYInBand(fontId, bandTop, bandH)` → o `y` que centraliza a tinta na faixa
- `bandHeight(fontId)` / `rowPitch(fontId)` → altura da faixa e passo entre linhas
- `lineStep(fontId)` → espaçamento entre linhas soltas. **Não** use
  `getLineHeight()` para isso: ele devolve `advanceY`, que na ubuntu_10 é
  exatamente a altura da tinta, e linhas empilhadas saem encostadas
- `footerRuleY(r, n)` / `footerLineY(ruleY, i)` → rodapé com `n` linhas

A referência para o topo da tinta é **`Á`, não `A`**, de propósito: um layout
calibrado em ASCII sem acento corta todo `á` e `ç` no dia em que o pt-br entrar.

Nenhuma tela deve voltar a empilhar texto com constantes fixas. A tela de
Bluetooth fazia isso (y = 45, 60, 72, 104 para linhas de 24px de altura) e as
linhas se imprimiam umas por cima das outras sempre que scan e lista apareciam
juntos.

---

## Build

```bash
pio run       # firmware
./test/run.sh # testes de host (ver seção abaixo) — rode antes de gravar
```

Mexeu em `sdkconfig.defaults`? **Apague `sdkconfig.xteink_x4` antes** — o próprio
arquivo avisa isso no topo. Sem isso a mudança não entra no build, e a
recompilação do IDF inteiro leva vários minutos.

Toolchain: PlatformIO em `~/.platformio/penv` (instalado pelo script oficial, não
por `pip` — Fedora bloqueia pip no Python do sistema). Máquina: Fedora KDE.

---

## Suporte a pt-br — implementado

Teclado alvo: **ABNT2**. Trocável em Settings (`Keyboard: US / US-Intl / ABNT2`),
persistido em NVS e no backup do SD. Default ABNT2.

### O erro que custou uma gravação — leia antes de mexer no teclado

Um teclado BLE reporta teclas por **posição física**, nunca pelo que está escrito
na tecla. O usage `0x34` significa "a segunda tecla à direita do L" em qualquer
teclado do mundo: é `'` no US ANSI e `~`/`^` no ABNT2.

Logo: **o layout não é detectável, tem que ser informado.** A primeira versão foi
escrita para US ANSI e o sintoma no aparelho foi apertar `~` e sair `ä` — porque
`0x34` chegava e o mapa US lia como aspas, armando trema em vez de til.

Se um símbolo sair errado, **não mexa no mapa por dedução**: vá em Settings e
leia o rodapé (`Key: 0x34 mod:0x00 -> ~ (dead)`). Ele mostra o código bruto que
o teclado mandou. Existe exatamente por isso.

### Estrutura

- `src/keymap.cpp` — HID usage → codepoint, uma tabela por layout. A tabela
  ABNT2 tem um comentário com a correspondência posição a posição contra o US
  ANSI; é a referência.
- `src/deadkeys.cpp` — composição de acento, indexada pela **marca**
  (agudo/grave/circunflexo/til/trema), não pela tecla. ABNT2 arma o agudo pela
  tecla `´` e o US-Intl pelo apóstrofo; os dois caem na mesma tabela.
- `src/utf8_util.cpp` — encode e caminhada por fronteira de caractere.

### As decisões que valem lembrar

**O buffer não mudou.** `textBuffer` já é `char[16384]`, e UTF-8 já é uma
sequência de bytes. Arquivos salvos são UTF-8 puro, sem conversão na gravação. O
que mudou foi tudo que assumia **1 byte = 1 caractere**: inserção, backspace,
delete, setas, word wrap.

**Acento não muda o advance do glifo** (verificado nas tabelas das fontes). Então
`charsPerLine` não precisou mudar — só o loop de wrap, que contava bytes.

**Acento armado aparece dentro do cursor, invertido.** Num e-ink de refresh lento,
uma tecla que não imprime nada é indistinguível de uma tecla ignorada. Backspace
com acento armado desarma o acento; setas e Esc também.

**Fallback nunca engole caractere.** `~t` não existe, então saem os dois. `~~` sai
como `~~`. Nenhum estado escondido sobrevive à tecla.

**`0x87` (International1) e `0x64` (Non-US \\) são teclas reais do ABNT2** — a
barra `/?` e a barra invertida ao lado do Shift esquerdo. O mapa US não as tem.

### Não implementado

AltGr (`¹²³£¢¬ºª°`). Os bits de Alt chegam em `modifiers`; é acrescentar os casos
em `abnt2Key()`.

### Teto conhecido

O título da nota faz round-trip pelo **nome do arquivo** (`loadFile()` reconstrói
o título com `filenameToTitle()`). `titleToFilename()` dobra acento para ASCII em
vez de descartar o byte — "Diário" era `dirio.txt` e voltava "Dirio", hoje é
`diario.txt` e volta "Diario". **O acento ainda se perde no título.** Guardar de
verdade exige mudar o formato do arquivo de nota e o protocolo de sync. O texto
dentro da nota nunca foi afetado.

---

## Sleep screens e botão power

### Wallpapers

Modo em Settings → `Sleep Screen: Text / Slideshow / Shuffle`. Imagens em BMP:

```
/sleep/       pasta de imagens — é o slideshow
/.sleep/      igual, oculta — é o caminho do próprio CrossPoint, então um cartão
              preparado para um aparelho CrossPoint funciona aqui sem mexer
/sleep.bmp    imagem única
```

Formatos aceitos pelo `Bitmap` vendorizado: **BMP de 1, 2, 8, 24 ou 32 bits**,
sem compressão (BI_RGB; BI_BITFIELDS só em 32 bits), até 2048x3072. Imagem maior
que a tela é **reduzida** para caber e centralizada; menor **não** é ampliada —
ampliar em e-ink transforma um 1-bit nítido em papa.

Prefira imagens já no tamanho do painel (480x800 ou 800x480): evita a redução e
o tempo que ela custa ao dormir.

### Como a imagem é desenhada

O mesmo caminho do CrossInk (`SleepActivity::renderBitmapSleepScreen`):

1. A foto é reduzida para caber **antes** do pontilhado
   (`Bitmap::setDitheredOutputSize`). Pontilhar no tamanho original e depois
   encolher por vizinho mais próximo vira costura.
2. O pontilhado é Atkinson, nos 4 níveis pares (0/85/170/255).
3. O painel mostra esses 4 níveis com o LUT de grayscale (`displayGrayBuffer`).
   BMP de 1 bit continua preto e branco, um único half refresh, para não
   amolecer um desenho nítido.

Sem cartão, sem pasta, ou sem nenhum BMP que faça parse: **cai no card de texto**.

Qual imagem aparece fica num anel de 16 índices em **RTC memory** (o mesmo
tamanho do `recentSleepImages` do CrossInk). Shuffle escolhe uniforme entre as
que não estão nesse anel, e a janela para em `count - 1` para uma pasta pequena
ainda ter para onde ir. Os nomes são ordenados, então o índice é o mesmo arquivo
de um sono para o outro. Não sobrevive a boot frio; o slideshow recomeça na
primeira imagem.

### Botão power: 400ms, e os dois guardas que vêm junto

Era 3000ms; agora é **400ms**, que é o valor do crosspoint
(`CrossPointSettings::getPowerButtonDuration`). Valor original anotado no
`main.cpp`.

**Encurtar não é só trocar o número.** Dois guardas são obrigatórios:

1. `powerReleasedSinceWake` — você acorda o aparelho *apertando* power, e no wake
   o chip reseta com o botão ainda afundado. Com 3000ms você já teria soltado;
   com 400ms ele voltaria a dormir na sua mão. O sleep só arma depois de ver uma
   soltura.
2. `allowSleepAt` — janela de graça após o boot, para o primeiro refresh terminar
   antes que qualquer segurar conte.

E a janela do toque curto (ir ao menu principal) teve que encolher de `<1000ms`
para `<POWER_SLEEP_HOLD_MS`: com o teto antigo, meio segundo de aperto seria as
duas coisas ao mesmo tempo.

---

## crosspoint-reader como referência

`https://github.com/crosspoint-reader/crosspoint-reader` (branch `develop`, MIT).
É legítimo e barato usar como referência: `lib/GfxRenderer/`, `lib/EpdFont/`,
`lib/InputManager/` e `lib/SDCardManager/` **daqui são o mesmo código de lá** —
as assinaturas batem, e o `main.cpp` deste fork já traz "matches crosspoint
pattern" escrito. O FreeInk SDK do `lib/RecoveryBoot/` é a mesma família.

Arquivos que valeram a consulta:

- `src/activities/boot_sleep/SleepActivity.cpp` — wallpapers de sleep. 889 linhas
  lá, porque também faz PNG, overlay com alpha, capa de livro e quick-resume.
  Aqui ficaram ~180: um aparelho de escrita não precisa do resto.
- `src/CrossPointSettings.h` — `getPowerButtonDuration()`, onde está o 400ms
- `src/main.cpp` — `powerReleasedSinceWake` e `allowSleepAt`

Diferença de API a lembrar: o `drawBitmap()` de lá ganhou um parâmetro
`preserveBackground` e retorna `bool`; o snapshot vendorizado aqui é anterior e
retorna `void`. Ao copiar trecho de lá, confira a assinatura local primeiro.

---

## Testes de host — use isto

```bash
./test/run.sh          # ~2 segundos, sem PlatformIO, sem aparelho
```

Compila os **fontes reais** do firmware — não cópias, então o teste não descola
do código:

- `test_ptbr.cpp` → 90 asserções: UTF-8, os três layouts de teclado, composição
  de acento, navegação do editor
- `test_sleep.cpp` → posicionamento do wallpaper e a ordem do slideshow/shuffle
- `test_menu.cpp` → os atalhos de dígito dos menus: quais teclas contam, quais
  modificadores desqualificam, e o cruzamento com `keymap` que comprova que a
  fileira de números está na mesma posição nos três layouts
- `test_files.cpp` → derivação de nome, busca e ordem, o teto de 16 KB do
  `loadFile`, e o **sidecar `/.ardosia/note_seq.bin`** — o único formato binário
  que o Ardosia grava no cartão: round-trip v2, leitura de um arquivo v1 antigo
  preservando a ordem de criação, recusa de índice danificado, e a guarda que
  impede o autosave de reescrever o índice a cada dez segundos
- `test_dpad.cpp` → a rotação dos botões de direção: que `Standard` é a
  identidade, que retrato não mexe em nada, que toda orientação é bijeção
  (nenhum botão perdido, nenhuma direção inalcançável), que as duas paisagens
  se desfazem uma à outra, e — concreto, não derivado — **a tabela que o
  aparelho confirmou**. A primeira versão foi construída a partir do antigo
  remendo `|| btnRight`, que descreve o case de quem escreveu o remendo e não
  este: o Ardosia é usado com a parte de baixo para a **direita** e as teclas
  laterais acima, e no painel o mapa saiu invertido. Os dois `case` de paisagem
  foram trocados em 23/09/2026. As quatro asserções concretas existem porque
  todo o resto do arquivo é forma (bijeção, espelho, identidade) e passaria
  igualmente com a tabela ao contrário
- `test_ui_layout.cpp` → a aritmética vertical das telas: extensão da tinta a
  partir das tabelas do fonte, altura de banda e passo de linha, a moldura
  (cabeçalho, rodapé de três colunas, caixa de corpo), a caixa modal, a janela
  de rolagem e a barra. Duas propriedades carregam o peso: todo pixel de tinta cai dentro da
  faixa, e a seleção está sempre dentro da janela visível — ambas varridas
  sobre os fontes reais × formas de lista
- `test_quickmenu.cpp` → o menu rápido levantado ao **segurar** o select (ou
  Ctrl+K). Mesma forma do diálogo de confirmação, e pelo mesmo motivo — o editor
  está vivo atrás da caixa: o seletor nunca abre nem dá a volta em cima do
  Delete, tecla estranha é engolida (Left/Right inclusive, que o auto-repeat do
  editor continua disparando), a linha em que o menu foi levantado sobrevive, e
  só as três linhas que saem da tela fecham o menu — as quatro que ciclam um
  valor ficam de pé, porque a linha é o único lugar onde o valor novo é legível
  com a página escondida
- `test_buttons.cpp` → o **ladder de ADC dos botões**, compilando o
  `lib/InputManager` real contra stubs que deixam o teste escolher o que cada
  conversão devolve. Existe porque o bug que ele cobre custou cinco gravações
  para achar, e é inteiramente decidível no host: o ladder transforma um número
  em botão, então quem escolhe o número escolhe a falha. Cobre as seis teclas
  reais nas **duas cadências** de varredura (10 ms ativa, 50 ms ociosa — a
  armadilha que uma regra em contagem de amostras cairia), a rejeição de uma
  conversão ruim em qualquer das três posições nos dois canais, o **teto
  declarado** do filtro (duas ruins de três ainda passam), a honestidade do
  contador do filtro, e a forma da tabela de faixas — incluindo que a faixa de
  repouso são só 295 contagens de 4096 e que um valor negativo decodifica como
  Right
- `test_hatch.cpp` → a **recuperação embutida**. Duas tarefas, e a segunda é a
  que se paga: (1) o que a linha de Settings pode oferecer, e sobretudo o que ela
  **recusa** — a ação sobrescreve um slot OTA inteiro num aparelho cuja única
  rota de fuga pode ser o que está nele; (2) **o `assets/hatch.bin` de verdade**,
  parseado e verificado como imagem ESP32 — magic, chip id, caminhada da tabela
  de segmentos, checksum XOR e tamanho declarado contra real. É o que pega um
  blob truncado ou de outro chip **em build time** em vez de no vidro, e é por
  isso que o firmware não precisa de SHA em runtime: a carga viaja dentro de uma
  imagem que o bootloader já verificou por hash, então a única corrupção que
  chega ao aparelho é a que já estava lá quando compilamos
- `test_ota_names.cpp` → **o nome do slot OTA no menu**. A propriedade que
  carrega o peso é a que o código antigo errava: um apelido só aparece quando se
  pode **demonstrar** que ainda descreve o que está no slot. Cobre o estado exato
  do aparelho (ota_0 que era CrossInk, hatch gravado por cima, NVS teimando),
  que um nome escolhido à mão sobrevive a uma atualização do mesmo app, que um
  apelido sem projeto gravado não é confiado, a cadeia de fallback inteira, e que
  nenhuma entrada produz rótulo vazio — linha em branco num aparelho sem console
  é indistinguível de bug
- `test_escape.cpp` → a **fuga generalizada** do combo (`src/escape.cpp`). O peso
  está nas propriedades **negativas**, porque um falso positivo custa um reboot
  para outro firmware e num aparelho sem USB a rota de fuga é o único
  comportamento que não tem segunda chance: nunca troca sem o combo, nunca troca
  para um slot sem app, nunca troca duas vezes seguidas, e um `EscapeInputs`
  default-construído não faz nada. Três booleanos são oito casos, então a tabela
  verdade é **enumerada, não amostrada**
- `test_note_json.cpp` → **a forma JSON da lista de notas** (`src/note_json.cpp`).
  A propriedade que carrega o peso é **negativa e sobre truncamento**: uma
  entrada que não cabe tem que ser recusada **inteira**, nunca escrita pela
  metade. Entrada parcial não perde uma nota — corrompe o array dali em diante e
  leva todas as seguintes, que é o que o achado F5 era de fato. Varre toda
  capacidade de 1 a 39 (cabe exatamente ou recusa limpo, com sentinela depois do
  fim), todo nome só-de-aspas de 1 a 255 bytes contra o
  `NOTE_JSON_ENTRY_MAX` — se esse número estiver errado a lista derruba notas em
  silêncio — e o array montado como o chamador monta, para a contabilidade de
  vírgula não produzir `[,` nem `},]`. O escaper do F5 **não tinha teste até
  agora**
- `test_zip.cpp` + `test/check-zip.py` → o **writer de ZIP** da exportação de
  notas (`src/zip_writer.cpp`), e são **dois portões de naturezas diferentes**.
  O `.cpp` afere os bytes contra o que eu espero que sejam, o que prova só que
  sou consistente comigo mesma; o `check-zip.py` abre o **mesmo arquivo com o
  `zipfile` do Python**, deixa ele reconferir todo CRC e compara o conteúdo com
  o manifesto que o lado C++ emitiu. Mutação mostra que os testes são sensíveis;
  a leitura independente mostra que o **formato está certo** — se o firmware
  discordar do resto do mundo sobre o que é um ZIP, nenhuma quantidade de
  mutação diz isso, porque mutação só testa os meus testes contra o meu código.
  Pulado em silêncio sem `python3`, igual aos sanitizers.
  A invariante que carrega o peso é a das **duas passadas**: o writer não guarda
  estado por entrada (bufferizar o diretório central são ~64 KB para 1000 notas),
  então a segunda passada **recomputa** cada offset em vez de lembrá-lo, e o
  teste compara os offsets das duas e confirma que cada um aponta para uma
  assinatura `PK\3\4` real na saída. Se divergirem, o arquivo *parece* bom e o
  leitor acusa corrupção em notas que foram escritas corretamente
- `test_note_html.cpp` + `test/check-page.py` → **a página de notas** servida no
  hotspot (`src/note_html.cpp` + a chrome literal de `src/web_page.h`). Mesmo
  arranjo de dois portões do ZIP, e pelo mesmo motivo: o `.cpp` afere as linhas
  contra o que eu espero, e o `check-page.py` lê as **três páginas que o handler
  pode produzir** (cheia, vazia, cartão ilegível) com o parser de HTML do
  Python. Uma tag não fechada dentro daquele `<style>` é uma página branca no
  celular de alguém e **nada** neste aparelho, que não tem como servir uma
  página para um validador.
  A propriedade que carrega o peso é que os **dois guardas ficam independentes**:
  o `noteUriNameIsSafe()` é sobre o **cartão** — recusa `/`, `\`, `..`, `%` e
  bytes de controle, e aceita `<`, `>`, `&` e aspas de bom grado, porque nada
  disso machuca um filesystem. A página é sobre **marcação**. Então o teste varre
  os 255 bytes e confirma que todo byte que a whitelist de href admite é inerte
  em HTML: se alguém alargar essa whitelist, quebra aqui e não no navegador de
  um estranho. Mais o mesmo negativo do JSON — linha que não cabe é recusada
  **inteira**, porque meio `<li>` leva o resto da lista — e o pior caso que o
  cartão consegue produzir (nome de 255 bytes, título de 64 `&`) medido contra
  `NOTE_HTML_ROW_MAX`: 639 de 704
- `test_confirm.cpp` → o diálogo de confirmação que guarda as duas ações
  irreversíveis da UI (apagar nota, esquecer pareamento). A propriedade que
  carrega o peso é negativa: **nenhuma tecla isolada, apertada uma vez numa
  caixa recém-aberta, destrói coisa alguma** — varrido sobre 20 teclas × 5
  combinações de modificador. Mais: a caixa é modal de verdade (tecla estranha
  é engolida, não fecha nem vaza para a tela de trás), a linha sobre a qual ela
  perguntou sobrevive à conversa inteira, e as duas saídas fecham a caixa, de
  modo que a resposta só pode ser entregue uma vez

Isso existe porque o X4 não tem console serial e cada teste no aparelho custa uma
gravação no SD mais um reboot. `src/deadkeys.*` foi separado de `input_handler`
**exatamente** para ficar livre de Arduino e testável aqui. Mantenha assim: lógica
nova que não dá para exercitar no host é lógica que não dá para exercitar.

Requer `gcc-c++` (`dnf install gcc-c++`). ASan/UBSan entram sozinhos se
`libasan`/`libubsan` estiverem instalados, e são pulados em silêncio se não.

`test/` fica fora do build do firmware (`src/CMakeLists.txt` faz glob só de
`src/`). Não rode `pio test` — a suíte não usa Unity.

**Suíte validada por mutação**: 222 mutações no total (21 em pt-br, 21 no sleep,
10 no ladder dos botões, 6 na fuga generalizada,
15 na recuperação embutida, 7 nos nomes de slot OTA, 13 no JSON da lista,
25 no writer de ZIP, 22 nos atalhos e linhas de menu, 7 nas abas do Settings, 15 no layout, 3 na
caixa modal, 12 na rolagem, 14 no sidecar, 5 em `deleteOpenNote()`, 6 na rotação
do d-pad, 9 no diálogo de confirmação, 11 no menu rápido), cada uma revertendo
um conserto real — incluindo reverter o mapa ABNT2 para o US, reverter o meio-tom
para o preto sólido e devolver o `textYInBand` ao topo da faixa, que são os três
bugs originais. Todas detectadas, exceto **nove mutantes equivalentes**:

- remover o atalho `density >= 16` cai em `bayer < 16`, e como Bayer vai só até
  15 o resultado é idêntico — o atalho é otimização, não comportamento;
- afrouxar o piso de `menuDigit()` de `hid < HID_KEY_1` para `hid < HID_KEY_1 - 1`
  só muda o usage `0x1D`, e lá a aritmética já dá `(0x1D - 0x1E) + 1 == 0`, que
  é exatamente o "não é atalho" que a checagem de faixa devolveria;
- os **dois clamps** de `textYInBandOf()` nunca disparam: sob o guarda
  `bandH >= ink.height()`, a fórmula de centralização já devolve um `y` dentro
  de `[yMin, yMax]` por construção. Verificado por força bruta em 34M de
  combinações. Ficam de pé porque é a fórmula que os torna mortos, e a fórmula
  é justamente o que uma mudança de layout mexeria;
- os **dois guardas de faixa** de `scrollStartOf()` e o **clamp inferior** de
  `scrollThumbOf()`: nenhum dispara para entrada válida (1,1M de formas de lista
  varridas, zero acertos). Ficam como absorção de uma seleção fora de faixa, que
  é bug de chamador e não deve virar índice negativo nem janela pendurada fora
  da lista;
- afrouxar `len <= 4` para `len < 4` em `noteJsonIncludes()`: o único nome de 4
  bytes terminando em `.txt` **é** `.txt`, que a checagem de arquivo oculto já
  rejeita antes. Provado por enumeração, não por argumento;
- o guarda do **próprio** buffer de `noteJsonEntry()`
  (`written >= sizeof(entry)`) é inalcançável: o `jsonAppendStr` limita o nome
  escapado ao buffer dele (≤512), então a entrada chega no máximo a
  1+9+512+9+20+1 = 552 contra os 576 do `NOTE_JSON_ENTRY_MAX`. Fica de pé porque
  é o `NOTE_JSON_ENTRY_MAX` que o torna morto, e esse número é justamente o que
  uma mudança de formato mexeria.

Escapar nesses sete é o esperado, não falha de teste.

A mutação da rolagem pagou na hora: dois **buracos de teste reais** apareceram
junto — o teto de altura do thumb, que meu sweep não exercitava porque começava
em trilhas de 20px enquanto o mínimo do thumb é 12, e a posição do thumb no meio
do percurso, que passava mesmo com a fórmula errada porque eu só conferia os dois
extremos. Ambos fechados e remutados.

A do sidecar pagou três, todos pela mesma razão: eu estava afirmando
`noteVisibleCount() == 1`, e **a lista de notas se reconstrói da pasta de
qualquer jeito** — então a asserção passava com o índice corrompido sendo lido,
com uma versão futura sendo mal-interpretada e com um registro truncado sendo
aceito. O observável que discrimina é o **índice reescrito**: um índice
descartado é reconstruído com exatamente as notas que existem no cartão, um
índice acreditado carrega os fantasmas para dentro dele. Trocado por isso, os
três morreram.

A de `deleteOpenNote()` existe por causa da armadilha oposta: apagar a nota que
está **aberta**. O autosave grava a cada dez segundos de ociosidade, e o Esc e o
botão de power gravam ao sair — então um buffer ainda sujo com o nome do arquivo
ainda setado **ressuscita a nota** segundos depois de apagá-la. A função desarma
o editor antes de remover o arquivo, e dois dos cinco mutantes são exatamente
essa ressurreição: deixar o nome setado, e não limpar o editor.

A do ladder dos botões pagou na hora: trocar a mediana pelo **máximo** de três
sobreviveu à primeira passada. Todos os casos que eu tinha escrito alimentavam
um valor espúrio **baixo**, e um máximo rejeita esses igualmente bem — os dois
só divergem quando um botão está genuinamente apertado e uma conversão volta em
repouso, onde o máximo perde a apertada e a mediana a mantém. Nada medido no
aparelho diz que uma conversão ruim só pode ler baixo; diz que as que causaram
fantasma leram baixo, que é outra afirmação. Caso simétrico acrescentado nos dois
canais, mutante morto.

A da recuperação embutida pagou uma: ignorar o **nome do projeto** ao comparar
builds sobreviveu, porque todos os pares que eu tinha escrito diferiam nos dois
campos — faltava o caso de dois projetos **diferentes** carregando a **mesma**
string de versão, que não é contrived: é o estado default de dois projetos
versionados à mão, ambos em "1.0.0". Caso acrescentado, mutante morto.

A dos nomes de slot pagou uma: largar a checagem explícita de "um projeto foi
gravado" sobreviveu, porque em todos os casos que eu tinha escrito o `strcmp`
pegava a diferença de qualquer jeito. O caso que discrimina é os **dois**
projetos vazios — aí `strcmp("", "")` concorda e só a checagem de vazio separa
"verificado igual" de "os dois em branco". Alcançável: um firmware compilado sem
nome de projeto, com um apelido em NVS de antes do rastreamento. Caso
acrescentado, mutante morto.

A do JSON da lista pagou uma, e também pagou **falsos negativos no método**:
quatro `sed` de mutação não casaram o padrão (um deu erro de sintaxe) e
reportaram "sobreviveu" com o arquivo intacto. Mutação por `sed` em C++ com
escape é frágil; refeita em Python com asserção de que a substituição aconteceu,
os quatro morreram. **Mutação que não muda o código não testa nada, e mente
dizendo que o teste é fraco.** O buraco real era comparar só `.tx` em vez de
`.txt`: todo nome rejeitado nos meus casos diferia bem antes do último byte, e o
que discrimina é um nome a uma letra de distância — `archive.txz` num cartão onde
as pessoas soltam arquivos à mão.

A do writer de ZIP matou todas as 25 na primeira passada, o que é incomum aqui e
vem da forma de dois portões: polinômio de CRC trocado, `deflate` em vez de
`stored`, bit 11 de UTF-8 desligado, assinatura errada, `put16` big-endian,
`zipLocalEntrySize` esquecendo o header ou o nome, `cdStart` tarde, falha de sink
não grudenta, e o somador de tamanho esquecendo o registro central, o nome
dentro dele, o registro local ou o registro final — cada uma dessas quebra uma
asserção concreta. **Mas o portão do
Python não é mutação**: ele cobre o eixo que mutação não alcança, que é eu ter
lido a especificação errado. Os dois não se substituem.

E ele pagou um bug **no teste**: o corpo `"uma\r\nduas\nzero\0byte"` passado como
`const char*` para `std::string` **para no NUL** — eram 14 bytes, não 19, e o
caso que o comentário prometia (um NUL dentro do corpo) nunca era exercitado.
Corpo de nota é bytes, não string C. Trocado por comprimento explícito, e o CRC
mudou junto, que é como se sabe que agora está de fato ali.

A mutação já pegou seis defeitos **nos testes**, não no firmware: uma asserção de cima/baixo que passava
com o conserto revertido (subir *de* uma linha ASCII é insensível por construção —
ali offset em byte e coluna visual são o mesmo número; o caso que discrimina é
*sair* de uma linha acentuada); um teste que esperava "ção" de `ç ´ o`, quando a
sequência certa é `ç ~ a o`; e um teste de boot frio do slideshow que usava
`last = 99999` com 4 imagens, onde `(99999+1) % 4` dá 0 — o mesmo que a resposta
certa, então passava pelo motivo errado; e um teste de meio-tom que exigia
contagem igual de pontos por linha, coisa que matriz de Bayer não promete (na
densidade 12 ela dá 4,2,4,2, e está certa) — trocado por dispersão e
aninhamento, que são as propriedades que realmente importam.

Ao mexer nesses testes, refaça a mutação — um teste que nunca falha não testa.

---

## Backlog

- **Remover o instrumento de heap do hotspot** (`apHttpStatusText()` em
  `src/wifi_sync.cpp` e sua linha em `drawSyncScreen()`; o `bleStackIsUp()` que
  ficava ao lado já não existe). Vale enquanto a exportação está em obra — a
  página do P3 é a última peça que cobra heap; não vale num release.

- AltGr no ABNT2 (`¹²³£¢¬ºª°`) — acrescentar casos em `abnt2Key()`
- Port para o X3 — **moderado**, não difícil. O FreeInk SDK já tem perfil do X3
  (`DisplayController::UC8253`, variante UC8279 por probe), o `XteinkDetect`
  distingue os dois por fingerprint I²C, e a entrada é idêntica. O trabalho é
  trocar o `lib/hal/` próprio pelo SDK (`BoardConfig` + `FreeInkDisplay`), tirar
  o `480` hardcoded do `EInkDisplay.cpp` e revisar offsets verticais nas 1050
  linhas do `ui_renderer.cpp`. Estimativa: 2–3 fins de semana. As libs
  vendorizadas do Ardosia **já são** o FreeInk SDK — as assinaturas de
  `InputManager` e `SDCardManager` batem exatamente.
- PR upstream com os bugs 1–3
- Remover `sdMigrateLegacyBackupDir()` de `src/sd_backup.h` — migração de uma vez
  só da pasta de backup `/microslate` → `/ardosia`, temporária por construção
- **Remover `src/dev_wipe.*`** e a chamada `devWipeOtherSlotIfArmed()` no
  `setup()` — ferramenta de teste que faz o outro slot ler como vazio, para
  reproduzir o estado de instalação nova. Armada só por um arquivo no cartão
  (`/ARDOSIA-WIPE-OTHER-SLOT`), roda uma vez, autolimitante. **Não pode ir para
  o release.**

---

## Convenções deste fork

- Comentar **por que**, não o quê — principalmente onde a escolha contraria o
  upstream ou depende de hardware específico
- Marcar explicitamente o que é diagnóstico temporário e o que é permanente
- Anotar valores originais ao alterar configuração, para reverter sem depender de
  memória
- Não filtrar informação da UI de forma irreversível em aparelho sem USB
