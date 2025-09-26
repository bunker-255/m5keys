#include <M5StickCPlus2.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <vector>
#include <map>
#include "time.h"

// --- СТРУКТУРЫ ДАННЫХ ---
struct Command { String label; String command_id; };
std::vector<String> categoryNames;
std::map<String, std::vector<Command>> categorizedCommands;
int currentCategoryIndex = 0;
int currentScriptIndex = 0;

// --- МАШИНА СОСТОЯНИЙ ---
enum DisplayState { STATE_CATEGORIES, STATE_SCRIPTS, STATE_VOLUME, STATE_SCREENSAVER };
DisplayState currentState = STATE_CATEGORIES;

// --- НАСТРОЙКИ СНА И ВРЕМЕНИ ---
long screensaverTimeoutMs = 1 * 60 * 1000;
long screenOffTimeoutMs = 2 * 60 * 1000;
long timezoneOffsetSec = 3 * 3600;
unsigned long lastActivityTime = 0;
bool isScreenOff = false;

WebServer server(80);
Preferences preferences;
String saved_ssid, saved_pass, pc_ip_address, mDNS_hostname;
String server_name = "Unknown PC";

// --- ВЕБ-СТРАНИЦА НАСТРОЕК ---
const char* config_html PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><title>M5 Remote Setup</title><meta name="viewport" content="width=device-width, initial-scale=1">
<style>body{font-family:Arial,sans-serif;background-color:#2c3e50;color:#ecf0f1;text-align:center;padding:20px;}h1,h2{color:#f39c12;} .container{background-color:#34495e;padding:20px;border-radius:10px;display:inline-block; max-width: 400px;}input,select{width:95%;padding:12px;margin:10px 0;font-size:1.1em;border-radius:5px;border:none;background-color:#2c3e50;color:#ecf0f1;}button{width: 95%; padding:12px 25px;font-size:1.1em;background-color:#f39c12;border:none;color:#fff;cursor:pointer;border-radius:5px;}label{display:block;margin-top:15px;color:#bdc3c7;}
</style></head><body><div class="container"><h1>M5 Remote Setup</h1><form action="/save" method="post">
<h2>PC & Network</h2>
<label for="pc_ip">PC Server IP:</label><input type="text" id="pc_ip" name="pc_ip" value="%%PC_IP%%">
<label for="wifi_ssid">Wi-Fi SSID:</label><input type="text" id="wifi_ssid" name="wifi_ssid" value="%%WIFI_SSID%%">
<label for="wifi_pass">Wi-Fi Password:</label><input type="password" id="wifi_pass" name="wifi_pass" placeholder="Leave blank to keep old">
<h2>Sleep & Time</h2>
<label for="timezone">Timezone:</label>
<select id="timezone" name="timezone">%%TIMEZONE_OPTIONS%%</select>
<label for="screensaver_time">Inactivity for Clock (minutes):</label><input type="number" id="screensaver_time" name="screensaver_time" value="%%SCREENSAVER_TIME%%">
<label for="screenoff_time">Clock on Screen (minutes):</label><input type="number" id="screenoff_time" name="screenoff_time" value="%%SCREENOFF_TIME%%">
<br><br><button type="submit">Save & Reboot</button></form></div></body></html>)rawliteral";

void handleRoot() {
    String page = String(config_html);
    page.replace("%%PC_IP%%", pc_ip_address);
    page.replace("%%WIFI_SSID%%", saved_ssid);
    page.replace("%%SCREENSAVER_TIME%%", String(screensaverTimeoutMs / 60000));
    page.replace("%%SCREENOFF_TIME%%", String(screenOffTimeoutMs / 60000));
    String tz_options;
    for (int i = -12; i <= 14; i++) {
        tz_options += "<option value=\"" + String(i * 3600) + "\"";
        if (i * 3600 == timezoneOffsetSec) { tz_options += " selected"; }
        tz_options += ">UTC" + String(i >= 0 ? "+" : "") + String(i) + "</option>\n";
    }
    page.replace("%%TIMEZONE_OPTIONS%%", tz_options);
    server.send(200, "text/html", page);
}

void handleSave() {
    preferences.begin("settings", false);
    if(server.hasArg("wifi_ssid")&&server.arg("wifi_ssid").length()>0) preferences.putString("wifi_ssid", server.arg("wifi_ssid"));
    if(server.hasArg("wifi_pass")&&server.arg("wifi_pass").length()>0) preferences.putString("wifi_pass", server.arg("wifi_pass"));
    if(server.hasArg("pc_ip")&&server.arg("pc_ip").length()>6) preferences.putString("pc_ip", server.arg("pc_ip"));
    if(server.hasArg("timezone")) preferences.putLong("tz_offset", server.arg("timezone").toInt());
    if(server.hasArg("screensaver_time")) preferences.putLong("ss_time", server.arg("screensaver_time").toInt() * 60000);
    if(server.hasArg("screenoff_time")) preferences.putLong("so_time", server.arg("screenoff_time").toInt() * 60000);
    preferences.end();
    server.send(200, "text/html", "<h1>Saved!</h1><p>Rebooting...</p>"); delay(3000); ESP.restart();
}

