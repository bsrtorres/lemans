/*
  IES SANJE · Vuelta rápida en La Sarthe
  Puesto de conducción con ESP32 por MQTT

  ENVÍA al juego (tema <PREFIX>/control, 30 veces por segundo):
    {"steer":-1..1, "gas":0..1, "brake":0..1, "hand":0|1}
  y "start" en <PREFIX>/cmd al pulsar el botón de salida.

  RECIBE del juego:
    <PREFIX>/telemetry  -> velocidad, revoluciones, pianos...
    <PREFIX>/event      -> semáforo, salida, choques, vueltas

  Hardware:
    GPIO34  potenciómetro del volante   (pin central; extremos a 3V3 y GND)
    GPIO35  potenciómetro del acelerador
    GPIO32  potenciómetro del freno
    GPIO25  pulsador de salida           (a GND)
    GPIO26  interruptor freno de mano    (a GND)
    GPIO5   tira WS2812 de 10 LED (revoluciones + semáforo)
    GPIO27  ventilador por MOSFET (viento según la velocidad)
    GPIO14  motor de vibración por transistor (choques y pianos)

  Librerías (Gestor de librerías de Arduino):
    PubSubClient, ArduinoJson (v7), Adafruit NeoPixel
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>

// ---------- Configuración ----------
const char* WIFI_SSID = "TU_WIFI";
const char* WIFI_PASS = "TU_CLAVE";
// Broker MQTT (público de HiveMQ, o la IP de tu Mosquitto)
const char* MQTT_HOST = "broker.hivemq.com";   // broker público y gratuito
const int   MQTT_PORT = 1883;
// Credenciales del cliente creado en FlowFuse > Broker. Déjalas vacías con Mosquitto sin contraseña.
// En FlowFuse el usuario es también el ID de cliente: "volante@IDdelEquipo"
const char* MQTT_USER = "";   // vacío con HiveMQ
const char* MQTT_PASS = "";
// Prefijo + código de sala: copia las 4 letras que muestra el juego en el menú
// (por ejemplo, si el juego enseña la sala K7P2, pon "iessanje/lemans/K7P2")
const char* PREFIX    = "iessanje/lemans/K7P2";

#define PIN_STEER 34
#define PIN_GAS   35
#define PIN_BRAKE 32
#define PIN_START 25
#define PIN_HAND  26
#define PIN_LEDS   5
#define PIN_FAN   27
#define PIN_VIB   14
#define NUM_LEDS  10

// Calibración: mira los valores en el monitor serie y ajústalos
int STEER_MIN = 300,  STEER_MAX = 3800;
int GAS_MIN   = 400,  GAS_MAX   = 3600;
int BRAKE_MIN = 400,  BRAKE_MAX = 3600;
const bool CALIBRAR = false;              // true = imprime lecturas crudas

// ---------- Estado ----------
Adafruit_NeoPixel strip(NUM_LEDS, PIN_LEDS, NEO_GRB + NEO_KHZ800);
WiFiClient net;
PubSubClient mqtt(net);

String gameState = "menu";
float rpm = 0;
int kmh = 0;
bool onKerb = false;
int lightsOn = 0;
unsigned long goUntil = 0, vibUntil = 0, lastSend = 0;
bool lastStart = false;

float norm(int raw, int mn, int mx) {
  return constrain((raw - mn) / (float)(mx - mn), 0.0f, 1.0f);
}

String topic(const char* sub) { return String(PREFIX) + "/" + sub; }

void onMessage(char* t, byte* payload, unsigned int len) {
  JsonDocument d;
  if (deserializeJson(d, payload, len)) return;
  String tp(t);
  if (tp.endsWith("/telemetry")) {
    kmh = d["kmh"] | 0;
    rpm = d["rpm"] | 0.0f;
    onKerb = d["kerb"] | false;
    gameState = (const char*)(d["state"] | "menu");
  } else if (tp.endsWith("/event")) {
    const char* type = d["type"] | "";
    if (!strcmp(type, "lights"))      lightsOn = d["n"] | 0;
    else if (!strcmp(type, "go"))     { lightsOn = 0; goUntil = millis() + 1200; }
    else if (!strcmp(type, "turn"))   lightsOn = 0;
    else if (!strcmp(type, "impact")) vibUntil = millis() + 150 + (int)((d["force"] | 0.0f) * 25);
  }
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.println(" OK " + WiFi.localIP().toString());
}

void connectMqtt() {
  while (!mqtt.connected()) {
    bool ok;
    if (strlen(MQTT_USER) > 0) ok = mqtt.connect(MQTT_USER, MQTT_USER, MQTT_PASS);   // ID = usuario
    else ok = mqtt.connect(("volante-" + String((uint32_t)ESP.getEfuseMac(), HEX)).c_str());
    if (ok) {
      mqtt.subscribe(topic("telemetry").c_str());
      mqtt.subscribe(topic("event").c_str());
      Serial.println("MQTT conectado");
    } else {
      Serial.println("MQTT fallo, reintento...");
      delay(1500);
    }
  }
}

void drawLeds() {
  unsigned long now = millis();
  strip.clear();
  if (gameState == "countdown") {
    for (int i = 0; i < lightsOn * 2 && i < NUM_LEDS; i++) strip.setPixelColor(i, strip.Color(255, 0, 0));
  } else if (now < goUntil) {
    for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, strip.Color(0, 255, 0));
  } else if (gameState == "race" || gameState == "done") {
    int n = round(constrain((rpm - 0.5f) / 0.5f, 0.0f, 1.0f) * NUM_LEDS);
    for (int i = 0; i < n; i++) {
      uint32_t c = i < 4 ? strip.Color(0, 200, 40) : (i < 8 ? strip.Color(255, 20, 0) : strip.Color(0, 60, 255));
      strip.setPixelColor(i, c);
    }
    if (rpm > 0.97 && (now / 80) % 2) strip.fill(strip.Color(0, 60, 255));   // cambio de marcha
  } else {
    strip.setPixelColor((now / 150) % NUM_LEDS, strip.Color(29, 63, 138)); // azul SANJE en espera
  }
  strip.show();
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_START, INPUT_PULLUP);
  pinMode(PIN_HAND, INPUT_PULLUP);
  pinMode(PIN_VIB, OUTPUT);
  pinMode(PIN_FAN, OUTPUT);
  analogReadResolution(12);
  strip.begin(); strip.setBrightness(80); strip.show();
  connectWifi();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);
  mqtt.setCallback(onMessage);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWifi();
  if (!mqtt.connected()) connectMqtt();
  mqtt.loop();

  unsigned long now = millis();
  if (now - lastSend >= 33) {
    lastSend = now;
    int rs = analogRead(PIN_STEER), rg = analogRead(PIN_GAS), rb = analogRead(PIN_BRAKE);
    if (CALIBRAR) Serial.printf("volante %d  gas %d  freno %d\n", rs, rg, rb);
    float steer = norm(rs, STEER_MIN, STEER_MAX) * 2.0f - 1.0f;
    if (fabs(steer) < 0.03f) steer = 0;
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"steer\":%.3f,\"gas\":%.3f,\"brake\":%.3f,\"hand\":%d}",
             steer, norm(rg, GAS_MIN, GAS_MAX), norm(rb, BRAKE_MIN, BRAKE_MAX), digitalRead(PIN_HAND) == LOW);
    mqtt.publish(topic("control").c_str(), buf);
  }

  bool st = digitalRead(PIN_START) == LOW;
  if (st && !lastStart) mqtt.publish(topic("cmd").c_str(), "start");
  lastStart = st;

  bool racing = gameState == "race";
  analogWrite(PIN_FAN, racing ? map(constrain(kmh, 0, 340), 0, 340, 60, 255) : 0);
  digitalWrite(PIN_VIB, (now < vibUntil || (racing && onKerb)) ? HIGH : LOW);

  static unsigned long lastLed = 0;
  if (now - lastLed > 30) { lastLed = now; drawLeds(); }
}
