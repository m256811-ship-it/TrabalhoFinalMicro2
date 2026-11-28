//Configurações do projeto
//Inclui as bibliotecas responsáveis pelo funcionamento do ESP32 e pela comunicação Wi-Fi
#include <WiFi.h>
#include <WebServer.h>   

//Configuração do nível lógico utilizado para ligar e desligar a bomba pelo transistor NPN
// Transistor NPN conectado ao pino RELAY_PIN via resistor de base (1 kOhm):
// - RELAY_PIN = HIGH (3.3V) -> transistor conduz -> puxa IN do rele para 0V -> LIGA bomba.
// - RELAY_PIN = LOW  (0V)   -> transistor corta   -> IN do rele sobe para 5V -> DESLIGA bomba.
constexpr uint8_t RELAY_ON_LEVEL  = HIGH;
constexpr uint8_t RELAY_OFF_LEVEL = (RELAY_ON_LEVEL == HIGH) ? LOW : HIGH;

//Configuração dos pinos do sensor, do relé e do LED

#define SENSOR_PIN 35
#define RELAY_PIN 26  

// GPIO5 -> resistor de 240 ohms -> anodo do LED.
// Catodo do LED -> GND.
// Configurações do PMW do led 
#define LED_PWM_PIN 5
#define LED_PWM_CHANNEL 0
#define LED_PWM_FREQ_HZ 5000
#define LED_PWM_RESOLUTION_BITS 8
#define LED_PWM_MAX_DUTY ((1U << LED_PWM_RESOLUTION_BITS) - 1U)

// ==================================================
// Calibracao do sensor de umidade
// ==================================================

#define VAL_SECO 4000
#define VAL_MOLHADO 1000

//Configuração dos limites de umidade e dos tempos de segurança da irrigação automática

#define AUTO_START_HUMIDITY 35
#define AUTO_STOP_HUMIDITY 60
#define MAX_PUMP_TIME 4000UL   // Tempo maximo continuo ligado (4s)
#define AUTO_COOLDOWN 4000UL   // Intervalo minimo entre ciclos automaticos (4s)

// Vazao estimada da bomba
// 0.25 ml/s 
constexpr float PUMP_FLOW_ML_PER_SECOND = 0.25f;

//Configuração do nome e da senha da rede Wi-Fi criada pelo ESP32

#define AP_SSID "Irrigação feliz"
#define AP_PASS "paciencia123"

//Declaração dos objetos e das variáveis utilizadas para armazenar as leituras e os estados do sistema

WebServer server(80);

int rawValue = 0;
int humidity = 0;

unsigned int ledDuty = 0;

bool pumpOn = false;
bool autoMode = false;

bool manualTimedWatering = false;
unsigned long manualStartTime = 0;
unsigned long manualDurationMs = 0;

String lastPumpReason = "Sistema pronto";
String humanMessage = "Auto irrigação";

unsigned long lastSensorRead = 0;
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

//Envia o nível lógico para o pino do relé e exibe o estado comandado da bomba.
void applyRelayLevel(uint8_t level) {
  digitalWrite(RELAY_PIN, level);
  Serial.print("[GPIO");
  Serial.print(RELAY_PIN);
  Serial.print("] Nivel comandado: ");
  Serial.print(levelToStr(level));
  Serial.println(level == RELAY_ON_LEVEL
                   ? " (Transistor ON -> IN=0V / Rele LIGADO)"
                   : " (Transistor OFF -> IN=5V / Rele DESLIGADO)");
}

//Sensor e estados

//Converte a leitura analógica do sensor em um valor de umidade de 0 a 100%, utilizando os valores definidos na calibração.
int rawToHumidity(int raw) {
  int h = map(raw, VAL_SECO, VAL_MOLHADO, 0, 100);
  return constrain(h, 0, 100);
}