void startAccessPointMode() {
    const char* ap="M5-Remote-Setup"; WiFi.softAP(ap);
    M5.Lcd.fillScreen(TFT_DARKCYAN); M5.Lcd.setTextColor(WHITE); M5.Lcd.setTextSize(2); M5.Lcd.setTextDatum(TL_DATUM);
    M5.Lcd.setCursor(10,10); M5.Lcd.printf("--- AP MODE ---");
    M5.Lcd.setCursor(10,45); M5.Lcd.printf("Connect to Wi-Fi:\n '%s'", ap);
    M5.Lcd.setCursor(10,100); M5.Lcd.printf("Open in browser:\n %s", WiFi.softAPIP().toString().c_str());
    server.on("/", handleRoot); server.on("/save", HTTP_POST, handleSave); server.begin();
}

bool fetchServerProfile() {
    if (pc_ip_address.length() == 0) return false;
    HTTPClient http; String url = "http://" + pc_ip_address + ":8080/info";
    M5.Lcd.fillScreen(BLACK); M5.Lcd.setTextDatum(MC_DATUM); M5.Lcd.drawString("Loading Profile...", 120, 68);
    http.begin(url); int httpCode = http.GET();
    if (httpCode != 200) { http.end(); return false; }
    String payload = http.getString(); http.end();
    auto doc = new JsonDocument();
    if (!doc) { return false; }
    DeserializationError error = deserializeJson(*doc, payload);
    if (error) { delete doc; return false; }
    categoryNames.clear(); categorizedCommands.clear(); currentCategoryIndex = 0; currentScriptIndex = 0;
    server_name = (*doc)["profile"]["name"].as<String>();
    JsonObject categories_obj = (*doc)["profile"]["categories"].as<JsonObject>();
    for (JsonPair kv : categories_obj) {
        String category_name = kv.key().c_str(); categoryNames.push_back(category_name);
        std::vector<Command> current_commands;
        for (JsonObject cmd_obj : kv.value().as<JsonArray>()) {
            current_commands.push_back({cmd_obj["label"].as<String>(), cmd_obj["command_id"].as<String>()});
        }
        categorizedCommands[category_name] = current_commands;
    }
    delete doc; return true;
}

void drawBatteryIndicator() {
    int level = M5.Power.getBatteryLevel(); bool is_charging = M5.Power.isCharging(); float voltage = M5.Power.getBatteryVoltage() / 1000.0f;
    int icon_x = 205, icon_y = 8, icon_w = 28, icon_h = 14;
    M5.Lcd.setTextDatum(TR_DATUM); M5.Lcd.setTextSize(2);
    if (is_charging) { M5.Lcd.setTextColor(YELLOW, BLACK); M5.Lcd.drawString("~", icon_x - 4, icon_y); } 
    else { M5.Lcd.setTextColor(WHITE, BLACK); M5.Lcd.drawString(String(level) + "%", icon_x - 4, icon_y); }
    M5.Lcd.setTextSize(1); M5.Lcd.setTextColor(TFT_WHITE, BLACK); M5.Lcd.drawString(String(voltage, 2) + "V", 238, icon_y + icon_h + 2);
    M5.Lcd.drawRect(icon_x, icon_y, icon_w, icon_h, WHITE); M5.Lcd.fillRect(icon_x + icon_w, icon_y + 3, 2, icon_h - 6, WHITE);
    M5.Lcd.fillRect(icon_x + 2, icon_y + 2, icon_w - 4, icon_h - 4, BLACK);
    int fill_w = (icon_w - 4) * level / 100; uint32_t fill_color = (level > 50) ? GREEN : (level > 20) ? ORANGE : RED;
    M5.Lcd.fillRect(icon_x + 2, icon_y + 2, fill_w, icon_h - 4, fill_color);
}

void drawScreensaver() {
    M5.Lcd.fillScreen(BLACK); drawBatteryIndicator(); struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){ M5.Lcd.setTextDatum(MC_DATUM); M5.Lcd.drawString("Syncing time...", 120, 80); return; }
    char time_str[6]; char date_str[12]; strftime(time_str, sizeof(time_str), "%H:%M", &timeinfo); strftime(date_str, sizeof(date_str), "%d %b %Y", &timeinfo);
    M5.Lcd.setTextColor(WHITE, BLACK); M5.Lcd.setTextDatum(MC_DATUM); M5.Lcd.setTextSize(6); M5.Lcd.drawString(time_str, 120, 70);
    M5.Lcd.setTextDatum(BC_DATUM); M5.Lcd.setTextSize(2); M5.Lcd.drawString(date_str, 120, 130);
}

