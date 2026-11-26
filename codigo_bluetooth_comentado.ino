//Configurações do projeto
//Inclui as bibliotecas responsáveis pelo funcionamento do ESP32 e pela comunicação Bluetooth
#include <BluetoothSerial.h>


// Verifica se a placa selecionada suporta Bluetooth Classico (ESP32 padrao)
#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error "Bluetooth Classico nao habilitado nesta placa/configuracao (requer ESP32 padrao, nao S3/C3)."
#endif

//Configuração do nível lógico utilizado para ligar e desligar a bomba pelo transistor NPN
// Transistor NPN conectado ao pino RELAY_PIN via resistor de base (1 kOhm):
// - RELAY_PIN = HIGH (3.3V) -> transistor conduz -> puxa IN do rele para 0V -> LIGA bomba.
// - RELAY_PIN = LOW  (0V)   -> transistor corta   -> IN do rele sobe para 5V -> DESLIGA bomba.
constexpr uint8_t RELAY_ON_LEVEL  = HIGH;
constexpr uint8_t RELAY_OFF_LEVEL = (RELAY_ON_LEVEL == HIGH) ? LOW : HIGH;

//Configuração dos pinos do sensor, do relé e do LED

#define SENSOR_PIN 35
#define RELAY_PIN 26  

// GPIO5 -> resistor de 330 ohms -> anodo do LED.
// Catodo do LED -> GND.
//Configurações do PMW do led 
#define LED_PWM_PIN 5
#define LED_PWM_CHANNEL 0
#define LED_PWM_FREQ_HZ 5000
#define LED_PWM_RESOLUTION_BITS 8
#define LED_PWM_MAX_DUTY ((1U << LED_PWM_RESOLUTION_BITS) - 1U)

// ==================================================
// Calibracao do sensor de úmidade 
// ==================================================

#define VAL_SECO 4000
#define VAL_MOLHADO 1000

//Configuração dos limites de umidade e dos tempos de segurança da irrigação automática

#define AUTO_START_HUMIDITY 35
#define AUTO_STOP_HUMIDITY 60
#define MAX_PUMP_TIME 4000UL   // Tempo maximo continuo ligado (4s)
#define AUTO_COOLDOWN 4000UL   // Intervalo minimo entre ciclos automaticos (4s)

// Vazao estimada da bomba.
// 0.25 ml/s 
constexpr float PUMP_FLOW_ML_PER_SECOND = 0.25f;

//Configuração do nome utilizado para identificar o ESP32 na conexão Bluetooth

#define BT_DEVICE_NAME "Irrigacao feliz"

//Declaração dos objetos e das variáveis utilizadas para armazenar as leituras e os estados do sistema

BluetoothSerial SerialBT;

int rawValue = 0;
int humidity = 0;

unsigned int ledDuty = 0;

bool pumpOn = false;
bool autoMode = false;

bool manualTimedWatering = false;
unsigned long manualStartTime = 0;
unsigned long manualDurationMs = 0;

String lastPumpReason = "Sistema pronto";
String humanMessage = "Me cuido sozinho";

unsigned long lastSensorRead = 0;
unsigned long lastBtTelemetry = 0;
unsigned long pumpStartTime = 0;
unsigned long lastAutoStopTime = 0;
bool hasAutoRunOnce = false;

float totalWaterMl = 0.0f;
float lastWaterMl = 0.0f;
unsigned long irrigationCount = 0;

//Helpers de log e hardware

//Converte o nível lógico do pino em texto para exibição no monitor serial.
const char* levelToStr(uint8_t level) {
  return (level == HIGH) ? "HIGH" : "LOW";
}

// Envia mensagens importantes tanto para o Serial USB quanto para o app Bluetooth
//Exibe as mensagens de funcionamento no monitor serial e também no dispositivo Bluetooth quando existe um cliente conectado.
void logEvent(const String& msg) {
  Serial.println(msg);
  if (SerialBT.hasClient()) {
    SerialBT.println(msg);
  }
}

//Envia o nível lógico para o pino do relé e exibe o estado comandado da bomba.
void applyRelayLevel(uint8_t level) {
  digitalWrite(RELAY_PIN, level);
  String logMsg = "[GPIO" + String(RELAY_PIN) + "] Nivel comandado: " + String(levelToStr(level)) +
                  (level == RELAY_ON_LEVEL
                     ? " (Transistor ON -> IN=0V / Rele LIGADO)"
                     : " (Transistor OFF -> IN=5V / Rele DESLIGADO)");
  Serial.println(logMsg);
}

