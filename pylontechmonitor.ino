#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ESPmDNS.h>        
#include <PubSubClient.h>  
#include <Preferences.h>   
#include "utilities.h"

// ==========================
// MQTT GLOBALS
// ==========================
WiFiClient espClient;
PubSubClient mqttClient(espClient);
Preferences preferences;

char mqtt_server[40] = "";
char mqtt_user[40] = "";     
char mqtt_pass[40] = "";     

unsigned long lastMqttRetry = 0;
unsigned long lastMqttPublish = 0;
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

  float temp = 0;     // BMS / Board-Temperatur
  float tempLow = 0;  // Min Zell-Temperatur
  float tempHigh = 0; // Max Zell-Temperatur

  float cellMin = 0;
  float cellMax = 0;

  // Speicher für Einzelzellen (0 bis 15)
  float cellVoltages[16] = {0};
  int cellCount = 0;

  String raw;
};

Battery batteries[16];

// ==========================
// SYSTEM STATS
// ==========================

struct SystemStats {
  float avgSOC = 0;
  float avgVoltage = 0;
  float minCell = 999;
  float maxCell = 0;
  float totalCurrent = 0;
  float maxSystemTemp = -99.0;    // Maximale Zelltemperatur im Gesamtsystem
  float maxSystemBmsTemp = -99.0; // NEU: Maximale BMS-Temperatur im Gesamtsystem
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

// Variablen für die manuelle STAT-Abfrage
String statOutput = "";
bool statQueryInProgress = false;
unsigned long statStartTime = 0;
int statTargetUnit = 1;      
int statStep = 0;            

// ==========================
// WEB SERVER
// ==========================

WebServer server(80);

// ==========================
// REQUESTS
// ==========================

void requestPwr()
{
  currentMode = MODE_PWR;
  rxBuffer = "";
  Serial0.print("pwr\n");
}

void requestBat(int id)
{
  Battery &b = batteries[id - 1];

  // Alte Zellwerte zurücksetzen
  memset(b.cellVoltages, 0, sizeof(b.cellVoltages));
  b.cellCount = 0;

  currentMode = MODE_BAT;
  targetBatId = id;
  rxBuffer = "";

  // Richtige Batterie anwählen vor dem Auslesen
  Serial0.print("unit ");
  Serial0.print(id);
  Serial0.print("\n");

  Serial0.print("bat ");
  Serial0.print(id);
  Serial0.print("\n");
}

// Startet die manuelle STAT-Abfrage
void triggerStatQuery(int unitId)
{
  currentMode = MODE_STAT;
  statOutput = "";
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
        v[idx++] = token;
        token = "";
      }
    }
    else
    {
      token += c;
    }
  }
  if(token.length()) v[idx++] = token;
  if(idx < 8) return;

  int id = v[0].toInt();
  if(id < 1 || id > 16) return;

  Battery &b = batteries[id - 1];
  b.present = true;

  b.voltage = v[1].toFloat() / 1000.0;
  b.current = v[2].toFloat() / 1000.0;
  
  b.temp = v[3].toFloat() / 1000.0;     // BMS-Board-Temperatur (v[3])
  b.tempLow = v[4].toFloat() / 1000.0;  // Min Zell-Temp (v[4])
  b.tempHigh = v[5].toFloat() / 1000.0; // Max Zell-Temp (v[5])

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
    
    if(cellVolt > 2.5 && cellVolt < 3.65)
    {
      b.cellVoltages[cellId] = cellVolt;
    }
    
    if((cellId + 1) > b.cellCount) {
      b.cellCount = cellId + 1;
    }
  }
}

// ==========================
// SERIAL READ
// ==========================

void readSerial()
{
  while(Serial0.available())
  {
    char c = Serial0.read();
    
    bool inStatMode = statQueryInProgress;
    if(inStatMode) {
      statOutput += c;
      
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
      }
      else if(currentMode == MODE_BAT){
        parseBatLine(rxBuffer);
      }
      rxBuffer = "";
    }
    else
    {
      if(c != '\r') rxBuffer += c;
    }
  }
}

