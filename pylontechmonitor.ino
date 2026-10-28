#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include "utilities.h"

// ==========================
// KONFIGURATION (Stabilität)
// ==========================
const unsigned long STALE_MS          = 90000;   // Daten älter als 90 s gelten als veraltet (Zyklus bei 4 Akkus ca. 28 s)
const unsigned long WIFI_RECONNECT_MS = 30000;   // nach 30 s ohne WLAN: Reconnect versuchen
const unsigned long WIFI_RESTART_MS   = 120000;  // nach 2 min ohne WLAN: Neustart
const unsigned long MQTT_RESTART_MS   = 300000;  // nach 5 min ohne erfolgreichen MQTT-Publish: Neustart
const unsigned long BAT_CMD_GAP_MS    = 400;     // Pause zwischen "unit X" und "bat X"

// ==========================
// MQTT GLOBALS
// ==========================
WiFiClient espClient;
PubSubClient mqttClient(espClient);
Preferences preferences;

char mqtt_server[40] = "";
char mqtt_user[40] = "";
char mqtt_pass[40] = "";

// WiFiManager-Parameter global, damit der Save-Callback die neuen Werte lesen kann
WiFiManagerParameter custom_mqtt_server("server", "MQTT Broker IP", "", 40);
WiFiManagerParameter custom_mqtt_user("user", "MQTT Username (optional)", "", 40);
WiFiManagerParameter custom_mqtt_pass("pass", "MQTT Passwort (optional)", "", 40, "type='password'");

unsigned long lastMqttRetry = 0;
unsigned long lastMqttPublish = 0;
unsigned long lastMqttSuccess = 0;
const unsigned long MQTT_INTERVAL = 5000;
bool mqttReady = false;

// ==========================
// DATA MODEL
// ==========================

struct Battery {
  bool present = false;

  float soc = 0;
  float voltage = 0;
  float current = 0;

  float temp = 0;      // BMS / Board-Temperatur
  float tempLow = 0;   // Min Zell-Temperatur
  float tempHigh = 0;  // Max Zell-Temperatur

  float cellMin = 0;
  float cellMax = 0;

  // Einzelzellen (0 bis 15) - nur gültige, vollständig empfangene Werte
  float cellVoltages[16] = {0};
  int cellCount = 0;

  // Zwischenspeicher für laufende "bat"-Antwort
  float tmpCells[16] = {0};
  bool tmpGot[16] = {false};
  int tmpCellCount = 0;

  // Zeitstempel der letzten erfolgreichen Aktualisierung
  unsigned long lastPwrUpdate = 0;
  unsigned long lastCellUpdate = 0;

  String raw;
};

Battery batteries[16];

bool pwrFresh(const Battery &b)   { return b.lastPwrUpdate  != 0 && (millis() - b.lastPwrUpdate)  < STALE_MS; }
bool cellsFresh(const Battery &b) { return b.lastCellUpdate != 0 && (millis() - b.lastCellUpdate) < STALE_MS; }

// ==========================
// SYSTEM STATS
// ==========================

struct SystemStats {
  bool valid = false;
  float avgSOC = 0;
  float avgVoltage = 0;
  float minCell = 999;
  float maxCell = 0;
  float totalCurrent = 0;
  float maxSystemTemp = -99.0;     // Maximale Zelltemperatur im Gesamtsystem
  float maxSystemBmsTemp = -99.0;  // Maximale BMS-Temperatur im Gesamtsystem
};

SystemStats systemStats;

// ==========================
// STATE MACHINE & SERIAL
// ==========================

enum PytesParserMode { MODE_NONE, MODE_PWR, MODE_BAT, MODE_STAT };
PytesParserMode currentMode = MODE_NONE;
int targetBatId = 0;

String rxBuffer = "";
unsigned long lastSend = 0;
int nextQueryIndex = 0;

// Nicht-blockierende Sequenz "unit X" -> Pause -> "bat X"
int pendingBatId = 0;
unsigned long pendingBatAt = 0;

// Variablen für die manuelle STAT-Abfrage
String statOutput = "";
bool statQueryInProgress = false;
unsigned long statStartTime = 0;
int statTargetUnit = 1;
int statStep = 0;
const size_t STAT_MAX_LEN = 6000;