//Sensor e estados

//Converte a leitura analógica do sensor em um valor de umidade de 0 a 100%, utilizando os valores definidos na calibração.
int rawToHumidity(int raw) {
  int h = map(raw, VAL_SECO, VAL_MOLHADO, 0, 100);
  return constrain(h, 0, 100);
}

//Classifica a umidade do solo em faixas e retorna uma descrição em texto para o monitor serial e o Bluetooth.
String getSoilStateText() {
  if (humidity < 25) return "Seco";
  if (humidity < 40) return "Baixa";
  if (humidity <= 60) return "Ideal";
  if (humidity <= 75) return "Muito umido";
  return "Encharcado";
}

//Identifica a situação do sistema e retorna uma mensagem de acordo com a umidade, o modo selecionado e o estado comandado da bomba.
String getHumanMessage() {
  if (autoMode) {
    if (pumpOn) return "Regando (comandado)";
    return "Irrigacao automatica";
  }

  if (pumpOn) return "Irrigacao manual (comandada)";

  if (humidity < 25) return "Precisa de agua";
  if (humidity < 40) return "Umidade baixa";
  if (humidity <= 60) return "Umidade ideal";
  if (humidity <= 75) return "Muito umido";
  return "Solo encharcado";
}

// ==================================================
// LED PWM (Alerta visual: mais seco = mais brilho; apaga >= 80% de úmidade)
// Compativel com Arduino-ESP32 2.x e 3.x.
// ==================================================

//Configura o pino do LED para utilizar PWM com frequência de 5 kHz e resolução de 8 bits. Considera as diferenças entre as versões 2.x e 3.x do ESP32.
bool setupHumidityLed() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  if (!ledcAttach(
        LED_PWM_PIN,
        LED_PWM_FREQ_HZ,
        LED_PWM_RESOLUTION_BITS)) {
    return false;
  }

  ledcWrite(LED_PWM_PIN, 0);
#else
  if (ledcSetup(
        LED_PWM_CHANNEL,
        LED_PWM_FREQ_HZ,
        LED_PWM_RESOLUTION_BITS) == 0) {
    return false;
  }

  ledcAttachPin(LED_PWM_PIN, LED_PWM_CHANNEL);
  ledcWrite(LED_PWM_CHANNEL, 0);
#endif

  return true;
}

//Atualiza o brilho do LED de acordo com a umidade do solo. Quanto menor a umidade, maior o brilho; a partir de 80%, o LED permanece apagado.
void updateHumidityLed() {
  int h = constrain(humidity, 0, 100);

  if (h >= 80) {
    // Em 80% de umidade ou mais, apaga completamente o LED.
    ledDuty = 0;
  } else {
    // De 0% (seco) ate 79%, reduz o brilho proporcionalmente de 255 ate proximo de 0.
    unsigned int drynessRange = 80U - (unsigned int)h;
    ledDuty = (drynessRange * LED_PWM_MAX_DUTY + 40U) / 80U;
  }

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(LED_PWM_PIN, ledDuty);
#else
  ledcWrite(LED_PWM_CHANNEL, ledDuty);
#endif
}

//Realiza a leitura do sensor de umidade, converte o valor para porcentagem e atualiza o LED e a mensagem do sistema.
void readSensor() {
  rawValue = analogRead(SENSOR_PIN);
  humidity = rawToHumidity(rawValue);

  updateHumidityLed();
  humanMessage = getHumanMessage();
}

//Bomba e registro de agua

//Calcula o volume estimado de água utilizado a partir do tempo de acionamento da bomba. Atualiza o volume acumulado e o número de irrigações.
void registerWateringSession(unsigned long durationMs) {
  float seconds = durationMs / 1000.0f;

  lastWaterMl = seconds * PUMP_FLOW_ML_PER_SECOND;
  totalWaterMl += lastWaterMl;
  irrigationCount++;
}