// ==========================
// MQTT SENDER
// ==========================
void publishSystemStats()
{
  if (!mqttClient.connected()) return;

  mqttClient.publish("pylontech/system/avgSOC", String(systemStats.avgSOC, 1).c_str());
  mqttClient.publish("pylontech/system/avgVoltage", String(systemStats.avgVoltage, 2).c_str());
  mqttClient.publish("pylontech/system/totalCurrent", String(systemStats.totalCurrent, 2).c_str());
  mqttClient.publish("pylontech/system/minCell", String(systemStats.minCell, 3).c_str());
  mqttClient.publish("pylontech/system/maxCell", String(systemStats.maxCell, 3).c_str());
  
  // NEU: Beide System-Maximaltemperaturen getrennt an MQTT übertragen
  if (systemStats.maxSystemTemp > -99.0) {
    mqttClient.publish("pylontech/system/maxTemperature", String(systemStats.maxSystemTemp, 1).c_str());
  }
  if (systemStats.maxSystemBmsTemp > -99.0) {
    mqttClient.publish("pylontech/system/maxBmsTemperature", String(systemStats.maxSystemBmsTemp, 1).c_str());
  }

  for(int i = 0; i < 16; i++) {
    if(batteries[i].present) {
      String rootTopic = "pylontech/battery" + String(i + 1);
      mqttClient.publish((rootTopic + "/soc").c_str(), String(batteries[i].soc, 0).c_str());
      mqttClient.publish((rootTopic + "/voltage").c_str(), String(batteries[i].voltage, 2).c_str());
      mqttClient.publish((rootTopic + "/current").c_str(), String(batteries[i].current, 2).c_str());
      
      String tempTopic = rootTopic + "/temp";
      mqttClient.publish(tempTopic.c_str(), String(batteries[i].temp, 1).c_str()); 

      String maxZellTopic = rootTopic + "/cellTempMax";
      mqttClient.publish(maxZellTopic.c_str(), String(batteries[i].tempHigh, 1).c_str());

      for(int c = 0; c < batteries[i].cellCount; c++) {
        String cellTopic = rootTopic + "/cell" + String(c + 1);
        mqttClient.publish(cellTopic.c_str(), String(batteries[i].cellVoltages[c], 3).c_str());
      }
    }
  }
}

void handleMqtt()
{
  if (strlen(mqtt_server) == 0) return;

  if (WiFi.status() != WL_CONNECTED) return;

  mqttClient.setServer(mqtt_server, 1883);

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
        mqttClient.publish("pylontech/status", "online");
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
  systemStats.maxSystemBmsTemp = -99.0; // Reset für Neuberechnung

  for(int i = 0; i < 16; i++)
  {
    if(!batteries[i].present) continue;

    socSum += batteries[i].soc;
    voltageSum += batteries[i].voltage;
    systemStats.totalCurrent += batteries[i].current;
    count++;

    // Ermittlung der höchsten ZELL-Temperatur im System
    if(batteries[i].tempHigh > systemStats.maxSystemTemp) {
      systemStats.maxSystemTemp = batteries[i].tempHigh;
    }

    // NEU: Ermittlung der höchsten BMS-BOARD-Temperatur im System
    if(batteries[i].temp > systemStats.maxSystemBmsTemp) {
      systemStats.maxSystemBmsTemp = batteries[i].temp;
    }

    for(int c = 0; c < batteries[i].cellCount; c++)
    {
      float currentCellVolt = batteries[i].cellVoltages[c];

      if(currentCellVolt > 2.0 && currentCellVolt < 4.0) 
      {
        if(currentCellVolt < systemStats.minCell) {
          systemStats.minCell = currentCellVolt;
        }
        if(currentCellVolt > systemStats.maxCell) {
          systemStats.maxCell = currentCellVolt;
        }
      }
    }
  }

  if(count > 0)
  {
    systemStats.avgSOC = socSum / count;
    systemStats.avgVoltage = voltageSum / count;
  }
}

// ==========================
// DASHBOARD & SUBPAGES
// ==========================