// ==========================
// WEB SERVER
// ==========================

WebServer server(80);

// ==========================
// HILFSFUNKTIONEN SERIAL
// ==========================

void flushRx()
{
  while(Serial0.available()) Serial0.read();
  rxBuffer = "";
}

void resetBatTmp(int id)
{
  if(id < 1 || id > 16) return;
  Battery &b = batteries[id - 1];
  for(int i = 0; i < 16; i++) { b.tmpGot[i] = false; b.tmpCells[i] = 0; }
  b.tmpCellCount = 0;
}

// Übernimmt die empfangenen Zellwerte erst, wenn die Antwort (weitgehend) komplett ist.
// Zellen, die nicht empfangen wurden, behalten ihren letzten gültigen Wert.
void commitBat()
{
  if(targetBatId < 1 || targetBatId > 16) return;
  Battery &b = batteries[targetBatId - 1];

  bool gotAny = false;
  for(int i = 0; i < 16; i++)
  {
    if(b.tmpGot[i])
    {
      b.cellVoltages[i] = b.tmpCells[i];
      gotAny = true;
    }
  }

  if(gotAny)
  {
    if(b.tmpCellCount > b.cellCount) b.cellCount = b.tmpCellCount;
    b.lastCellUpdate = millis();
  }
  resetBatTmp(targetBatId);
  currentMode = MODE_NONE;
}

// ==========================
// REQUESTS
// ==========================

void requestPwr()
{
  if(currentMode == MODE_BAT) commitBat();   // angefangene bat-Antwort sichern

  flushRx();
  currentMode = MODE_PWR;
  Serial0.print("\npwr\n");   // erstes \n weckt den Prompt auf, falls die Konsole hängt
}

void requestBat(int id)
{
  if(currentMode == MODE_BAT) commitBat();

  flushRx();
  currentMode = MODE_NONE;       // Antwort auf "unit" nicht als Zelldaten interpretieren
  targetBatId = id;
  resetBatTmp(id);

  Serial0.printf("\nunit %d\n", id);
  pendingBatId = id;
  pendingBatAt = millis() + BAT_CMD_GAP_MS;
}

// Sendet "bat X" erst nach kurzer Pause (ohne zu blockieren)
void servicePendingBat()
{
  if(pendingBatId > 0 && (long)(millis() - pendingBatAt) >= 0)
  {
    flushRx();
    currentMode = MODE_BAT;
    Serial0.printf("bat %d\n", pendingBatId);
    pendingBatId = 0;
  }
}

// Startet die manuelle STAT-Abfrage
void triggerStatQuery(int unitId)
{
  if(currentMode == MODE_BAT) commitBat();

  pendingBatId = 0;
  flushRx();
  currentMode = MODE_STAT;
  statOutput = "";
  statOutput.reserve(STAT_MAX_LEN + 64);
  statQueryInProgress = true;
  statStartTime = millis();
  statTargetUnit = unitId;
  statStep = 0;

  Serial0.print("stat\n");
}

// ==========================
// PARSER FOR "pwr"
// ==========================

void parsePwrLine(String line)
{
  line.trim();
  if(line.length() < 10) return;
  if(!isDigit(line[0])) return;
  if(line.indexOf("Absent") >= 0) return;

  String v[30];
  int idx = 0;
  String token = "";

  for(size_t i = 0; i < line.length(); i++)
  {
    char c = line[i];
    if(c == ' ' || c == '\t')
    {
      if(token.length())
      {
        if(idx < 30) v[idx++] = token;
        token = "";
      }
    }
    else
    {
      token += c;
    }
  }
  if(token.length() && idx < 30) v[idx++] = token;
  if(idx < 8) return;

  int id = v[0].toInt();
  if(id < 1 || id > 16) return;

  Battery &b = batteries[id - 1];
  b.present = true;

  b.voltage = v[1].toFloat() / 1000.0;
  b.current = v[2].toFloat() / 1000.0;

  b.temp = v[3].toFloat() / 1000.0;      // BMS-Board-Temperatur (v[3])
  b.tempLow = v[4].toFloat() / 1000.0;   // Min Zell-Temp (v[4])
  b.tempHigh = v[5].toFloat() / 1000.0;  // Max Zell-Temp (v[5])

  float vmin = v[6].toFloat();
  float vmax = v[7].toFloat();

  if(vmin > 0) b.cellMin = vmin / 1000.0;
  if(vmax > 0) b.cellMax = vmax / 1000.0;

  int p = line.indexOf('%');
  if(p > 0)
  {
    int start = p;
    while(start > 0 && isDigit(line[start - 1])) start--;
    b.soc = line.substring(start, p).toFloat();
  }
  b.raw = line;
  b.lastPwrUpdate = millis();
}