void drawCategoryMenu() {
    M5.Lcd.fillScreen(BLACK); drawBatteryIndicator(); M5.Lcd.setTextDatum(TL_DATUM); M5.Lcd.setTextSize(2); M5.Lcd.setCursor(10, 10); M5.Lcd.setTextColor(GREEN); M5.Lcd.printf("PC: %s", server_name.c_str());
    M5.Lcd.drawRect(10, 50, 220, 60, WHITE); M5.Lcd.setTextSize(3); M5.Lcd.setTextColor(WHITE); M5.Lcd.setTextDatum(MC_DATUM);
    if (!categoryNames.empty()) { M5.Lcd.drawString(categoryNames[currentCategoryIndex], 120, 80); } else { M5.Lcd.drawString("No Categories", 120, 80); }
}

void drawScriptMenu() {
    M5.Lcd.fillScreen(TFT_NAVY); drawBatteryIndicator(); M5.Lcd.setTextDatum(TL_DATUM); M5.Lcd.setTextSize(2); M5.Lcd.setCursor(10, 10); M5.Lcd.setTextColor(CYAN);
    if (!categoryNames.empty()) M5.Lcd.printf("Cat: %s", categoryNames[currentCategoryIndex].c_str());
    M5.Lcd.drawRect(10, 50, 220, 60, WHITE); M5.Lcd.setTextSize(3); M5.Lcd.setTextColor(WHITE); M5.Lcd.setTextDatum(MC_DATUM);
    if (currentScriptIndex == 0) { M5.Lcd.drawString("< Back", 120, 80); } 
    else { String category_name = categoryNames[currentCategoryIndex]; M5.Lcd.drawString(categorizedCommands[category_name][currentScriptIndex - 1].label, 120, 80); }
}

void drawVolumeUI() {
    M5.Lcd.fillScreen(TFT_DARKGREY); drawBatteryIndicator(); M5.Lcd.setTextDatum(TC_DATUM); M5.Lcd.setTextSize(3); M5.Lcd.setTextColor(WHITE); M5.Lcd.drawString("Volume", 120, 10);
    M5.Lcd.setTextDatum(MC_DATUM); M5.Lcd.setTextSize(6); M5.Lcd.drawString("+", 200, 80); M5.Lcd.drawString("-", 40, 80);
    M5.Lcd.setTextDatum(BC_DATUM); M5.Lcd.setTextSize(2); M5.Lcd.drawString("Press A to Exit", 120, 130);
}

void drawCurrentState() {
    switch(currentState) {
        case STATE_CATEGORIES:  drawCategoryMenu(); break;
        case STATE_SCRIPTS:     drawScriptMenu(); break;
        case STATE_VOLUME:      drawVolumeUI(); break;
        case STATE_SCREENSAVER: drawScreensaver(); break;
    }
}

void sendCommand(const String& command_id) {
    if(WiFi.status()!=WL_CONNECTED)return; String url="http://"+pc_ip_address+":8080/execute?command="+command_id;
    if(command_id=="open_settings"){url+="&url=http://"+WiFi.localIP().toString();} HTTPClient http;
    if(currentState==STATE_SCRIPTS)M5.Lcd.fillCircle(120, 80, 25, YELLOW);
    http.begin(url); int httpCode=http.GET(); http.end();
    if(currentState==STATE_VOLUME){M5.Lcd.fillScreen(httpCode==200?TFT_GREEN:TFT_RED); delay(80); drawVolumeUI();}
    else{if(httpCode==200)M5.Lcd.fillCircle(120, 80, 25, GREEN); else M5.Lcd.fillCircle(120, 80, 25, RED); delay(500); drawCurrentState();}
}