String buildDashboard()
{
  String html = "<html><head><meta\ncharset='utf-8'>";
  html += "<meta\nname='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta\nhttp-equiv='refresh' content='5'>";

  html += "<style>";
  html +="body{margin:0;font-family:Arial,sans-serif;background:#0f1115;color:#e6e6e6;}";
  html +="h1{margin:0;padding:15px;text-align:center;background:#151a22;color:#00e5ff;}";
  html += ".container{padding:10px;max-width:1100px;margin:auto;}";
  html +=".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px;}";
  html +=".box{background:#1b1f2a;padding:15px;border-radius:10px;text-decoration:none;color:inherit;display:block;border:1px\nsolid #2c3244;transition:0.2s;}";
  html += ".box:hover{border-color:#00e5ff;background:#222838;}";
  html +=".system{background:#141824;padding:15px;border-radius:10px;margin-bottom:15px;border:1px\nsolid #1f2536;}";
  html +=".btn-container{display:flex;flex-wrap:wrap;gap:10px;margin-bottom:15px;}";
  html +=".btn-stat{padding:10px\n15px;background:#00e5ff;color:#0f1115;text-decoration:none;border-radius:5px;font-weight:bold;transition:0.2s;}";
  html +=".btn-stat:hover{background:#00b3cc;}";
  html += "</style></head><body>";

  html += "<h1>Pylontech\nMonitor</h1>";
  html += "<div\nclass='container'>";

  html += "<strong>Manuelle\nRohdatenabfrage:</strong><br>";
  html += "<div\nclass='btn-container'>";
  html += "<a class='btn-stat' href='/triggerstat?id=1'>⚡ STAT Bat</a>";
  html += "</div>";

  html += "<strong>System-Optionen:</strong><br>";
  html += "<div class='btn-container'>";
  html += "<a class='btn-stat' style='background:#f44336; color:#fff;' href='/resetwifi' onclick=\"return confirm('WLAN & MQTT wirklich löschen und neu starten?');\">⚠️ Monitor zurücksetzen</a>";
  html += "</div>";

  html += "<div\nclass='system'>";
  html += "<strong>Gesamtsystem:</strong><br><br>";
  html += "Ø SOC: " + String(systemStats.avgSOC,1) + "%<br>";
  html += "Ø Spannung: " + String(systemStats.avgVoltage,2) + " V<br>";
  html += "Gesamtstrom: " + String(systemStats.totalCurrent,2) + " A<br>";
  
  // NEU: Anzeige beider Werte unter Gesamtsystem
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

  html += "Min Zelle: " + String(systemStats.minCell,3) + " V<br>";
  html += "Max Zelle: " + String(systemStats.maxCell,3) + " V<br>";
  html += "<br>MQTT Broker IP: " + (strlen(mqtt_server) > 0 ? String(mqtt_server) : String("Keine (Über Setup einrichten)")) + "<br>";
  html += "</div>";

  html += "<h3>Aktivierte\nBatterien (Klicken für Details)</h3><div class='grid'>";

  for(int i = 0; i < 16; i++)
  {
    if(!batteries[i].present) continue;

    html += "<a class='box'\nhref='/battery?id=" + String(i + 1) + "'>";
    html += "<strong>BATTERIE\n" + String(i + 1) + "</strong><br><br>";
    html += "SOC: " + String(batteries[i].soc,0) + "%<br>";
    html += "Spannung: " + String(batteries[i].voltage,2) + " V<br>";
    html += "Strom: " + String(batteries[i].current,2) + " A<br>";
    html += "</a>";
  }

  html += "</div></div></body></html>";
  return html;
}

String buildBatteryPage(int id)
{
  if(id < 1 || id > 16) return "Ungültige Batterie ID";
  Battery &b = batteries[id - 1];

  String html = "<html><head><meta\ncharset='utf-8'>";
  html += "<meta\nname='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta\nhttp-equiv='refresh' content='5'>";
  html += "<style>";
  html +="body{margin:0;font-family:Arial,sans-serif;background:#0f1115;color:#e6e6e6;}";
  html += "h1{margin:0;padding:15px;text-align:center;background:#151a22;color:#00e5ff;}";
  html += ".container{padding:10px;max-width:600px;margin:auto;}";
  html += ".btn{display:inline-block;padding:8px\n15px;background:#222838;color:#00e5ff;text-decoration:none;border-radius:5px;margin-bottom:15px;border:1px\nsolid #00e5ff;}";
  html += "table{width:100%;border-collapse:collapse;margin-top:10px;}";
  html += "th,td{padding:10px;text-align:left;border-bottom:1px solid\n#2c3244;}";
  html += "th{background:#151a22;color:#00e5ff;}";
  html +=".box{background:#1b1f2a;padding:15px;border-radius:10px;margin-bottom:15px;}";
  html += "</style></head><body>";

  html += "<h1>Batterie " + String(id) + " Details</h1>";
  html += "<div\nclass='container'>";
  html += "<a class='btn'\nhref='/'>&larr; Zurück</a>";

  html += "<div class='box'>";
  html += "<strong>Zustand:</strong><br>";
  html += "SOC: " + String(b.soc,1) + " %<br>";
  html += "Spannung: " + String(b.voltage,2) + " V<br>";
  html += "Strom: " + String(b.current,2) + " A<br>";
  
  html += "BMS Board-Temperatur: " + String(b.temp,1) + " °C<br>";
  html += "Zelltemperatur Min: " + String(b.tempLow,1) + " °C<br>";
  html += "Zelltemperatur Max (Echte Temp): " + String(b.tempHigh,1) + " °C<br>";
  html += "</div>";

  html += "<h3>Einzelzellspannungen</h3>";
  html += "<table><tr><th>Zelle</th><th>Spannung</th></tr>";

  if(b.cellCount == 0){
    html += "<tr><td\ncolspan='2'>Lade Zelldaten... Bitte warten.</td></tr>";
  }else{
    for(int i = 0; i < b.cellCount; i++){
      html += "<tr>";
      html += "<td>Zelle " + String(i + 1) + "</td>";
      html += "<td>" + String(b.cellVoltages[i],3) + " V</td>";
      html += "</tr>";
    }
  }
  html += "</table>";
  
  html += "</div></body></html>";
  return html;
}