// ==========================
// PARSER FOR "bat X"
// ==========================
void parseBatLine(String line)
{
  line.trim();
  if(line.length() < 5) return;
  if(!isDigit(line[0])) return;

  String v[10];
  int idx = 0;
  String token = "";

  for(size_t i = 0; i < line.length(); i++)
  {
    char c = line[i];
    if(c == ' ' || c == '\t')
    {
      if(token.length()){
        v[idx++] = token;
        token = "";
        if(idx >= 10) break;
      }
    }
    else
    {
      token += c;
    }
  }
  if(token.length() && idx < 10) v[idx++] = token;

  if(idx < 2) return;

  int cellId = v[0].toInt();
  if(cellId < 0 || cellId > 15) return;

  float cellVolt = v[1].toFloat() / 1000.0;

  if(targetBatId >= 1 && targetBatId <= 16)
  {
    Battery &b = batteries[targetBatId - 1];

    // Nur in den Zwischenspeicher schreiben; Übernahme erst in commitBat()
    if(cellVolt > 2.5 && cellVolt < 3.65)
    {
      b.tmpCells[cellId] = cellVolt;
      b.tmpGot[cellId] = true;
    }

    if((cellId + 1) > b.tmpCellCount) {
      b.tmpCellCount = cellId + 1;
    }
  }
}

// ==========================
// SERIAL READ
// ==========================

bool isEndOfResponse(const String &line)
{
  return line.indexOf("Command completed") >= 0 ||
         line.indexOf("pylon>") >= 0 ||
         line.startsWith("$$");
}

void readSerial()
{
  while(Serial0.available())
  {
    char c = Serial0.read();

    if(statQueryInProgress) {
      if(statOutput.length() < STAT_MAX_LEN) statOutput += c;

      if(c == '\n') {
        if(statOutput.endsWith("pylon>") || statOutput.endsWith("$-_")) {
          statQueryInProgress = false;
          currentMode = MODE_NONE;
        }
      }
      continue;
    }

    if(c == '\n')
    {
      if(currentMode == MODE_PWR){
        parsePwrLine(rxBuffer);
        if(isEndOfResponse(rxBuffer)) currentMode = MODE_NONE;
      }
      else if(currentMode == MODE_BAT){
        parseBatLine(rxBuffer);
        if(isEndOfResponse(rxBuffer)) commitBat();   // Antwort komplett -> Werte übernehmen
      }
      rxBuffer = "";
    }
    else
    {
      if(c != '\r')
      {
        if(rxBuffer.length() < 300) rxBuffer += c;   // Schutz vor Müll ohne Zeilenende
        else rxBuffer = "";
      }
    }
  }
}

// ==========================
// MQTT SENDER
// ==========================

bool pubStr(const char *topic, const char *val)
{
  bool ok = mqttClient.publish(topic, val);
  if(ok) lastMqttSuccess = millis();
  return ok;
}

void pubFloat(const char *topic, float value, int decimals)
{
  char buf[16];
  snprintf(buf, sizeof(buf), "%.*f", decimals, value);
  pubStr(topic, buf);
}

