# Sistema de Irrigação Automatizada com ESP32

Projeto de irrigação automatizada desenvolvido com o microcontrolador **ESP32**, com duas versões de comunicação: **Wi-Fi**, utilizando uma interface web, e **Bluetooth Clássico**, utilizando um terminal serial.

Os dois programas realizam a leitura da umidade do solo, o controle de um LED por PWM e o acionamento de uma bomba d'água por relé. O usuário pode selecionar o modo de irrigação manual ou automático e consultar informações sobre o funcionamento do sistema.

## Arquivos do projeto

| Arquivo | Descrição |
| --- | --- |
| `codigo_wifi_comentado.ino` | Versão com ponto de acesso Wi-Fi e página web para monitoramento e controle. |
| `codigo_bluetooth_comentado.ino` | Versão com Bluetooth Clássico e comandos enviados por terminal serial. |

> Cada arquivo é um programa independente. Carregue **apenas uma versão por vez** no ESP32. 

## Componentes e conexões

O código considera os seguintes componentes:

- ESP32 com suporte aos recursos utilizados;
- sensor analógico de umidade do solo;
- LED e resistor de 240 Ω;
- módulo relé e transistor NPN (com resistor de base de 1 kΩ e resistor de 10kΩ como pulldown) para acionamento;
- bomba d'água e fonte de alimentação adequada.

| Elemento | Pino do ESP32 | Função |
| --- | --- | --- |
| Sensor de umidade | GPIO 35 | Leitura analógica (ADC) |
| Relé, via transistor NPN | GPIO 26 | Comando de acionamento da bomba |
| LED, via resistor de 240 Ω | GPIO 5 | Controle de brilho por PWM |

**Acionamento do relé:** os programas foram escritos para um circuito com transistor NPN e módulo relé acionado em nível baixo na entrada `IN`. O sinal `HIGH` (3,3 V) do GPIO 26 faz o transistor conduzir, puxa `IN` para 0 V e aciona o relé; o sinal `LOW` (0 V) corta o transistor e desliga o acionamento. A ligação deve seguir o circuito efetivamente montado, incluindo resistor de base de 1 kΩ. Não conecte a bomba diretamente a um GPIO do ESP32; verifique também a alimentação, o terra comum onde necessário e os limites elétricos do módulo relé.

## Funcionamento

### Leitura da umidade e LED

A leitura analógica do sensor é convertida em umidade percentual por `rawToHumidity()`, utilizando os parâmetros iniciais de calibração:

```cpp
#define VAL_SECO 4000
#define VAL_MOLHADO 1000
```

Esses valores devem ser ajustados a partir de medições reais do sensor. O LED indica visualmente a condição do solo: quanto menor a umidade, maior o brilho. A saída utiliza PWM de **5 kHz**, com resolução de **8 bits**; a partir de **80% de umidade**, o LED permanece apagado.

### Modos de irrigação

**Manual:** permite ligar a bomba por **2 ou 4 segundos** e interromper a irrigação antes do término. Os comandos de acionamento manual não são executados no modo automático.

**Automático:** utiliza os seguintes parâmetros, definidos nos dois códigos:

| Parâmetro | Valor |
| --- | --- |
| Umidade para iniciar a irrigação | Abaixo de 35% |
| Umidade para interromper a irrigação | A partir de 60% |
| Tempo máximo contínuo da bomba | 4 segundos |
| Intervalo mínimo entre ciclos automáticos | 4 segundos |
| Intervalo de leitura do sensor e avaliação automática | 1 segundo |

Os limites de tempo são tratados por `handleTimersAndSafety()`, enquanto `handleAutomaticIrrigation()` avalia as condições para irrigação automática.

O volume de água é **estimado** pelo tempo de acionamento e por uma vazão de referência de `0,25 mL/s` (`~0,9 L/h`). Os programas não medem a vazão real da bomba; portanto, o volume exibido depende de calibração física. O estado da bomba apresentado nas interfaces corresponde ao **estado comandado pelo software**, não a uma confirmação independente de fluxo de água.

## Versão Wi-Fi

A versão Wi-Fi utiliza as bibliotecas `WiFi.h` e `WebServer.h`. O ESP32 cria um **ponto de acesso próprio**, sem depender de um roteador externo, e disponibiliza a interface pela porta HTTP 80.

**Configuração presente no código:**

```cpp
#define AP_SSID "Irrigação feliz"
#define AP_PASS "paciencia123"
```

Para utilizar:

1. Carregue `codigo_wifi_comentado.ino` no ESP32.
2. Conecte um celular ou computador à rede **Irrigação feliz**, utilizando a senha definida no código.
3. Abra um navegador e acesse o endereço IP mostrado no Monitor Serial do ESP32 (115200 baud).
4. Utilize os botões da página para consultar a umidade, alternar o modo de funcionamento e controlar a irrigação manual.

O programa atende às rotas `GET /`, `POST /water2`, `POST /water4`, `POST /stop` e `POST /mode`. A interface é montada por `buildPage()` e possui **atualização automática a cada 3 segundos**, definida no HTML.

> O nome e a senha da rede são valores de demonstração definidos diretamente no código. Altere-os caso necessário antes de utilizar o sistema em outro ambiente.

## Versão Bluetooth

A versão Bluetooth utiliza `BluetoothSerial.h` para comunicação por **Bluetooth Clássico (SPP)**. Requer uma placa ESP32 com suporte a Bluetooth Clássico e uma configuração de placa que habilite esse recurso; o próprio código impede a compilação em configurações sem o suporte necessário, como ESP32-S3 ou ESP32-C3.

O dispositivo é anunciado com o nome:

```cpp
#define BT_DEVICE_NAME "Irrigacao feliz"
```

Para utilizar:

1. Carregue `codigo_bluetooth_comentado.ino` em um ESP32 compatível.
2. Faça o pareamento com **Irrigacao feliz** em um dispositivo e aplicativo terminal compatíveis com Bluetooth Clássico SPP.
3. No terminal, configure o envio dos comandos com **quebra de linha (`\n`)**.
4. Envie os comandos abaixo para controlar o sistema.

| Comando | Ação |
| --- | --- |
| `2` ou `regar2` | Irrigar por 2 segundos no modo manual |
| `4` ou `regar4` | Irrigar por 4 segundos no modo manual |
| `0`, `parar` ou `stop` | Interromper a irrigação manual |
| `m` ou `modo` | Alternar entre os modos manual e automático |
| `s` ou `status` | Exibir as informações do sistema |
| `?`, `ajuda` ou `help` | Exibir os comandos disponíveis |

Os mesmos comandos também podem ser enviados pelo **Monitor Serial USB**, configurado em **115200 baud**, com envio de nova linha. A cada **5 segundos**, quando um cliente Bluetooth está conectado, o programa envia um resumo com umidade, modo, estado comandado da bomba e PWM do LED.

## Comparação das implementações

| Característica | Wi-Fi | Bluetooth |
| --- | --- | --- |
| Biblioteca principal | `WiFi.h` e `WebServer.h` | `BluetoothSerial.h` |
| Comunicação | Ponto de acesso e requisições HTTP | Comunicação serial Bluetooth Clássico |
| Interface | Página web com botões e indicadores | Terminal de texto |
| Recebimento de comandos | Rotas HTTP tratadas pelo servidor | `handleBluetoothAndSerialInputs()` e `processCommand()` |
| Atualização para o usuário | Recarregamento da página a cada 3 s | Resumo enviado a cada 5 s, com cliente conectado |
| Lógica central de irrigação | Sensor, PWM, bomba e temporizadores | Mesmas condições de controle |

As duas versões mantêm a lógica principal de irrigação. As alterações estão concentradas no recebimento e processamento dos comandos, na interface com o usuário e no registro e envio das mensagens do sistema.

## Ambiente de desenvolvimento

Os programas foram escritos para o **framework Arduino no ESP32** e incluem tratamento para as diferenças de API de PWM entre as versões **2.x e 3.x** do pacote Arduino-ESP32. As bibliotecas de comunicação utilizadas fazem parte do ambiente Arduino-ESP32 nas configurações de placa compatíveis.

Antes de carregar o programa, selecione a placa ESP32 correta, verifique a porta serial e confirme as conexões do circuito. Utilize o Monitor Serial em **115200 baud** para acompanhar os registros de operação.

## Observações

- A calibração do sensor (`VAL_SECO` e `VAL_MOLHADO`) deve ser realizada conforme o dispositivo utilizado.
- A vazão da bomba (`0,25 mL/s`) é um valor estimado, não uma medição direta.
- O acionamento do relé depende do circuito de transistor NPN previsto nos códigos.
- Os parâmetros de irrigação e os tempos de segurança podem ser alterados nas constantes de configuração dos programas.
- As duas implementações representam alternativas de comunicação para o mesmo sistema, não programas destinados a funcionar simultaneamente.