//Controla o acionamento e o desligamento da bomba pelo relé. Registra o motivo da mudança de estado e, ao desligar, atualiza os dados da irrigação.
void setPump(bool state, const String& reason = "", bool forceHardwareSync = false) {
  // Evita retrabalho se o estado ja for o desejado, exceto quando forceHardwareSync = true
  if (state == pumpOn && !forceHardwareSync) {
    if (reason.length() > 0) {
      lastPumpReason = reason;
    }
    logEvent("[BOMBA] Comando redundante ignorado. Estado ja estava em: " +
             String(pumpOn ? "LIGADA (comandado)" : "DESLIGADA (comandado)"));
    return;
  }

  unsigned long now = millis();

  if (state) {
    pumpOn = true;
    pumpStartTime = now;

    logEvent("[BOMBA] Transicao -> LIGADA (comandado) | Motivo: " +
             (reason.length() > 0 ? reason : String("Nao especificado")));
    applyRelayLevel(RELAY_ON_LEVEL);
  } else {
    unsigned long durationMs = pumpOn ? (now - pumpStartTime) : 0;
    bool wasOn = pumpOn;

    pumpOn = false;
    manualTimedWatering = false;
    manualDurationMs = 0;

    logEvent("[BOMBA] Transicao -> DESLIGADA (comandado) | Motivo: " +
             (reason.length() > 0 ? reason : String("Nao especificado")));
    applyRelayLevel(RELAY_OFF_LEVEL);

    if (wasOn) {
      if (durationMs > 300UL) {
        registerWateringSession(durationMs);
      }
      if (autoMode) {
        lastAutoStopTime = now;
        hasAutoRunOnce = true;
      }
    }
  }

  if (reason.length() > 0) {
    lastPumpReason = reason;
  }

  humanMessage = getHumanMessage();
}

//Inicia a irrigação manual pelo tempo solicitado. Verifica se o modo manual está ativo e limita o tempo ao valor máximo de segurança.
void startManualWatering(unsigned long seconds) {
  if (autoMode) {
    logEvent("[MANUAL] Comando ignorado: sistema esta em modo AUTOMATICO.");
    return;
  }

  if (pumpOn) {
    logEvent("[MANUAL] Comando ignorado: bomba ja esta com comando ativo.");
    return;
  }

  // Garante que o tempo manual nunca ultrapasse o limite de seguranca
  unsigned long requestedMs = seconds * 1000UL;
  if (requestedMs > MAX_PUMP_TIME) {
    requestedMs = MAX_PUMP_TIME;
  }

  manualTimedWatering = true;
  manualStartTime = millis();
  manualDurationMs = requestedMs;

  String reason = "Manual: " + String(requestedMs / 1000UL) + "s";
  setPump(true, reason);
}

//Verifica se o tempo de irrigação manual terminou ou se a bomba atingiu o tempo máximo de funcionamento. Desliga a bomba quando necessário.
void handleTimersAndSafety() {
  unsigned long now = millis();

  // 1. Verificacao de desligamento manual temporizado (seguro contra rollover)
  if (!autoMode && manualTimedWatering && pumpOn) {
    if ((now - manualStartTime) >= manualDurationMs) {
      setPump(false, "Manual: tempo concluido");
      return;
    }
  }

  // 2. Protecao global de tempo maximo continuo (vale para manual e automatico)
  if (pumpOn && (now - pumpStartTime) >= MAX_PUMP_TIME) {
    setPump(false, "Seguranca: corte tempo maximo (" + String(MAX_PUMP_TIME / 1000UL) + "s)");
    return;
  }
}

//Controla a irrigação automática com base na umidade do solo. Liga a bomba abaixo de 35%, desliga ao atingir 60% e respeita o intervalo mínimo entre ciclos.
void handleAutomaticIrrigation() {
  if (!autoMode) return;

  unsigned long now = millis();

  if (!pumpOn) {
    bool soilNeedsWater = humidity < AUTO_START_HUMIDITY;
    // Na primeira execucao em modo automatico nao exige espera de cooldown
    bool cooldownFinished = (!hasAutoRunOnce) || ((now - lastAutoStopTime) >= AUTO_COOLDOWN);

    if (soilNeedsWater && cooldownFinished) {
      setPump(true, "Auto: umidade baixa (< " + String(AUTO_START_HUMIDITY) + "%)");
    }
  } else {
    bool humidityRecovered = humidity >= AUTO_STOP_HUMIDITY;
    if (humidityRecovered) {
      setPump(false, "Auto: umidade alvo atingida (>= " + String(AUTO_STOP_HUMIDITY) + "%)");
    }
  }
}

//Funções responsáveis pelos comandos e pelas informações enviadas por Bluetooth