void publishSystemStats()
{
  if (!mqttClient.connected()) return;

  if (systemStats.valid)
  {
    pubFloat("pylontech/system/avgSOC", systemStats.avgSOC, 1);
    pubFloat("pylontech/system/avgVoltage", systemStats.avgVoltage, 2);
    pubFloat("pylontech/system/totalCurrent", systemStats.totalCurrent, 2);

    if (systemStats.maxCell > 0) {
      pubFloat("pylontech/system/minCell", systemStats.minCell, 3);
      pubFloat("pylontech/system/maxCell", systemStats.maxCell, 3);
    }
    if (systemStats.maxSystemTemp > -99.0) {
      pubFloat("pylontech/system/maxTemperature", systemStats.maxSystemTemp, 1);
    }
    if (systemStats.maxSystemBmsTemp > -99.0) {
      pubFloat("pylontech/system/maxBmsTemperature", systemStats.maxSystemBmsTemp, 1);
    }
  }

  char topic[48];

  for(int i = 0; i < 16; i++) {
    Battery &b = batteries[i];
    if(!b.present) continue;

    // Veraltete Daten NICHT mehr veröffentlichen -> Home Assistant sieht einen echten Ausfall
    if(pwrFresh(b))
    {
      snprintf(topic, sizeof(topic), "pylontech/battery%d/soc", i + 1);
      pubFloat(topic, b.soc, 0);
      snprintf(topic, sizeof(topic), "pylontech/battery%d/voltage", i + 1);
      pubFloat(topic, b.voltage, 2);
      snprintf(topic, sizeof(topic), "pylontech/battery%d/current", i + 1);
      pubFloat(topic, b.current, 2);
      snprintf(topic, sizeof(topic), "pylontech/battery%d/temp", i + 1);
      pubFloat(topic, b.temp, 1);
      snprintf(topic, sizeof(topic), "pylontech/battery%d/cellTempMax", i + 1);
      pubFloat(topic, b.tempHigh, 1);
    }

    if(cellsFresh(b))
    {
      for(int c = 0; c < b.cellCount; c++) {
        if(b.cellVoltages[c] <= 0) continue;   // nie empfangene Zelle nicht als 0 V melden
        snprintf(topic, sizeof(topic), "pylontech/battery%d/cell%d", i + 1, c + 1);
        pubFloat(topic, b.cellVoltages[c], 3);
      }
    }
  }
}