//Classifica a umidade do solo em faixas e retorna o texto que será mostrado na página web.
String getSoilStateWeb() {
  if (humidity < 25) return "Seco";
  if (humidity < 40) return "Baixa";
  if (humidity <= 60) return "Ideal";
  if (humidity <= 75) return "Muito úmido";
  return "Encharcado";
}

//Identifica a situação do sistema e retorna uma mensagem de acordo com a umidade, o modo selecionado e o estado comandado da bomba.
String getHumanMessage() {
  if (autoMode) {
    if (pumpOn) return "Regando (comandado)";
    return "Irrigação automática";
  }

  if (pumpOn) return "Irrigação manual (comandada)";

  if (humidity < 25) return "Precisa de água";
  if (humidity < 40) return "Umidade baixa";
  if (humidity <= 60) return "Umidade ideal";
  if (humidity <= 75) return "Muito úmido";
  return "Solo encharcado";
}

// ==================================================
// LED PWM (Alerta visual: mais seco = mais brilho)
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
    Serial.print("[BOMBA] Comando redundante ignorado. Estado ja estava em: ");
    Serial.println(pumpOn ? "LIGADA (comandado)" : "DESLIGADA (comandado)");
    return;
  }

  unsigned long now = millis();

  if (state) {
    pumpOn = true;
    pumpStartTime = now;

    Serial.print("[BOMBA] Transicao -> LIGADA (comandado) | Motivo: ");
    Serial.println(reason.length() > 0 ? reason : "Nao especificado");
    applyRelayLevel(RELAY_ON_LEVEL);
  } else {
    unsigned long durationMs = pumpOn ? (now - pumpStartTime) : 0;
    bool wasOn = pumpOn;

    pumpOn = false;
    manualTimedWatering = false;
    manualDurationMs = 0;

    Serial.print("[BOMBA] Transicao -> DESLIGADA (comandado) | Motivo: ");
    Serial.println(reason.length() > 0 ? reason : "Nao especificado");
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
    Serial.println("[MANUAL] Comando ignorado: sistema esta em modo AUTOMATICO.");
    return;
  }

  if (pumpOn) {
    Serial.println("[MANUAL] Comando ignorado: bomba ja esta com comando ativo.");
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
      setPump(false, "Manual: tempo concluído");
      return;
    }
  }

  // 2. Protecao global de tempo maximo continuo (vale para manual e automatico)
  if (pumpOn && (now - pumpStartTime) >= MAX_PUMP_TIME) {
    setPump(false, "Segurança: corte tempo máximo (" + String(MAX_PUMP_TIME / 1000UL) + "s)");
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

//Painel web

//Monta a página HTML exibida no navegador, incluindo as leituras do sensor, o estado do sistema e os botões de controle da irrigação.
String buildPage() {
  String modeText = autoMode ? "Automático" : "Manual";
  String pumpText = pumpOn ? "Ligada (cmd)" : "Desligada (cmd)";
  String soilText = getSoilStateWeb();

  String sceneClass = pumpOn ? "scene watering" : "scene";
  String waterVisualText =
    pumpOn ? "Bomba comandada: LIGADA" : "Sistema em repouso";

  float estimatedLitersPerHour = (PUMP_FLOW_ML_PER_SECOND * 3600.0f) / 1000.0f;

  String html = "";

  html += "<!DOCTYPE html><html lang='pt-BR'><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='3'>";
  html += "<title>Rega do Bonsai</title>";

  html += "<style>";
  html += ":root{--bg1:#eaf4ef;--bg2:#dcecff;--glass:rgba(255,255,255,.58);--line:rgba(255,255,255,.65);--text:#111827;--muted:#667085;--green:#34c759;--blue:#007aff;--red:#ff3b30;}";
  html += "*{box-sizing:border-box}";
  html += "body{margin:0;min-height:100vh;font-family:-apple-system,BlinkMacSystemFont,'SF Pro Display','Segoe UI',Arial,sans-serif;color:var(--text);background:radial-gradient(circle at 20% 10%,#fff 0,#e9f8f0 26%,transparent 45%),radial-gradient(circle at 85% 15%,#e7f0ff 0,#dceaff 32%,transparent 50%),linear-gradient(135deg,var(--bg1),var(--bg2));padding:22px;}";
  html += ".wrap{max-width:520px;margin:0 auto}";
  html += ".hero{padding:24px 10px 10px}";
  html += "h1{font-size:36px;line-height:1.05;margin:0;font-weight:800;letter-spacing:-1px}";
  html += ".sub{color:var(--muted);margin-top:8px;font-size:16px}";
  html += ".grid{display:grid;gap:14px;margin-top:18px}";
  html += ".card{position:relative;overflow:hidden;background:var(--glass);border:1px solid var(--line);border-radius:28px;padding:20px;box-shadow:0 20px 60px rgba(31,41,55,.14);backdrop-filter:blur(22px);-webkit-backdrop-filter:blur(22px);}";
  html += ".card:before{content:'';position:absolute;inset:0;background:linear-gradient(145deg,rgba(255,255,255,.75),rgba(255,255,255,.16));pointer-events:none}";
  html += ".content{position:relative;z-index:1}";
  html += ".row{display:flex;align-items:center;justify-content:space-between;gap:12px}";
  html += ".label{font-size:13px;color:var(--muted);font-weight:600;text-transform:uppercase;letter-spacing:.08em}";
  html += ".big{font-size:68px;font-weight:800;letter-spacing:-4px;line-height:.95}";
  html += ".percent{font-size:32px;letter-spacing:-1px}";
  html += ".pill{display:inline-flex;align-items:center;gap:8px;padding:8px 12px;border-radius:999px;background:rgba(255,255,255,.55);border:1px solid rgba(255,255,255,.65);font-weight:700;font-size:14px}";
  html += ".dot{width:9px;height:9px;border-radius:50%;display:inline-block;background:var(--green)}";
  html += ".dot.blue{background:var(--blue)}";
  html += ".msg{font-size:20px;font-weight:700;letter-spacing:-.3px;margin-top:14px}";
  html += ".small{color:var(--muted);font-size:14px;margin-top:6px}";
  html += ".actions{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:14px}";
  html += "button{width:100%;appearance:none;border:0;border-radius:18px;padding:15px 12px;font-size:16px;font-weight:800;color:white;box-shadow:0 10px 26px rgba(0,0,0,.12);cursor:pointer}";
  html += ".btn-blue{background:linear-gradient(180deg,#4aa3ff,#007aff)}";
  html += ".btn-red{background:linear-gradient(180deg,#ff716b,#ff3b30)}";
  html += ".btn-green{background:linear-gradient(180deg,#5ee27f,#34c759)}";
  html += ".btn-gray{background:linear-gradient(180deg,#8e8e93,#636366)}";
  html += ".btn-disabled{opacity:.45;cursor:not-allowed}";
  html += ".metric{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:10px}";
  html += ".mini{background:rgba(255,255,255,.42);border:1px solid rgba(255,255,255,.55);border-radius:20px;padding:14px}";
  html += ".mini b{display:block;font-size:20px;margin-top:4px;letter-spacing:-.4px}";
  html += ".range{display:grid;gap:8px;margin-top:14px}";
  html += ".range-row{display:flex;align-items:center;justify-content:space-between;background:rgba(255,255,255,.38);border:1px solid rgba(255,255,255,.52);border-radius:16px;padding:10px 12px;font-size:14px}";
  html += ".tag{font-weight:800}";
  html += ".dry{color:#b42318}.low{color:#b54708}.ideal{color:#027a48}.wet{color:#175cd3}.drown{color:#5925dc}";

  // Barra de umidade.
  html += ".bar{height:18px;background:rgba(120,160,190,.18);border-radius:999px;overflow:hidden;margin-top:16px;position:relative;border:1px solid rgba(255,255,255,.72);box-shadow:inset 0 2px 8px rgba(30,80,120,.12)}";
  html += ".fill{height:100%;border-radius:999px;background:linear-gradient(180deg,#9be7ff 0%,#4aa3ff 45%,#007aff 100%);width:" + String(humidity) + "%;position:relative;overflow:hidden;box-shadow:inset 0 2px 6px rgba(255,255,255,.55),inset 0 -4px 8px rgba(0,80,170,.22)}";
  html += ".fill:before{content:'';position:absolute;left:-25%;top:-8px;width:150%;height:18px;background:radial-gradient(ellipse at center,rgba(255,255,255,.65) 0 35%,transparent 36%);background-size:24px 9px;animation:waterline 2.8s linear infinite;opacity:.45}";
  html += ".fill:after{content:'';position:absolute;inset:0;background:linear-gradient(90deg,transparent,rgba(255,255,255,.30),transparent);animation:shine 3s ease-in-out infinite}";
  html += "@keyframes waterline{from{transform:translateX(0)}to{transform:translateX(24px)}}";
  html += "@keyframes shine{0%{transform:translateX(-120%)}70%,100%{transform:translateX(120%)}}";

  // Ilustracao do bonsai.
  html += ".scene{height:170px;border-radius:30px;margin-top:16px;position:relative;overflow:hidden;background:linear-gradient(180deg,rgba(232,245,255,.86),rgba(235,255,240,.62));border:1px solid rgba(255,255,255,.7);box-shadow:inset 0 0 24px rgba(255,255,255,.45)}";
  html += ".sky{position:absolute;inset:0;background:radial-gradient(circle at 20% 20%,rgba(255,255,255,.9),transparent 22%),linear-gradient(180deg,rgba(218,238,255,.7),rgba(234,250,235,.4))}";
  html += ".scene-text{position:absolute;left:14px;top:12px;padding:8px 12px;border-radius:999px;background:rgba(255,255,255,.62);border:1px solid rgba(255,255,255,.7);font-size:13px;font-weight:800;color:#175cd3;z-index:10}";
  html += ".bonsai-wrap{position:absolute;left:50%;bottom:8px;transform:translateX(-50%);width:180px;height:140px;z-index:3}";
  html += ".pot{position:absolute;left:50%;bottom:0;width:112px;height:50px;margin-left:-56px;background:linear-gradient(180deg,#c88755,#9b5f35);border-radius:12px 12px 28px 28px;box-shadow:inset 0 8px 12px rgba(255,255,255,.2);z-index:1}";
  html += ".trunk{position:absolute;left:50%;bottom:50px;width:10px;height:50px;margin-left:-5px;background:#6b4428;border-radius:10px;transform:rotate(-6deg);z-index:2}";
  html += ".leaf{position:absolute;background:linear-gradient(180deg,#49d46d,#1f8f44);border-radius:50%;box-shadow:0 8px 18px rgba(31,143,68,.18);z-index:3}";
  html += ".leaf.l1{width:54px;height:34px;left:34px;bottom:88px}";
  html += ".leaf.l2{width:68px;height:40px;left:56px;bottom:98px}";
  html += ".leaf.l3{width:58px;height:34px;left:92px;bottom:88px}";
  html += ".leaf.l4{width:46px;height:28px;left:66px;bottom:78px}";

  // Animacao de irrigacao.
  html += ".rain{position:absolute;left:50%;top:18px;transform:translateX(-50%);width:74px;height:92px;opacity:0;z-index:8;pointer-events:none}";
  html += ".rain span{position:absolute;top:-34px;width:5px;height:34px;border-radius:999px;background:linear-gradient(180deg,rgba(185,240,255,.98),rgba(74,163,255,.7),rgba(74,163,255,.05));animation:dropfall .72s linear infinite}";
  html += ".rain span:nth-child(1){left:6px;animation-delay:0s}";
  html += ".rain span:nth-child(2){left:20px;animation-delay:.12s}";
  html += ".rain span:nth-child(3){left:34px;animation-delay:.24s}";
  html += ".rain span:nth-child(4){left:48px;animation-delay:.36s}";
  html += ".rain span:nth-child(5){left:62px;animation-delay:.18s}";
  html += ".splash{position:absolute;left:50%;top:82px;transform:translateX(-50%);width:86px;height:22px;opacity:0;z-index:9}";
  html += ".splash span{position:absolute;bottom:0;width:7px;height:7px;background:#64d2ff;border-radius:50%;animation:splash .7s ease-out infinite}";
  html += ".splash span:nth-child(1){left:6px;animation-delay:0s}";
  html += ".splash span:nth-child(2){left:24px;animation-delay:.18s}";
  html += ".splash span:nth-child(3){left:42px;animation-delay:.32s}";
  html += ".splash span:nth-child(4){left:60px;animation-delay:.46s}";
  html += ".splash span:nth-child(5){left:76px;animation-delay:.24s}";
  html += ".watering .rain,.watering .splash{opacity:1}";
  html += "@keyframes dropfall{from{transform:translateY(0);opacity:0}18%{opacity:1}to{transform:translateY(102px);opacity:0}}";
  html += "@keyframes splash{0%{transform:translateY(0) scale(.5);opacity:0}40%{opacity:1}100%{transform:translateY(-18px) scale(1.1);opacity:0}}";
  html += "</style>";

  html += "</head><body><main class='wrap'>";

  html += "<section class='hero'>";
  html += "<h1>Rega do Bonsai</h1>";
  html += "<div class='sub'>Controle de umidade e irrigação do bonsai.</div>";
  html += "</section>";

  html += "<section class='grid'>";

  // Umidade e LED PWM.
  html += "<div class='card'><div class='content'>";
  html += "<div class='row'>";
  html += "<div class='label'>Umidade do solo</div>";
  html += "<div class='pill'><span class='dot'></span>" + soilText + "</div>";
  html += "</div>";
  html += "<div class='big'>" + String(humidity) + "<span class='percent'>%</span></div>";
  html += "<div class='bar'><div class='fill'></div></div>";
  html += "<div class='small'>LED PWM (GPIO" + String(LED_PWM_PIN) + "): " + String(ledDuty) + "/255 (mais seco = mais brilho; apaga em &ge; 80%)</div>";

  html += "<div class='" + sceneClass + "'>";
  html += "<div class='sky'></div>";
  html += "<div class='scene-text'>" + waterVisualText + "</div>";
  html += "<div class='rain'><span></span><span></span><span></span><span></span><span></span></div>";
  html += "<div class='splash'><span></span><span></span><span></span><span></span><span></span></div>";
  html += "<div class='bonsai-wrap'>";
  html += "<div class='trunk'></div>";
  html += "<div class='leaf l1'></div>";
  html += "<div class='leaf l2'></div>";
  html += "<div class='leaf l3'></div>";
  html += "<div class='leaf l4'></div>";
  html += "<div class='pot'></div>";
  html += "</div></div>";

  html += "<div class='msg'>" + humanMessage + "</div>";
  html += "<div class='small'>Último evento: " + lastPumpReason + "</div>";
  html += "</div></div>";

  // Modo de irrigacao e bomba.
  html += "<div class='card'><div class='content'>";
  html += "<div class='row'>";
  html += "<div>";
  html += "<div class='label'>Modo de irrigação</div>";
  html += "<div class='msg'>" + modeText + "</div>";
  html += "</div>";
  html += "<div class='pill'><span class='dot blue'></span>" + pumpText + "</div>";
  html += "</div>";

  html += "<div class='metric'>";
  html += "<div class='mini'><span class='label'>Rega se baixar de</span><b>&lt; " + String(AUTO_START_HUMIDITY) + "%</b></div>";
  html += "<div class='mini'><span class='label'>Objetivo</span><b>40-" + String(AUTO_STOP_HUMIDITY) + "%</b></div>";
  html += "</div>";

  html += "<div class='actions'>";
  html += "<form action='/mode' method='POST'><button class='btn-blue'>Alterar modo</button></form>";

  if (autoMode) {
    html += "<form action='/' method='GET'><button class='btn-gray btn-disabled' disabled>Bomba auto</button></form>";
  } else {
    if (pumpOn) {
      html += "<form action='/stop' method='POST'><button class='btn-red'>Parar rega</button></form>";
    } else {
      html += "<form action='/water2' method='POST'><button class='btn-green'>Regar 2s</button></form>";
      html += "<form action='/water4' method='POST'><button class='btn-green'>Regar 4s</button></form>";
    }
  }

  html += "</div>";

  html += "<div class='small'>";
  if (autoMode) {
    html += "Modo automático ativo. O estado exibido indica o comando enviado ao relé (sem sensor de vazão).";
  } else {
    html += "Modo manual ativo. Você pode comandar a rega por 2 ou 4 segundos (sem sensor de vazão).";
  }
  html += "</div>";

  html += "</div></div>";

  // Registro de irrigacao.
  html += "<div class='card'><div class='content'>";
  html += "<div class='label'>Registro estimado de rega</div>";
  html += "<div class='metric'>";
  html += "<div class='mini'><span class='label'>Total estimado</span><b>" + String(totalWaterMl / 1000.0f, 2) + " L</b></div>";
  html += "<div class='mini'><span class='label'>Última rega</span><b>" + String(lastWaterMl, 0) + " ml</b></div>";
  html += "</div>";
  html += "<div class='small'>Ciclos comandados: " + String(irrigationCount) + " · Estimativa sem calibração: " + String(PUMP_FLOW_ML_PER_SECOND, 1) + " ml/s (~" + String(estimatedLitersPerHour, 1) + " L/h)</div>";
  html += "</div></div>";

  // Escala de umidade.
  html += "<div class='card'><div class='content'>";
  html += "<div class='label'>Escala de umidade</div>";
  html += "<div class='range'>";
  html += "<div class='range-row'><span class='tag dry'>Seco</span><b>0-24%</b></div>";
  html += "<div class='range-row'><span class='tag low'>Baixa</span><b>25-39%</b></div>";
  html += "<div class='range-row'><span class='tag ideal'>Ideal</span><b>40-60%</b></div>";
  html += "<div class='range-row'><span class='tag wet'>Muito úmido</span><b>61-75%</b></div>";
  html += "<div class='range-row'><span class='tag drown'>Encharcado</span><b>76-100%</b></div>";
  html += "</div>";
  html += "</div></div>";

  html += "</section></main></body></html>";

  return html;
}

//Configuração das funções responsáveis pelas requisições recebidas do navegador

//Envia a página principal para o navegador quando o endereço do servidor é acessado.
void handleRoot() {
  server.send(200, "text/html", buildPage());
}

//Recebe o comando enviado pelo botão de irrigação por 2 segundos. Inicia a bomba apenas no modo manual e retorna à página principal.
void handleWater2() {
  Serial.println("[HTTP] Comando recebido: POST /water2");
  if (!autoMode) {
    startManualWatering(2);
  } else {
    Serial.println("[HTTP] /water2 ignorado: sistema em modo automatico.");
  }

  server.sendHeader("Location", "/");
  server.send(303);
}

//Recebe o comando enviado pelo botão de irrigação por 4 segundos. Inicia a bomba apenas no modo manual e retorna à página principal.
void handleWater4() {
  Serial.println("[HTTP] Comando recebido: POST /water4");
  if (!autoMode) {
    startManualWatering(4);
  } else {
    Serial.println("[HTTP] /water4 ignorado: sistema em modo automatico.");
  }

  server.sendHeader("Location", "/");
  server.send(303);
}

//Recebe o comando para interromper a irrigação manual e retorna à página principal.
void handleStop() {
  Serial.println("[HTTP] Comando recebido: POST /stop");
  if (!autoMode && pumpOn) {
    setPump(false, "Manual: rega interrompida pelo usuário");
  } else {
    Serial.println("[HTTP] /stop ignorado: bomba ja desligada ou em modo automatico.");
  }

  server.sendHeader("Location", "/");
  server.send(303);
}

//Alterna entre os modos manual e automático. Desliga a bomba durante a mudança e atualiza as informações do sistema.
void handleMode() {
  Serial.println("[HTTP] Comando recebido: POST /mode");
  autoMode = !autoMode;

  if (autoMode) {
    hasAutoRunOnce = false; // Permite avaliacao imediata ao entrar no modo automatico
    setPump(false, "Modo automático ativado", true);
  } else {
    setPump(false, "Modo manual ativado", true);
  }

  humanMessage = getHumanMessage();

  server.sendHeader("Location", "/");
  server.send(303);
}

//Exibe a página principal quando o navegador solicita um endereço não cadastrado.
void handleNotFound() {
  server.send(200, "text/html", buildPage());
}

//Setup

//Configuração inicial do projeto: inicia a comunicação serial, configura o relé, o sensor e o LED, cria o ponto de acesso Wi-Fi e inicializa o servidor web.
void setup() {
  Serial.begin(115200);
  delay(500);

  // Pre-carrega o nivel de desligamento antes de habilitar a saida para minimizar pulsos
  digitalWrite(RELAY_PIN, RELAY_OFF_LEVEL);
  pinMode(RELAY_PIN, OUTPUT);
  setPump(false, "Inicialização do sistema", true);

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

  WiFi.disconnect(true);
  delay(200);

  WiFi.mode(WIFI_AP);
  bool wifiOK = WiFi.softAP(AP_SSID, AP_PASS);
  delay(500);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/water2", HTTP_POST, handleWater2);
  server.on("/water4", HTTP_POST, handleWater4);
  server.on("/stop", HTTP_POST, handleStop);
  server.on("/mode", HTTP_POST, handleMode);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println();
  Serial.println("=================================");
  Serial.print("WiFi AP: ");
  Serial.println(wifiOK ? "OK" : "ERRO");
  Serial.print("Rede: ");
  Serial.println(AP_SSID);
  Serial.print("Senha: ");
  Serial.println(AP_PASS);
  Serial.print("IP: http://");
  Serial.println(WiFi.softAPIP());
  Serial.print("Pino Rele (Base NPN): GPIO");
  Serial.print(RELAY_PIN);
  Serial.print(" | ON: ");
  Serial.print(levelToStr(RELAY_ON_LEVEL));
  Serial.print(" | OFF: ");
  Serial.println(levelToStr(RELAY_OFF_LEVEL));
  Serial.println("=================================");
}

//Loop

//Programa principal: atende às requisições do navegador, verifica os limites de segurança e atualiza a leitura do sensor e a irrigação automática a cada segundo.
void loop() {
  server.handleClient();

  // Avalia temporizadores e seguranca continuamente (sem esperar 1s)
  handleTimersAndSafety();

  unsigned long now = millis();
  if ((now - lastSensorRead) >= 1000UL) {
    lastSensorRead = now;

    readSensor();
    handleAutomaticIrrigation();
    humanMessage = getHumanMessage();

    Serial.print("IP: http://");
    Serial.print(WiFi.softAPIP());
    Serial.print(" | RAW: ");
    Serial.print(rawValue);
    Serial.print(" | Umidade: ");
    Serial.print(humidity);
    Serial.print("% | PWM LED: ");
    Serial.print(ledDuty);
    Serial.print("/255 | Estado: ");
    Serial.print(getSoilStateWeb());
    Serial.print(" | Modo: ");
    Serial.print(autoMode ? "AUTO" : "MANUAL");
    Serial.print(" | Bomba (cmd): ");
    Serial.print(pumpOn ? "ON" : "OFF");
    Serial.print(" | Agua est.: ");
    Serial.print(totalWaterMl, 1);
    Serial.print(" ml | Motivo: ");
    Serial.println(lastPumpReason);
  }
}