//Envia para o dispositivo conectado a lista de comandos disponíveis para controlar e consultar o sistema.
void sendBluetoothHelp() {
  if (!SerialBT.hasClient()) return;

  SerialBT.println("\n=== COMANDOS - REGA DO BONSAI ===");
  SerialBT.println("2      -> Regar por 2s (modo manual)");
  SerialBT.println("4      -> Regar por 4s (modo manual)");
  SerialBT.println("parar  -> Parar rega manual (ou digite 0)");
  SerialBT.println("modo   -> Alternar Manual / Automatico (ou m)");
  SerialBT.println("status -> Ver leitura e estado atual (ou s)");
  SerialBT.println("ajuda  -> Exibir esta lista (ou ?)");
  SerialBT.println("=================================\n");
}

//Envia para o dispositivo conectado as leituras de umidade, o brilho do LED, o modo de operação e os registros estimados da irrigação.
void sendBluetoothStatus() {
  if (!SerialBT.hasClient()) return;

  float estimatedLitersPerHour = (PUMP_FLOW_ML_PER_SECOND * 3600.0f) / 1000.0f;

  SerialBT.println("\n--- STATUS DO BONSAI ---");
  SerialBT.print("Umidade do solo: ");
  SerialBT.print(humidity);
  SerialBT.print("% (");
  SerialBT.print(getSoilStateText());
  SerialBT.print(") | RAW: ");
  SerialBT.println(rawValue);

  SerialBT.print("Situacao: ");
  SerialBT.println(humanMessage);

  SerialBT.print("LED PWM (GPIO");
  SerialBT.print(LED_PWM_PIN);
  SerialBT.print("): ");
  SerialBT.print(ledDuty);
  SerialBT.println("/255");

  SerialBT.print("Modo: ");
  SerialBT.print(autoMode ? "AUTOMATICO" : "MANUAL");
  SerialBT.print(" | Bomba (cmd, sem sensor de fluxo): ");
  SerialBT.println(pumpOn ? "LIGADA" : "DESLIGADA");

  SerialBT.print("Ultimo evento: ");
  SerialBT.println(lastPumpReason);

  SerialBT.print("Registro estimado: ");
  SerialBT.print(totalWaterMl / 1000.0f, 2);
  SerialBT.print(" L total | Ultima: ");
  SerialBT.print(lastWaterMl, 0);
  SerialBT.print(" ml | Ciclos: ");
  SerialBT.print(irrigationCount);
  SerialBT.print(" (base sem calibracao: ");
  SerialBT.print(PUMP_FLOW_ML_PER_SECOND, 2);
  SerialBT.print(" ml/s ~");
  SerialBT.print(estimatedLitersPerHour, 1);
  SerialBT.println(" L/h)");
  SerialBT.println("------------------------\n");
}

//Identifica o comando recebido e executa a ação correspondente: irrigar, parar a bomba, alternar o modo, consultar o estado ou exibir a ajuda.
void processCommand(String cmd) {
  cmd.trim();
  cmd.toLowerCase();
  if (cmd.length() == 0) return;

  Serial.print("[CMD] Comando recebido: ");
  Serial.println(cmd);

  if (cmd == "2" || cmd == "regar2") {
    startManualWatering(2);
  } else if (cmd == "4" || cmd == "regar4") {
    startManualWatering(4);
  } else if (cmd == "0" || cmd == "parar" || cmd == "stop") {
    if (!autoMode && pumpOn) {
      setPump(false, "Manual: rega interrompida pelo usuario");
    } else {
      logEvent("[CMD] 'parar' ignorado: bomba ja desligada ou sistema em modo automatico.");
    }
  } else if (cmd == "m" || cmd == "modo") {
    autoMode = !autoMode;
    if (autoMode) {
      hasAutoRunOnce = false;
      setPump(false, "Modo automatico ativado", true);
    } else {
      setPump(false, "Modo manual ativado", true);
    }
    humanMessage = getHumanMessage();
    sendBluetoothStatus();
  } else if (cmd == "s" || cmd == "status") {
    sendBluetoothStatus();
  } else if (cmd == "?" || cmd == "ajuda" || cmd == "help") {
    sendBluetoothHelp();
  } else {
    logEvent("[CMD] Comando desconhecido: '" + cmd + "'. Envie 'ajuda' para ver as opcoes.");
  }
}