void handleMqtt()
{
  if (strlen(mqtt_server) == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;

  if (!mqttClient.connected())
  {
    if (millis() - lastMqttRetry > 5000)
    {
      lastMqttRetry = millis();

      bool ok;
      if (strlen(mqtt_user) > 0)
        ok = mqttClient.connect("PylontechMonitor", mqtt_user, mqtt_pass);
      else
        ok = mqttClient.connect("PylontechMonitor");

      if (ok)
      {
        pubStr("pylontech/status", "online");
        mqttReady = true;
      }
      else
      {
        mqttReady = false;
      }
    }
    return;
  }

  mqttClient.loop();

  if (millis() - lastMqttPublish > MQTT_INTERVAL)
  {
    lastMqttPublish = millis();
    publishSystemStats();
  }
}

// ==========================
// WATCHDOG (WLAN + MQTT)
// ==========================

void checkWatchdogs()
{
  static unsigned long wifiLostSince = 0;
  static unsigned long lastReconnectTry = 0;

  if (WiFi.status() != WL_CONNECTED)
  {
    if (!wifiLostSince) {
      wifiLostSince = millis();
      lastReconnectTry = millis();
    }
    if (millis() - lastReconnectTry > WIFI_RECONNECT_MS) {
      lastReconnectTry = millis();
      WiFi.disconnect();
      WiFi.reconnect();
    }
    if (millis() - wifiLostSince > WIFI_RESTART_MS) {
      ESP.restart();
    }
  }
  else
  {
    wifiLostSince = 0;
  }

  // Kein erfolgreicher MQTT-Publish mehr -> Neustart
  if (strlen(mqtt_server) > 0 && (millis() - lastMqttSuccess) > MQTT_RESTART_MS) {
    ESP.restart();
  }
}

// ==========================
// SYSTEM STATS CALCULATOR
// ==========================

void calcSystem()
{
  float socSum = 0;
  float voltageSum = 0;
  int count = 0;

  systemStats.minCell = 999.0;
  systemStats.maxCell = 0.0;
  systemStats.totalCurrent = 0;
  systemStats.maxSystemTemp = -99.0;
  systemStats.maxSystemBmsTemp = -99.0;

  for(int i = 0; i < 16; i++)
  {
    Battery &b = batteries[i];
    if(!b.present) continue;

    if(pwrFresh(b))
    {
      socSum += b.soc;
      voltageSum += b.voltage;
      systemStats.totalCurrent += b.current;
      count++;

      if(b.tempHigh > systemStats.maxSystemTemp) systemStats.maxSystemTemp = b.tempHigh;
      if(b.temp > systemStats.maxSystemBmsTemp) systemStats.maxSystemBmsTemp = b.temp;
    }

    if(cellsFresh(b))
    {
      for(int c = 0; c < b.cellCount; c++)
      {
        float v = b.cellVoltages[c];
        if(v > 2.0 && v < 4.0)
        {
          if(v < systemStats.minCell) systemStats.minCell = v;
          if(v > systemStats.maxCell) systemStats.maxCell = v;
        }
      }
    }
  }

  if(count > 0)
  {
    systemStats.valid = true;
    systemStats.avgSOC = socSum / count;
    systemStats.avgVoltage = voltageSum / count;
  }
  else
  {
    systemStats.valid = false;
  }
}

// ==========================
// DASHBOARD & SUBPAGES
// ==========================

String buildDashboard()
{
  String html;
  html.reserve(5000);

  html += "<html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='5'>";

  html += "<style>";
  html += "body{margin:0;font-family:Arial,sans-serif;background:#0f1115;color:#e6e6e6;}";
  html += "h1{margin:0;padding:15px;text-align:center;background:#151a22;color:#00e5ff;}";
  html += ".container{padding:10px;max-width:1100px;margin:auto;}";
  html += ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px;}";
  html += ".box{background:#1b1f2a;padding:15px;border-radius:10px;text-decoration:none;color:inherit;display:block;border:1px solid #2c3244;transition:0.2s;}";
  html += ".box:hover{border-color:#00e5ff;background:#222838;}";
  html += ".system{background:#141824;padding:15px;border-radius:10px;margin-bottom:15px;border:1px solid #1f2536;}";
  html += ".btn-container{display:flex;flex-wrap:wrap;gap:10px;margin-bottom:15px;}";
  html += ".btn-stat{padding:10px 15px;background:#00e5ff;color:#0f1115;text-decoration:none;border-radius:5px;font-weight:bold;transition:0.2s;}";
  html += ".btn-stat:hover{background:#00b3cc;}";
  html += ".stale{color:#ebcb8b;font-weight:bold;}";
  html += "</style></head><body>";

  html += "<h1>Pylontech Monitor</h1>";
  html += "<div class='container'>";

  html += "<strong>Manuelle Rohdatenabfrage:</strong><br>";
  html += "<div class='btn-container'>";
  html += "<a class='btn-stat' href='/triggerstat?id=1'>&#9889; STAT Bat</a>";
  html += "</div>";

  html += "<strong>System-Optionen:</strong><br>";
  html += "<div class='btn-container'>";
  html += "<a class='btn-stat' style='background:#f44336; color:#fff;' href='/resetwifi' onclick=\"return confirm('WLAN & MQTT wirklich löschen und neu starten?');\">&#9888;&#65039; Monitor zurücksetzen</a>";
  html += "</div>";

  html += "<div class='system'>";
  html += "<strong>Gesamtsystem:</strong><br><br>";

  if(systemStats.valid)
  {
    html += "Ø SOC: " + String(systemStats.avgSOC, 1) + "%<br>";
    html += "Ø Spannung: " + String(systemStats.avgVoltage, 2) + " V<br>";
    html += "Gesamtstrom: " + String(systemStats.totalCurrent, 2) + " A<br>";
  }
  else
  {
    html += "Lade Daten...<br>";
  }

  if (systemStats.maxSystemTemp > -99.0) {
    html += "Max. System-Zelltemperatur: " + String(systemStats.maxSystemTemp, 1) + " °C<br>";
  } else {
    html += "Max. System-Zelltemperatur: Lade Daten...<br>";
  }

  if (systemStats.maxSystemBmsTemp > -99.0) {
    html += "Max. System-BMS-Temperatur: " + String(systemStats.maxSystemBmsTemp, 1) + " °C<br>";
  } else {
    html += "Max. System-BMS-Temperatur: Lade Daten...<br>";
  }

  if(systemStats.maxCell > 0) {
    html += "Min Zelle: " + String(systemStats.minCell, 3) + " V<br>";
    html += "Max Zelle: " + String(systemStats.maxCell, 3) + " V<br>";
  }
  html += "<br>MQTT Broker IP: " + (strlen(mqtt_server) > 0 ? String(mqtt_server) : String("Keine (Über Setup einrichten)")) + "<br>";
  html += "</div>";

  html += "<h3>Aktivierte Batterien (Klicken für Details)</h3><div class='grid'>";

  for(int i = 0; i < 16; i++)
  {
    if(!batteries[i].present) continue;

    html += "<a class='box' href='/battery?id=" + String(i + 1) + "'>";
    html += "<strong>BATTERIE " + String(i + 1) + "</strong><br><br>";
    if(!pwrFresh(batteries[i])) {
      html += "<span class='stale'>&#9888; Daten veraltet</span><br>";
    }
    html += "SOC: " + String(batteries[i].soc, 0) + "%<br>";
    html += "Spannung: " + String(batteries[i].voltage, 2) + " V<br>";
    html += "Strom: " + String(batteries[i].current, 2) + " A<br>";
    html += "</a>";
  }

  html += "</div></div></body></html>";
  return html;
}