void setup() {
    M5.begin(); M5.Lcd.setRotation(3); M5.update();
    
    preferences.begin("settings", true);
    saved_ssid = preferences.getString("wifi_ssid",""); saved_pass = preferences.getString("wifi_pass","");
    pc_ip_address = preferences.getString("pc_ip","");
    screensaverTimeoutMs = preferences.getLong("ss_time", 1 * 60000); screenOffTimeoutMs = preferences.getLong("so_time", 2 * 60000);
    timezoneOffsetSec = preferences.getLong("tz_offset", 3 * 3600);
    preferences.end();
    
    M5.Lcd.setTextSize(2); M5.Lcd.fillScreen(BLACK); M5.Lcd.setCursor(10,10);

    // --- ЛОГИКА ПЕРЕКЛЮЧЕНИЯ В РЕЖИМ AP ---
    // Сценарий 1: Сеть еще не настроена
    if(saved_ssid.length() == 0) {
        M5.Lcd.println("No Wi-Fi config."); delay(2000);
        startAccessPointMode();
        return; // Прекращаем выполнение setup()
    }
    
    M5.Lcd.printf("Connecting to\n'%s'...", saved_ssid.c_str());
    WiFi.begin(saved_ssid.c_str(), saved_pass.c_str());
    for(int i = 0; i < 20; i++) { if(WiFi.status() == WL_CONNECTED) break; delay(500); M5.Lcd.print("."); }
    
    // Сценарий 2: Не удалось подключиться к сохраненной сети
    if(WiFi.status() != WL_CONNECTED) {
        M5.Lcd.fillScreen(RED); M5.Lcd.setCursor(10,10); M5.Lcd.println("Connection Failed!"); delay(2000);
        startAccessPointMode();
        return; // Прекращаем выполнение setup()
    }
    
    // Если подключение успешно:
    configTime(timezoneOffsetSec, 0, "pool.ntp.org");
    server.on("/",handleRoot); server.on("/save",HTTP_POST,handleSave); server.begin();
    if(!fetchServerProfile()){categoryNames.clear();categorizedCommands.clear();server_name="PC Connect Error";categoryNames.push_back("System");categorizedCommands["System"].push_back({"Settings","open_settings"});}
    lastActivityTime = millis();
    drawCurrentState();
}

void loop() {
    server.handleClient(); M5.update(); if (WiFi.getMode() == WIFI_AP) return;

    if (isScreenOff) {
        if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnPWR.wasPressed()) {
            M5.Display.wakeup(); isScreenOff = false; lastActivityTime = millis();
            currentState = STATE_SCREENSAVER; drawCurrentState();
        }
        return;
    }

    if (currentState != STATE_SCREENSAVER && screensaverTimeoutMs > 0 && (millis() - lastActivityTime > screensaverTimeoutMs)) {
        currentState = STATE_SCREENSAVER; drawCurrentState();
    }
    if (currentState == STATE_SCREENSAVER && screenOffTimeoutMs > 0 && (millis() - lastActivityTime > screensaverTimeoutMs + screenOffTimeoutMs)) {
        M5.Display.sleep(); isScreenOff = true;
    }
    if (currentState == STATE_SCREENSAVER && millis() % 30000 < 20) {
        drawCurrentState();
    }

    bool buttonPressed = M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnPWR.wasPressed();
    if (buttonPressed && currentState != STATE_SCREENSAVER) {
        lastActivityTime = millis();
    }
    
    bool needsRedraw = false;
    switch(currentState) {
        case STATE_SCREENSAVER:
            if (buttonPressed) {
                lastActivityTime = millis(); currentState = STATE_CATEGORIES; needsRedraw = true;
            }
            break;
        case STATE_CATEGORIES:
            if (M5.BtnA.wasPressed() && !categoryNames.empty()) { currentState = STATE_SCRIPTS; currentScriptIndex = 0; needsRedraw = true; }
            if (M5.BtnB.wasPressed() && !categoryNames.empty()) { currentCategoryIndex = (currentCategoryIndex + 1) % categoryNames.size(); needsRedraw = true; }
            if (M5.BtnPWR.wasPressed() && !categoryNames.empty()) { currentCategoryIndex = (currentCategoryIndex == 0) ? (categoryNames.size() - 1) : (currentCategoryIndex - 1); needsRedraw = true; }
            break;
        case STATE_SCRIPTS:
            if (M5.BtnA.wasPressed()) { if (currentScriptIndex == 0) { currentState = STATE_CATEGORIES; needsRedraw = true; } else { String cat_name=categoryNames[currentCategoryIndex]; Command cmd=categorizedCommands[cat_name][currentScriptIndex-1]; if(cmd.command_id=="volume_mode"){currentState=STATE_VOLUME;needsRedraw=true;} else {sendCommand(cmd.command_id);}}}
            if (M5.BtnB.wasPressed()) { String cat_name=categoryNames[currentCategoryIndex]; int total=categorizedCommands[cat_name].size()+1; currentScriptIndex=(currentScriptIndex+1)%total; needsRedraw=true;}
            if (M5.BtnPWR.wasPressed()) { String cat_name=categoryNames[currentCategoryIndex]; int total=categorizedCommands[cat_name].size()+1; currentScriptIndex=(currentScriptIndex==0)?(total-1):(currentScriptIndex-1); needsRedraw=true;}
            break;
        case STATE_VOLUME:
            if (M5.BtnA.wasPressed()) { currentState = STATE_SCRIPTS; needsRedraw = true; }
            if (M5.BtnB.wasPressed()) { sendCommand("vol_up"); }
            if (M5.BtnPWR.wasPressed()) { sendCommand("vol_down"); }
            break;
    }
    
    if (needsRedraw) { drawCurrentState(); }
}