//Verifica se existem comandos disponíveis no Bluetooth ou no monitor serial USB e encaminha o texto recebido para processamento.
void handleBluetoothAndSerialInputs() {
  if (SerialBT.available() > 0) {
    String btCmd = SerialBT.readStringUntil('\n');
    processCommand(btCmd);
  }

  // Tambem aceita os mesmos comandos via cabo USB (Monitor Serial)
  if (Serial.available() > 0) {
    String usbCmd = Serial.readStringUntil('\n');
    processCommand(usbCmd);
  }
}

//Setup

//Configuração inicial do projeto: inicia a comunicação serial, configura o relé, o sensor e o LED e inicializa o Bluetooth com o nome definido.
void setup() {
  Serial.begin(115200);
  delay(500);

  // Pre-carrega o nivel de desligamento antes de habilitar a saida para minimizar pulsos
  digitalWrite(RELAY_PIN, RELAY_OFF_LEVEL);
  pinMode(RELAY_PIN, OUTPUT);
  setPump(false, "Inicializacao do sistema", true);

  analogReadResolution(12);
  analogSetPinAttenuation(SENSOR_PIN, ADC_11db);

  if (!setupHumidityLed()) {
    Serial.println("ERRO: nao foi possivel configurar o LED PWM; bomba mantida desligada.");
    setPump(false, "Erro: falha no LED PWM", true);
    while (true) {
      delay(1000);
    }
  }

  readSensor();

  bool btOK = SerialBT.begin(BT_DEVICE_NAME);

  Serial.println();
  Serial.println("=================================");
  Serial.print("Bluetooth: ");
  Serial.println(btOK ? "OK" : "ERRO");
  Serial.print("Dispositivo: ");
  Serial.println(BT_DEVICE_NAME);
  Serial.print("Pino Rele (Base NPN): GPIO");
  Serial.print(RELAY_PIN);
  Serial.print(" | ON: ");
  Serial.print(levelToStr(RELAY_ON_LEVEL));
  Serial.print(" | OFF: ");
  Serial.println(levelToStr(RELAY_OFF_LEVEL));
  Serial.println("Envie 'ajuda' pelo Bluetooth ou Serial para ver os comandos.");
  Serial.println("=================================");
}

//Loop

//Programa principal: recebe comandos, verifica os limites de segurança e atualiza as leituras a cada segundo. Envia informações ao dispositivo Bluetooth conectado a cada 5 segundos.
void loop() {
  // 1. Le comandos recebidos via Bluetooth ou Serial USB
  handleBluetoothAndSerialInputs();

  // 2. Avalia temporizadores e seguranca continuamente (sem esperar 1s)
  handleTimersAndSafety();

  unsigned long now = millis();

  // 3. Leitura do sensor e logica automatica a cada 1 segundo
  if ((now - lastSensorRead) >= 1000UL) {
    lastSensorRead = now;

    readSensor();
    handleAutomaticIrrigation();
    humanMessage = getHumanMessage();

    Serial.print("BT: ");
    Serial.print(SerialBT.hasClient() ? "CONECTADO" : "AGUARDANDO");
    Serial.print(" | RAW: ");
    Serial.print(rawValue);
    Serial.print(" | Umidade: ");
    Serial.print(humidity);
    Serial.print("% | PWM LED: ");
    Serial.print(ledDuty);
    Serial.print("/255 | Estado: ");
    Serial.print(getSoilStateText());
    Serial.print(" | Modo: ");
    Serial.print(autoMode ? "AUTO" : "MANUAL");
    Serial.print(" | Bomba (cmd): ");
    Serial.print(pumpOn ? "ON" : "OFF");
    Serial.print(" | Agua est.: ");
    Serial.print(totalWaterMl, 1);
    Serial.print(" ml | Motivo: ");
    Serial.println(lastPumpReason);
  }

  // 4. Envia um resumo resumido para o celular conectado a cada 5 segundos
  if (SerialBT.hasClient() && (now - lastBtTelemetry) >= 5000UL) {
    lastBtTelemetry = now;

    SerialBT.print("[");
    SerialBT.print(autoMode ? "AUTO" : "MANUAL");
    SerialBT.print("] Umidade: ");
    SerialBT.print(humidity);
    SerialBT.print("% (");
    SerialBT.print(getSoilStateText());
    SerialBT.print(") | Bomba (cmd): ");
    SerialBT.print(pumpOn ? "ON" : "OFF");
    SerialBT.print(" | LED: ");
    SerialBT.print(ledDuty);
    SerialBT.println("/255");
  }
}