String buildBatteryPage(int id)
{
  if(id < 1 || id > 16) return "Ungültige Batterie ID";
  Battery &b = batteries[id - 1];

  String html;
  html.reserve(4000);

  html += "<html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='5'>";
  html += "<style>";
  html += "body{margin:0;font-family:Arial,sans-serif;background:#0f1115;color:#e6e6e6;}";
  html += "h1{margin:0;padding:15px;text-align:center;background:#151a22;color:#00e5ff;}";
  html += ".container{padding:10px;max-width:600px;margin:auto;}";
  html += ".btn{display:inline-block;padding:8px 15px;background:#222838;color:#00e5ff;text-decoration:none;border-radius:5px;margin-bottom:15px;border:1px solid #00e5ff;}";
  html += "table{width:100%;border-collapse:collapse;margin-top:10px;}";
  html += "th,td{padding:10px;text-align:left;border-bottom:1px solid #2c3244;}";
  html += "th{background:#151a22;color:#00e5ff;}";
  html += ".box{background:#1b1f2a;padding:15px;border-radius:10px;margin-bottom:15px;}";
  html += ".stale{color:#ebcb8b;font-weight:bold;}";
  html += "</style></head><body>";

  html += "<h1>Batterie " + String(id) + " Details</h1>";
  html += "<div class='container'>";
  html += "<a class='btn' href='/'>&larr; Zurück</a>";

  html += "<div class='box'>";
  html += "<strong>Zustand:</strong><br>";
  if(!pwrFresh(b)) html += "<span class='stale'>&#9888; Daten veraltet</span><br>";
  html += "SOC: " + String(b.soc, 1) + " %<br>";
  html += "Spannung: " + String(b.voltage, 2) + " V<br>";
  html += "Strom: " + String(b.current, 2) + " A<br>";

  html += "BMS Board-Temperatur: " + String(b.temp, 1) + " °C<br>";
  html += "Zelltemperatur Min: " + String(b.tempLow, 1) + " °C<br>";
  html += "Zelltemperatur Max (Echte Temp): " + String(b.tempHigh, 1) + " °C<br>";
  html += "</div>";

  html += "<h3>Einzelzellspannungen</h3>";
  if(b.cellCount > 0 && !cellsFresh(b)) {
    html += "<div class='stale'>&#9888; Zelldaten veraltet</div>";
  }
  html += "<table><tr><th>Zelle</th><th>Spannung</th></tr>";

  if(b.cellCount == 0){
    html += "<tr><td colspan='2'>Lade Zelldaten... Bitte warten.</td></tr>";
  }else{
    for(int i = 0; i < b.cellCount; i++){
      html += "<tr>";
      html += "<td>Zelle " + String(i + 1) + "</td>";
      html += "<td>" + String(b.cellVoltages[i], 3) + " V</td>";
      html += "</tr>";
    }
  }
  html += "</table>";

  html += "</div></body></html>";
  return html;
}