String buildStatPage()
{
  String html = "<html><head><meta\ncharset='utf-8'>";
  html += "<meta\nname='viewport' content='width=device-width, initial-scale=1'>";
  if(statQueryInProgress) {
    html += "<meta\nhttp-equiv='refresh' content='2'>";
  }
  html += "<style>";
  html +="body{margin:0;font-family:Arial,sans-serif;background:#0f1115;color:#e6e6e6;}";
  html +="h1{margin:0;padding:15px;text-align:center;background:#151a22;color:#00e5ff;}";
  html += ".container{padding:10px;max-width:900px;margin:auto;}";
  html += ".btn{display:inline-block;padding:8px\n15px;background:#222838;color:#00e5ff;text-decoration:none;border-radius:5px;margin-bottom:15px;border:1px\nsolid #00e5ff;}";
  html += "pre{background:#05070a;padding:15px;border-radius:5px;border:1px\nsolid\n#2c3244;overflow-x:auto;font-family:Consolas,Monaco,monospace;color:#a3be8c;white-space:pre-wrap;}";
  html += ".status-load{color:#ebcb8b;font-weight:bold;margin-bottom:10px;}";
  html += "</style></head><body>"; // KORREKTUR: Fehlendes html += behoben

  html += "<h1>Ungeparste\nSTAT Daten</h1>"; 
  html += "<div\nclass='container'>";
  html += "<a class='btn'\nhref='/'>&larr; Zurück zum Dashboard</a>";

  if(statQueryInProgress) {
    html += "<div\nclass='status-load'>⏳ Führe Befehl aus (stat)... Bitte warten.</div>";
  } else {
    html += "<div\nstyle='color:#a3be8c;margin-bottom:10px;'>✅ Abfrage beendet.</div>";
  }

  html += "<pre>";
  if(statOutput.length() == 0) {
    html += "Warte auf Antwort vom\nBMS...";
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

void saveParamCallback() {
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

  preferences.begin("mqtt-config", true);
  preferences.getString("broker", "").toCharArray(mqtt_server, 40);
  preferences.getString("user", "").toCharArray(mqtt_user, 40);
  preferences.getString("pass", "").toCharArray(mqtt_pass, 40);
  preferences.end();

  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  
  WiFiManagerParameter custom_mqtt_server("server", "MQTT Broker IP", mqtt_server, 40);
  WiFiManagerParameter custom_mqtt_user("user", "MQTT Username (optional)", mqtt_user, 40);
  WiFiManagerParameter custom_mqtt_pass("pass", "MQTT Passwort (optional)", mqtt_pass, 40, "type='password'");
  
  wm.addParameter(&custom_mqtt_server);
  wm.addParameter(&custom_mqtt_user);
  wm.addParameter(&custom_mqtt_pass);
  wm.setSaveParamsCallback(saveParamCallback);
  
  wm.setHostname("pylontech");
  wm.autoConnect("PylontechMonitor");

  strncpy(mqtt_server, custom_mqtt_server.getValue(), 40);
  strncpy(mqtt_user, custom_mqtt_user.getValue(), 40);
  strncpy(mqtt_pass, custom_mqtt_pass.getValue(), 40);

  if (MDNS.begin("pylontech")) {
    MDNS.addService("http", "tcp", 80);
  }

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
  }

  if(!statQueryInProgress)
  {
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
          Serial0.print("unit 1\n");
          nextQueryIndex = 0;
          requestPwr();
        }
      }
      lastSend = millis();
    }
  }

  readSerial();
  calcSystem();
  handleMqtt(); 
  server.handleClient();
}