String buildStatPage()
{
  String html;
  html.reserve(statOutput.length() + 2500);

  html += "<html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  if(statQueryInProgress) {
    html += "<meta http-equiv='refresh' content='2'>";
  }
  html += "<style>";
  html += "body{margin:0;font-family:Arial,sans-serif;background:#0f1115;color:#e6e6e6;}";
  html += "h1{margin:0;padding:15px;text-align:center;background:#151a22;color:#00e5ff;}";
  html += ".container{padding:10px;max-width:900px;margin:auto;}";
  html += ".btn{display:inline-block;padding:8px 15px;background:#222838;color:#00e5ff;text-decoration:none;border-radius:5px;margin-bottom:15px;border:1px solid #00e5ff;}";
  html += "pre{background:#05070a;padding:15px;border-radius:5px;border:1px solid #2c3244;overflow-x:auto;font-family:Consolas,Monaco,monospace;color:#a3be8c;white-space:pre-wrap;}";
  html += ".status-load{color:#ebcb8b;font-weight:bold;margin-bottom:10px;}";
  html += "</style></head><body>";

  html += "<h1>Ungeparste STAT Daten</h1>";
  html += "<div class='container'>";
  html += "<a class='btn' href='/'>&larr; Zurück zum Dashboard</a>";

  if(statQueryInProgress) {
    html += "<div class='status-load'>&#9203; Führe Befehl aus (stat)... Bitte warten.</div>";
  } else {
    html += "<div style='color:#a3be8c;margin-bottom:10px;'>&#9989; Abfrage beendet.</div>";
  }

  html += "<pre>";
  if(statOutput.length() == 0) {
    html += "Warte auf Antwort vom BMS...";
  } else {
    html += statOutput;
  }
  html += "</pre>";

  html += "</div></body></html>";
  return html;
}

// ==========================
// ROUTES
// ==========================

void handleRoot()
{
  server.send(200, "text/html", buildDashboard());
}

void handleBattery()
{
  if(server.hasArg("id")){
    int id = server.arg("id").toInt();
    server.send(200, "text/html", buildBatteryPage(id));
  }else{
    server.send(400, "text/plain", "Bad Request: Missing ID");
  }
}

void handleTriggerStat()
{
  int targetId = 1;
  if(server.hasArg("id")) {
    targetId = server.arg("id").toInt();
  }
  triggerStatQuery(targetId);

  server.sendHeader("Location", "/stat");
  server.send(302, "text/plain", "");
}

void handleStatPage()
{
  server.send(200, "text/html", buildStatPage());
}

// Wird vom WiFiManager aufgerufen, wenn im Portal gespeichert wurde.
// Die Werte stehen in den Parameter-Objekten, nicht in den char-Arrays.
void saveParamCallback() {
  strncpy(mqtt_server, custom_mqtt_server.getValue(), sizeof(mqtt_server) - 1);
  strncpy(mqtt_user, custom_mqtt_user.getValue(), sizeof(mqtt_user) - 1);
  strncpy(mqtt_pass, custom_mqtt_pass.getValue(), sizeof(mqtt_pass) - 1);
  mqtt_server[sizeof(mqtt_server) - 1] = 0;
  mqtt_user[sizeof(mqtt_user) - 1] = 0;
  mqtt_pass[sizeof(mqtt_pass) - 1] = 0;

  preferences.begin("mqtt-config", false);
  preferences.putString("broker", mqtt_server);
  preferences.putString("user", mqtt_user);
  preferences.putString("pass", mqtt_pass);
  preferences.end();
}

// ==========================
// SETUP
// ==========================

void setup()
{
  Serial.begin(115200);

  rxBuffer.reserve(320);

  preferences.begin("mqtt-config", true);
  preferences.getString("broker", "").toCharArray(mqtt_server, 40);
  preferences.getString("user", "").toCharArray(mqtt_user, 40);
  preferences.getString("pass", "").toCharArray(mqtt_pass, 40);
  preferences.end();

  // Gespeicherte Werte als Vorgabe im Portal anzeigen
  custom_mqtt_server.setValue(mqtt_server, 40);
  custom_mqtt_user.setValue(mqtt_user, 40);
  custom_mqtt_pass.setValue(mqtt_pass, 40);

  // --- WLAN: stärksten AP wählen, Powersave aus, Auto-Reconnect an ---
  WiFi.mode(WIFI_STA);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);

  WiFiManager wm;
  wm.addParameter(&custom_mqtt_server);
  wm.addParameter(&custom_mqtt_user);
  wm.addParameter(&custom_mqtt_pass);
  wm.setSaveParamsCallback(saveParamCallback);
  wm.setConnectTimeout(20);
  wm.setConfigPortalTimeout(180);   // Portal blockiert nicht mehr ewig

  wm.setHostname("pylontech");
  if (!wm.autoConnect("PylontechMonitor")) {
    // Keine Verbindung und Portal-Timeout abgelaufen -> neu versuchen
    ESP.restart();
  }

  // Sicherheitshalber nach dem Verbinden erneut setzen
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);

  // Aktuelle Werte aus dem Portal übernehmen
  strncpy(mqtt_server, custom_mqtt_server.getValue(), sizeof(mqtt_server) - 1);
  strncpy(mqtt_user, custom_mqtt_user.getValue(), sizeof(mqtt_user) - 1);
  strncpy(mqtt_pass, custom_mqtt_pass.getValue(), sizeof(mqtt_pass) - 1);
  mqtt_server[sizeof(mqtt_server) - 1] = 0;
  mqtt_user[sizeof(mqtt_user) - 1] = 0;
  mqtt_pass[sizeof(mqtt_pass) - 1] = 0;

  if (MDNS.begin("pylontech")) {
    MDNS.addService("http", "tcp", 80);
  }

  // --- MQTT: Server nur einmal setzen, kurzer Timeout ---
  mqttClient.setServer(mqtt_server, 1883);
  mqttClient.setSocketTimeout(2);
  espClient.setTimeout(2000);
  lastMqttSuccess = millis();

  // --- Serial: großer RX-Puffer VOR begin() ---
  Serial0.setRxBufferSize(2048);
  Serial0.begin(RS232_BAUD, SERIAL_8N1, RS232_RX_PIN, RS232_TX_PIN);

  server.on("/", handleRoot);
  server.on("/battery", handleBattery);
  server.on("/triggerstat", handleTriggerStat);
  server.on("/stat", handleStatPage);

  server.on("/resetwifi", [](){
    server.send(200, "text/html", "<html><body style='background:#0f1115;color:#fff;'><h3>Monitor wird zurückgesetzt...</h3><p>WLAN-Daten gelöscht. Bitte verbinde dich gleich neu mit dem WLAN 'PylontechMonitor'.</p></body></html>");
    delay(2000);
    WiFiManager wm_reset;
    wm_reset.resetSettings();
    preferences.begin("mqtt-config", false);
    preferences.clear();
    preferences.end();
    ESP.restart();
  });

  server.onNotFound([](){
    server.send(404, "text/plain", "Not Found");
  });

  server.begin();
}

// ==========================
// LOOP
// ==========================

void loop()
{
  if(statQueryInProgress && (millis() - statStartTime > 12000)) {
    Serial0.print("unit 1\n");
    statQueryInProgress = false;
    currentMode = MODE_NONE;
  }

  if(!statQueryInProgress)
  {
    servicePendingBat();

    if(millis() - lastSend > 5500)
    {
      if(nextQueryIndex == 0)
      {
        requestPwr();
        nextQueryIndex = 1;
      }
      else
      {
        bool queried = false;
        for(int i = nextQueryIndex - 1; i < 16; i++){
          if(batteries[i].present){
            requestBat(i + 1);
            nextQueryIndex = i + 2;
            if(nextQueryIndex > 16) nextQueryIndex = 0;
            queried = true;
            break;
          }
        }
        if(!queried){
          // Alle Akkus durch: zurück auf Unit 1, nächster Slot startet wieder mit "pwr"
          if(currentMode == MODE_BAT) commitBat();
          Serial0.print("unit 1\n");
          nextQueryIndex = 0;
        }
      }
      lastSend = millis();
    }
  }

  readSerial();

  static unsigned long lastCalc = 0;
  if(millis() - lastCalc > 1000) {
    lastCalc = millis();
    calcSystem();
  }

  handleMqtt();
  checkWatchdogs();
  server.handleClient();
}
