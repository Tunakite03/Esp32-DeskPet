#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>
#include <algorithm>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <sys/time.h>
#include <time.h>
#include <vector>
#include "src/core/audio/audio.h"
#include "src/core/system/config.h"
#include "src/features/ui/layout.h"
#include "src/core/system/log.h"
#include "src/app/model.h"
#include "src/features/pet/pet_assets.h"
#include "src/app/state.h"
#include "src/features/talk/talk.h"
#include "src/core/system/timekeeping.h"
#include "src/core/input/touch_input.h"
#include "src/features/ui/ui.h"
#include "src/features/weather/weather.h"

namespace {
constexpr uint32_t WEATHER_INTERVAL = 15UL * 60000UL;
constexpr uint32_t WEATHER_RETRY_INTERVAL = 60000UL;
// DHCP and DNS are often not ready the moment Wi-Fi reports a connection.
constexpr uint32_t NETWORK_SETTLE = 3000;
constexpr uint32_t REACTION_DURATION = 1100;
constexpr uint32_t DIM_AFTER = 60000;
constexpr uint32_t FRAME_INTERVAL = 50;
constexpr uint32_t LONG_PRESS = 650;
constexpr int LONG_PRESS_SLOP = 15, TAP_SLOP = 25;

AppState state;
WebServer server(80);
Preferences preferences;

// Bookkeeping that only input handling and persistence need.
uint32_t alarmStartedAt = 0, lastAlarmCheck = 0;
bool deviceSettingsPending = false;
uint32_t deviceSettingsChangedAt = 0;
uint8_t petReactionIndex = 0, rapidPetTaps = 0;
uint32_t lastPetTapAt = 0, lastFrame = 0, lastWeatherAttempt = 0;
uint32_t lastSave = 0, buttonAt = 0, lastReconnect = 0;
bool buttonHeld = false, buttonWasDown = false, everConnected = false;
bool savePending = false, fatalError = false, setupOffered = false;
String formToken, networkName, networkPassword, serialLine;
uint32_t pendingConnectAt = 0;
// Set by any input so the next frame is drawn immediately instead of on the 50 ms tick.
bool inputPending = false;

// Worst-case timings since the last "perf" command, to find what makes input feel slow.
struct Perf {
  uint32_t loops = 0, renders = 0;
  uint32_t maxLoopUs = 0, maxRenderUs = 0, maxPumpUs = 0, maxSaveUs = 0, maxInputLatencyMs = 0;
  uint64_t totalRenderUs = 0;
};
Perf perf;
// Frames or input handling slower than this print a "slow_frame" event with its breakdown.
constexpr uint32_t SLOW_FRAME_US = 40000;
// Tile rows the display may send in one pass of the main loop.
constexpr uint32_t DISPLAY_SLICE_US = 6000;

bool petPageInteractive() {
  return !state.alarmRinging && !state.setup.open && state.page == PAGE_PET && !state.pet.pickerOpen;
}

void requestWeather(uint32_t now) {
  if (state.weather.busy || WiFi.status() != WL_CONNECTED) return;
  if (requestWeatherFetch(state.weather.place)) {
    state.weather.busy = true;
    lastWeatherAttempt = now;
  }
}

String escapeHtml(const String &value) {
  String result = value;
  result.replace("&", "&amp;"); result.replace("<", "&lt;");
  result.replace(">", "&gt;"); result.replace("\"", "&quot;"); result.replace("'", "&#39;");
  return result;
}

bool parseUnsigned(const String &input, uint32_t minimum, uint32_t maximum, uint32_t &result) {
  if (input.isEmpty() || input.length() > 10) return false;
  uint64_t value = 0;
  for (size_t i = 0; i < input.length(); ++i) {
    if (input[i] < '0' || input[i] > '9') return false;
    value = value * 10 + static_cast<uint8_t>(input[i] - '0');
    if (value > maximum) return false;
  }
  if (value < minimum) return false;
  result = static_cast<uint32_t>(value);
  return true;
}

// ---------------------------------------------------------------------------
// Setup portal (SoftAP + web form)

struct ScannedNetwork {
  String ssid;
  int32_t rssi;
  bool secure;
};

void triggerWifiScan() {
  if (WiFi.scanComplete() != -1) {
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
  }
}

std::vector<ScannedNetwork> getScannedNetworks() {
  std::vector<ScannedNetwork> list;
  const int16_t count = WiFi.scanComplete();
  if (count <= 0) return list;

  list.reserve(count > 30 ? 30 : count);
  for (int16_t i = 0; i < count; ++i) {
    String ssid = WiFi.SSID(i);
    ssid.trim();
    if (ssid.isEmpty()) continue;

    const int32_t rssi = WiFi.RSSI(i);
    const bool secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;

    bool found = false;
    for (auto &net : list) {
      if (net.ssid == ssid) {
        found = true;
        if (rssi > net.rssi) {
          net.rssi = rssi;
          net.secure = secure;
        }
        break;
      }
    }
    if (!found) {
      list.push_back({ssid, rssi, secure});
    }
  }

  std::sort(list.begin(), list.end(), [](const ScannedNetwork &a, const ScannedNetwork &b) {
    return a.rssi > b.rssi;
  });

  if (list.size() > 30) list.resize(30);
  return list;
}

bool setupClientAllowed() {
  const IPAddress address = server.client().remoteIP();
  return state.setup.open && address[0] == 192 && address[1] == 168 && address[2] == 4;
}

void handleScan() {
  if (!setupClientAllowed()) {
    server.send(403, "application/json", "{\"error\":\"forbidden\"}");
    return;
  }
  if (server.hasArg("rescan")) {
    triggerWifiScan();
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", "{\"status\":\"scanning\"}");
    return;
  }
  const int16_t status = WiFi.scanComplete();
  if (status == -1) {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", "{\"status\":\"scanning\"}");
    return;
  }
  if (status == -2) {
    triggerWifiScan();
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", "{\"status\":\"scanning\"}");
    return;
  }
  const std::vector<ScannedNetwork> networks = getScannedNetworks();
  JsonDocument doc;
  doc["status"] = "done";
  JsonArray arr = doc["networks"].to<JsonArray>();
  for (const auto &net : networks) {
    JsonObject obj = arr.add<JsonObject>();
    obj["ssid"] = net.ssid;
    obj["rssi"] = net.rssi;
    obj["secure"] = net.secure;
  }
  String response;
  serializeJson(doc, response);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", response);
}

void serveSetup() {
  if (!setupClientAllowed()) {
    server.send(403, "text/plain", "Connect to the setup Wi-Fi shown on the device.");
    return;
  }
  const int16_t scanStatus = WiFi.scanComplete();
  std::vector<ScannedNetwork> scanned;
  if (scanStatus >= 0) {
    scanned = getScannedNetworks();
  } else if (scanStatus == -2) {
    triggerWifiScan();
  }

  String html = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8">)HTML"
      R"HTML(<meta name="viewport" content="width=device-width,initial-scale=1"><title>DeskPet setup</title>)HTML"
      R"HTML(<style>body{background:#101724;color:#e9f3ef;font:16px system-ui;margin:30px auto;padding:20px;max-width:440px})HTML"
      R"HTML(h1{color:#7de2b7;margin:0 0 10px}label{display:block;margin:18px 0 7px})HTML"
      R"HTML(input,select,button{box-sizing:border-box;width:100%;padding:13px;border:1px solid #405063;border-radius:10px;background:#1c293b;color:#fff;font-size:15px})HTML"
      R"HTML(input:focus,select:focus{border-color:#7de2b7;outline:none})HTML"
      R"HTML(button{background:#7de2b7;color:#101724;margin-top:24px;font-weight:700;cursor:pointer})HTML"
      R"HTML(small{color:#a3b2be;display:block;margin-top:4px})HTML"
      R"HTML(.wifi-hdr{display:flex;justify-content:space-between;align-items:center;margin:18px 0 7px})HTML"
      R"HTML(.wifi-hdr label{margin:0})HTML"
      R"HTML(.btn-scan{width:auto;margin:0;padding:6px 14px;font-size:13px;font-weight:600;background:#24354a;color:#7de2b7;border:1px solid #405063;border-radius:8px;cursor:pointer})HTML"
      R"HTML(.btn-scan:disabled{opacity:0.6;cursor:wait})HTML"
      R"HTML(#manual_box{margin-top:8px;display:none})HTML"
      R"HTML(noscript #manual_box{display:block !important})HTML"
      R"HTML(noscript #wifi_select_wrap{display:none !important}</style></head><body>)HTML"
      R"HTML(<h1>Your little desk companion</h1>)HTML"
      R"HTML(<p>Set up once. Clock and alarms work offline after the time is synced.</p>)HTML"
      R"HTML(<form method="post" action="/save"><input type="hidden" name="token" value=")HTML";
  html += formToken;
  html += R"HTML("><div id="wifi_select_wrap"><div class="wifi-hdr">)HTML"
          R"HTML(<label for="wifi_select">Wi-Fi network</label>)HTML"
          R"HTML(<button type="button" class="btn-scan" id="scan_btn">Scan</button></div>)HTML";

  const String defaultLabel = networkName.isEmpty() ? "-- Select a Wi-Fi network --"
                                                    : "-- Keep current network (" + escapeHtml(networkName) + ") --";
  html += "<select id=\"wifi_select\" data-default=\"" + defaultLabel + "\">";
  html += "<option value=\"\">" + defaultLabel + "</option>";

  if (!scanned.empty()) {
    for (const auto &net : scanned) {
      String label = "📶 " + escapeHtml(net.ssid);
      if (net.secure) label += " 🔒";
      label += " (" + String(net.rssi) + " dBm)";
      html += "<option value=\"" + escapeHtml(net.ssid) + "\" data-secure=\"" + (net.secure ? "1" : "0") + "\">" +
              label + "</option>";
    }
  } else {
    html += R"HTML(<option value="" disabled id="scan_opt">Scanning nearby networks...</option>)HTML";
  }
  html += R"HTML(<option value="__manual__">&#9998; Enter manually / Hidden network...</option></select></div>)HTML";

  html += R"HTML(<div id="manual_box"><label for="ssid">Wi-Fi name</label>)HTML"
          R"HTML(<input name="ssid" id="ssid" maxlength="32" autocomplete="off" placeholder=")HTML";
  html += networkName.isEmpty() ? "Enter Wi-Fi name" : "Leave blank to keep current network";
  html += R"HTML("></div>)HTML";

  html += R"HTML(<label for="password">Wi-Fi password</label>)HTML"
          R"HTML(<input name="password" id="password" type="password" maxlength="63" autocomplete="new-password" )HTML"
          R"HTML(placeholder="Not shown or logged">)HTML"
          R"HTML(<label for="pet">Pet name</label><input name="pet" id="pet" maxlength="12" required value=")HTML";
  html += escapeHtml(state.pet.name);
  html += R"HTML("><small>Use English letters, numbers or spaces.</small>)HTML"
          R"HTML(<label for="place">Weather location</label><select name="place" id="place">)HTML";
  for (uint8_t i = 0; i < PLACE_COUNT; ++i) {
    html += "<option value=" + String(i) + (state.weather.place == i ? " selected>" : ">") + PLACES[i].name +
            "</option>";
  }
  html += R"HTML(</select><label for="brightness">Brightness</label><input type="range" name="brightness" id="brightness" min="3" max="255" value=)HTML";
  html += String(state.device.brightness);
  html += R"HTML(><input type="hidden" name="epoch" id="epoch"><button type="submit">Save and sync phone time</button></form>)HTML"
          R"HTML(<p><small>Weather: Open-Meteo.com. Time zone: Vietnam (UTC+7). No account required.</small></p>)HTML";

  html += R"HTML(<script>
const sel=document.getElementById('wifi_select');
const ssidInp=document.getElementById('ssid');
const manBox=document.getElementById('manual_box');
const passInp=document.getElementById('password');
const scanBtn=document.getElementById('scan_btn');
let pollTimer=null;
sel.addEventListener('change',function(){
  if(this.value==='__manual__'){
    ssidInp.value='';manBox.style.display='block';ssidInp.focus();
  }else if(this.value===''){
    ssidInp.value='';manBox.style.display='none';
  }else{
    ssidInp.value=this.value;manBox.style.display='none';
    const opt=this.options[this.selectedIndex];
    if(opt&&opt.getAttribute('data-secure')==='0'){
      passInp.placeholder='Open network (no password)';
    }else{
      passInp.placeholder='Not shown or logged';
    }
    passInp.focus();
  }
});
function updateOptions(networks){
  const cur=ssidInp.value;
  sel.innerHTML='';
  const defOpt=document.createElement('option');
  defOpt.value='';
  defOpt.textContent=sel.getAttribute('data-default');
  sel.appendChild(defOpt);
  let matched=false;
  networks.forEach(function(n){
    const opt=document.createElement('option');
    opt.value=n.ssid;
    opt.setAttribute('data-secure',n.secure?'1':'0');
    opt.textContent='📶 '+n.ssid+(n.secure?' 🔒':'')+' ('+n.rssi+' dBm)';
    if(n.ssid===cur){opt.selected=true;matched=true;}
    sel.appendChild(opt);
  });
  const manOpt=document.createElement('option');
  manOpt.value='__manual__';
  manOpt.textContent='✏️ Enter manually / Hidden network...';
  if(!matched&&cur){manOpt.selected=true;manBox.style.display='block';}
  sel.appendChild(manOpt);
}
function pollScan(){
  fetch('/scan')
    .then(function(r){return r.json();})
    .then(function(d){
      if(d.status==='done'&&d.networks){
        updateOptions(d.networks);
        scanBtn.disabled=false;
        scanBtn.textContent='Scan';
      }else{
        pollTimer=setTimeout(pollScan,1200);
      }
    })
    .catch(function(){scanBtn.disabled=false;scanBtn.textContent='Scan';});
}
function startScan(rescan){
  scanBtn.disabled=true;
  scanBtn.textContent='Scanning...';
  if(pollTimer)clearTimeout(pollTimer);
  fetch(rescan?'/scan?rescan=1':'/scan')
    .then(function(r){return r.json();})
    .then(function(d){
      if(d.status==='done'&&d.networks&&!rescan){
        updateOptions(d.networks);
        scanBtn.disabled=false;
        scanBtn.textContent='Scan';
      }else{
        pollTimer=setTimeout(pollScan,1200);
      }
    })
    .catch(function(){scanBtn.disabled=false;scanBtn.textContent='Scan';});
}
scanBtn.addEventListener('click',function(){startScan(true);});
if(document.getElementById('scan_opt')){startScan(false);}
document.querySelector('form').addEventListener('submit',function(){
  document.getElementById('epoch').value=Math.floor(Date.now()/1000);
});
</script></body></html>)HTML";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", html);
}

bool validPetName(const String &name) {
  if (name.length() < 1 || name.length() > 12) return false;
  for (size_t i = 0; i < name.length(); ++i) {
    if (!isalnum(static_cast<unsigned char>(name[i])) && name[i] != ' ') return false;
  }
  return true;
}

void saveSetup() {
  if (!setupClientAllowed() || !server.hasArg("token") || server.arg("token") != formToken) {
    server.send(403, "text/plain", "Invalid setup session.");
    return;
  }
  uint32_t newPlace = 0, newBrightness = 0, epoch = 0;
  const String name = server.arg("pet");
  const String ssid = server.arg("ssid");
  const String password = server.arg("password");
  const bool passwordValid = password.isEmpty() || (password.length() >= 8 && password.length() <= 63);
  if (!validPetName(name) || !parseUnsigned(server.arg("place"), 0, PLACE_COUNT - 1, newPlace) ||
      !parseUnsigned(server.arg("brightness"), 3, 255, newBrightness) ||
      !parseUnsigned(server.arg("epoch"), CLOCK_VALID_AFTER, 4102444799UL, epoch) ||
      ssid.length() > 32 || !passwordValid) {
    server.send(400, "text/plain",
                "Invalid settings. Check pet name, location, brightness, phone time and Wi-Fi password length.");
    return;
  }
  if (preferences.putString("pet", name) == 0 || preferences.putUChar("place", newPlace) == 0 ||
      preferences.putUChar("bright", newBrightness) == 0) {
    logError("settings", "NVS write failed");
    server.send(500, "text/plain", "Settings could not be saved.");
    return;
  }
  if (!ssid.isEmpty()) {
    if (preferences.putString("ssid", ssid) == 0 ||
        (!password.isEmpty() && preferences.putString("password", password) == 0)) {
      logError("wifi", "NVS write failed");
      server.send(500, "text/plain", "Network settings could not be saved.");
      return;
    }
    if (password.isEmpty()) preferences.remove("password");
    networkName = ssid; networkPassword = password;
    pendingConnectAt = millis() + 1000;
  }
  WiFi.scanDelete();
  state.pet.name = name;
  state.weather.place = newPlace;
  state.device.brightness = newBrightness;
  ++state.uiRevision;
  state.weather.valid = false;
  lastWeatherAttempt = 0;
  const timeval tv{static_cast<time_t>(epoch), 0};
  settimeofday(&tv, nullptr);
  server.send(200, "text/html",
              "<meta name=viewport content='width=device-width,initial-scale=1'><h1>Saved!</h1>"
              "<p>Phone time synchronized. Check the device for Wi-Fi connection status, then tap DONE. "
              "You can now reconnect your phone to your usual Wi-Fi.</p>");
  Serial.println("{\"event\":\"settings_saved\"}");
}

void openConfig() {
  if (state.setup.open) return;
  char suffix[9];
  snprintf(suffix, sizeof(suffix), "%04X", static_cast<unsigned>(ESP.getEfuseMac() & 0xFFFF));
  state.setup.apName = "DeskPet-" + String(suffix);
  state.setup.apPassword = preferences.getString("ap_password", "");
  if (state.setup.apPassword.isEmpty()) {
    constexpr char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    for (int i = 0; i < 12; ++i) state.setup.apPassword += alphabet[esp_random() % (sizeof(alphabet) - 1)];
  }
  formToken = String(esp_random(), HEX) + String(esp_random(), HEX);
  WiFi.mode(WIFI_AP_STA);
  if (!WiFi.softAP(state.setup.apName.c_str(), state.setup.apPassword.c_str())) {
    logError("setup", "Access point start failed");
    return;
  }
  state.setup.open = true;
  setupOffered = true;
  triggerWifiScan();
  server.begin();
  Serial.println("{\"event\":\"setup_open\",\"url\":\"http://192.168.4.1\"}");
}

void closeConfig() {
  server.stop();
  WiFi.scanDelete();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  state.setup.open = false;
  state.setup.apPassword = "";
  formToken = "";
  state.page = PAGE_PET;
  state.lastInteraction = millis();
}

// ---------------------------------------------------------------------------
// Device settings and alarm

void markDeviceSettingsChanged(uint32_t now) {
  deviceSettingsPending = true;
  deviceSettingsChangedAt = now;
  state.lastInteraction = now;
  ++state.uiRevision;
  setAudioGain(state.device.muted ? 0 : state.device.volume);
  setBacklight(state.device.brightness);
}

// Writes are debounced so dragging a slider does not wear the flash.
void saveDeviceSettings(uint32_t now) {
  if (!deviceSettingsPending || now - deviceSettingsChangedAt < 600) return;
  const DeviceSettings &device = state.device;
  const AlarmSchedule &alarm = state.alarm;
  const bool saved = preferences.putUChar("bright", device.brightness) != 0 &&
                     preferences.putUChar("volume", device.volume) != 0 &&
                     preferences.putBool("muted", device.muted) != 0 &&
                     preferences.putBool("talk", device.talk) != 0 &&
                     preferences.putUChar("alarm_hour", alarm.hour) != 0 &&
                     preferences.putUChar("alarm_min", alarm.minute) != 0 &&
                     preferences.putBool("alarm_on", alarm.enabled) != 0 &&
                     preferences.putUInt("alarm_day", alarm.lastFiredDay) != 0;
  if (!saved) {
    logError("settings", "Device or alarm settings could not be saved");
    deviceSettingsChangedAt = now;
    return;
  }
  deviceSettingsPending = false;
  Serial.println("{\"event\":\"device_settings_saved\"}");
}

void handleSettingsTap(int x, int y, uint32_t now) {
  DeviceSettings &device = state.device;
  if (BRIGHTNESS_SLIDER.row().contains(x, y)) {
    const int target = BRIGHTNESS_SLIDER.target(x, brightnessPercent(device.brightness));
    const uint8_t next = (constrain(target, 1, 100) * 255 + 50) / 100;
    if (next != device.brightness) { device.brightness = next; markDeviceSettingsChanged(now); }
  } else if (VOLUME_SLIDER.row().contains(x, y) && audioIsReady()) {
    const uint8_t next = constrain(VOLUME_SLIDER.target(x, device.volume), 0, 100);
    if (next != device.volume || device.muted) {
      device.volume = next; device.muted = false; markDeviceSettingsChanged(now);
    }
  } else if (hit(MUTE_BUTTON, x, y) && audioIsReady()) {
    device.muted = !device.muted;
    markDeviceSettingsChanged(now);
  } else if (hit(TEST_SOUND_BUTTON, x, y) && audioIsReady()) {
    requestSound();
  } else if (hit(TALK_BUTTON, x, y) && micReady()) {
    device.talk = !device.talk;
    markDeviceSettingsChanged(now);
  }
}

void printSettingsStatus() {
  const DeviceSettings &device = state.device;
  const AudioStats audio = audioStats();
  Serial.printf("{\"brightness\":%u,\"brightness_percent\":%d,\"backlight_pwm\":%lu,\"volume\":%u,"
                "\"muted\":%s,\"audio_ready\":%s,\"audio_completed\":%lu,\"audio_peak\":%lu,"
                "\"settings_pending\":%s,\"talk\":%s,\"mic_ready\":%s}\n",
                device.brightness, brightnessPercent(device.brightness),
                static_cast<unsigned long>(backlightLevel()), device.volume, device.muted ? "true" : "false",
                audioIsReady() ? "true" : "false", static_cast<unsigned long>(audio.completed),
                static_cast<unsigned long>(audio.peak), deviceSettingsPending ? "true" : "false",
                device.talk ? "true" : "false", micReady() ? "true" : "false");
}

void startAlarm(uint32_t now) {
  state.alarmRinging = true;
  alarmStartedAt = now;
  state.lastInteraction = now;
  state.pet.pickerOpen = false;
  startRinging();
  Serial.println("{\"event\":\"alarm_ringing\"}");
}

void stopAlarm(uint32_t now) {
  state.alarmRinging = false;
  state.lastInteraction = now;
  cancelSound();
  Serial.println("{\"event\":\"alarm_stopped\"}");
}

void updateAlarm(uint32_t now) {
  if (state.alarmRinging && !alarmWithinRingWindow(alarmStartedAt, now)) stopAlarm(now);
  if (now - lastAlarmCheck < 250 || !clockReady() || deviceSettingsPending) return;
  lastAlarmCheck = now;
  const time_t epoch = time(nullptr);
  tm local{};
  localtime_r(&epoch, &local);
  const uint32_t day = (local.tm_year + 1900) * 10000UL + (local.tm_mon + 1) * 100UL + local.tm_mday;
  if (!alarmDue(state.alarm, day, local.tm_hour, local.tm_min)) return;
  state.alarm.lastFiredDay = day;
  if (preferences.putUInt("alarm_day", day) == 0) {
    logError("alarm", "Unable to persist fired date; a restart may repeat this alarm");
  }
  startAlarm(now);
}

// The microphone is only open while talk mode is on and someone can see the
// companion; it closes for the alarm, the setup portal, the picker and other pages.
void updateTalk(uint32_t now) {
  const DeviceSettings &device = state.device;
  const bool audible = !device.muted && device.volume > 0;
  const bool posed = petAssetsReady() && petAsset(state.pet.mascot).kind == PET_ASSET_POSES;
  setTalkActive(device.talk && audible && posed && petPageInteractive());
  // A conversation counts as interaction: keep the screen bright and the pet awake.
  const TalkPhase phase = talkPhase();
  if (phase == TalkPhase::Hearing || phase == TalkPhase::Talking) state.lastInteraction = now;
}

void printAlarmStatus() {
  const AlarmSchedule &alarm = state.alarm;
  Serial.printf("{\"alarm_hour\":%u,\"alarm_minute\":%u,\"enabled\":%s,\"ringing\":%s,\"last_fired_day\":%lu,"
                "\"clock_synced\":%s,\"local_time\":\"%s\"}\n",
                alarm.hour, alarm.minute, alarm.enabled ? "true" : "false", state.alarmRinging ? "true" : "false",
                static_cast<unsigned long>(alarm.lastFiredDay), clockReady() ? "true" : "false",
                dateText("%Y-%m-%d %H:%M:%S").c_str());
}

// ---------------------------------------------------------------------------
// Touch, button and pet interaction

void handlePetHold(int x, int y, uint32_t now) {
  if (state.alarmRinging || state.setup.open || state.page != PAGE_PET || !PET_AREA.contains(x, y)) return;
  if (!petAssetsReady()) return;
  state.pet.pickerPage = state.pet.mascot / PICKER_SLOTS;
  state.pet.pickerOpen = true;
  state.pet.reacting = false;
  state.lastInteraction = now;
}

void selectCompanion(uint8_t choice, uint32_t now) {
  PetState &pet = state.pet;
  if (choice != pet.mascot && preferences.putUChar("mascot", choice) == 0) {
    logError("settings", "Companion selection could not be saved");
    return;
  }
  pet.mascot = choice;
  pet.pickerOpen = false;
  pet.reacting = petAsset(choice).kind == PET_ASSET_POSES;
  pet.expression = PetExpression::Joy;
  pet.reactedAt = now;
  pet.animationStartedAt = now;
  pet.direction = POSE_FRAME_FRONT;
  rapidPetTaps = 0;
  Serial.printf("{\"event\":\"mascot_selected\",\"mascot\":%u}\n", pet.mascot);
}

void handlePickerTap(int x, int y, uint32_t now) {
  PetState &pet = state.pet;
  const uint8_t pages = (petAssetCount() + PICKER_SLOTS - 1) / PICKER_SLOTS;
  if (PICKER_TITLE_BAR.contains(x, y)) {
    if (PICKER_PREV.contains(x, y)) pet.pickerPage = (pet.pickerPage + pages - 1) % pages;
    else if (PICKER_NEXT.contains(x, y)) pet.pickerPage = (pet.pickerPage + 1) % pages;
    return;
  }
  if (!PICKER_AREA.contains(x, y)) { pet.pickerOpen = false; return; }
  for (uint8_t slot = 0; slot < PICKER_SLOTS; ++slot) {
    const uint8_t choice = pet.pickerPage * PICKER_SLOTS + slot;
    if (choice < petAssetCount() && hit(pickerCard(slot), x, y)) { selectCompanion(choice, now); return; }
  }
}

void handlePetTap(uint32_t now) {
  PetState &pet = state.pet;
  if (petAsset(pet.mascot).kind == PET_ASSET_ANIMATION) {
    pet.animationStartedAt = now;
    pet.reacting = false;
    return;
  }
  constexpr PetExpression reactions[] = {PetExpression::Love, PetExpression::Sparkle, PetExpression::Surprise,
                                         PetExpression::Wink, PetExpression::Bashful, PetExpression::Joy};
  constexpr uint8_t reactionCount = sizeof(reactions) / sizeof(reactions[0]);
  // Four quick taps in a row make the pet dizzy.
  rapidPetTaps = now - lastPetTapAt < 1600 ? rapidPetTaps + 1 : 1;
  lastPetTapAt = now;
  if (rapidPetTaps >= 4) {
    pet.expression = PetExpression::Dizzy;
    rapidPetTaps = 0;
  } else {
    pet.expression = reactions[petReactionIndex];
    petReactionIndex = (petReactionIndex + 1) % reactionCount;
  }
  pet.reacting = true;
  pet.reactedAt = now;
  ++pet.tapCount;
  savePending = true;
}

void handleClockTap(int x, int y, uint32_t now) {
  AlarmSchedule &alarm = state.alarm;
  if (hit(ALARM_HOUR_DOWN, x, y)) alarm.hour = (alarm.hour + 23) % 24;
  else if (hit(ALARM_HOUR_UP, x, y)) alarm.hour = (alarm.hour + 1) % 24;
  else if (hit(ALARM_MINUTE_DOWN, x, y)) alarm.minute = (alarm.minute + 59) % 60;
  else if (hit(ALARM_MINUTE_UP, x, y)) alarm.minute = (alarm.minute + 1) % 60;
  else if (hit(ALARM_TOGGLE_BUTTON, x, y)) { alarm.enabled = !alarm.enabled; markDeviceSettingsChanged(now); return; }
  else if (hit(TEST_BELL_BUTTON, x, y)) { startAlarm(now); return; }
  else return;
  // A new time may be later today, so let it fire again.
  alarm.lastFiredDay = 0;
  markDeviceSettingsChanged(now);
}

void handleTap(int x, int y, uint32_t now) {
  state.lastInteraction = now;
  if (state.alarmRinging) { if (hit(STOP_ALARM_BUTTON, x, y)) stopAlarm(now); return; }
  if (state.setup.open) { if (hit(SETUP_DONE_BUTTON, x, y)) closeConfig(); return; }
  if (NAV_BAR.contains(x, y)) {
    state.page = min(PAGE_COUNT - 1, x / NAV_TAB_WIDTH);
    state.pet.pickerOpen = false;
    return;
  }
  switch (state.page) {
    case PAGE_PET:
      if (!petAssetsReady()) return;
      if (state.pet.pickerOpen) handlePickerTap(x, y, now);
      else handlePetTap(now);
      break;
    case PAGE_CLOCK: handleClockTap(x, y, now); break;
    case PAGE_WEATHER: requestWeather(now); break;
    case PAGE_SETTINGS: handleSettingsTap(x, y, now); break;
  }
}

// Applies one press, move or release from the touch task.
void handleTouchSample(const TouchSample &sample) {
  TouchState &touch = state.touch;
  const uint32_t at = sample.at;
  perf.maxInputLatencyMs = max(perf.maxInputLatencyMs, millis() - at);
  state.lastInteraction = at;
  inputPending = true;
  if (sample.pressed) {
    touch.x = sample.x;
    touch.y = sample.y;
    if (petPageInteractive()) state.pet.direction = petDirectionAt(touch.x, touch.y);
    if (!touch.down) {
      touch.down = true;
      touch.startX = touch.x; touch.startY = touch.y; touch.since = at;
      touch.longPressHandled = false;
      touch.draggingControl = !state.alarmRinging && !state.setup.open && state.page == PAGE_SETTINGS &&
                              (BRIGHTNESS_SLIDER.dragZone().contains(touch.x, touch.y) ||
                               VOLUME_SLIDER.dragZone().contains(touch.x, touch.y));
    }
    if (touch.draggingControl) {
      const Slider &slider =
          BRIGHTNESS_SLIDER.row().contains(touch.startX, touch.startY) ? BRIGHTNESS_SLIDER : VOLUME_SLIDER;
      handleSettingsTap(slider.clampToTrack(touch.x), touch.startY, at);
    }
    return;
  }
  if (!touch.down) return;
  touch.down = false;
  if (touch.longPressHandled) { touch.longPressHandled = false; return; }
  if (touch.draggingControl) { touch.draggingControl = false; return; }
  const int direction = swipeDirection(touch.startX, touch.startY, touch.x, touch.y);
  if (!state.alarmRinging && !state.setup.open && direction != 0) {
    PetState &pet = state.pet;
    if (pet.pickerOpen) {
      const int pages = (petAssetCount() + PICKER_SLOTS - 1) / PICKER_SLOTS;
      pet.pickerPage = (pet.pickerPage + direction + pages) % pages;
    } else {
      state.page = (state.page + direction + PAGE_COUNT) % PAGE_COUNT;
    }
  } else if (abs(touch.x - touch.startX) < TAP_SLOP && abs(touch.y - touch.startY) < TAP_SLOP) {
    handleTap(touch.x, touch.y, at);
  }
}

// A finger held still produces no new samples, so the long press is timed here.
void checkLongPress(uint32_t now) {
  TouchState &touch = state.touch;
  if (!touch.down || touch.longPressHandled || !petPageInteractive()) return;
  const bool still = abs(touch.x - touch.startX) < LONG_PRESS_SLOP && abs(touch.y - touch.startY) < LONG_PRESS_SLOP;
  if (still && PET_AREA.contains(touch.startX, touch.startY) && now - touch.since >= LONG_PRESS) {
    handlePetHold(touch.x, touch.y, now);
    touch.longPressHandled = true;
    inputPending = true;
  }
}

void pollTouch(uint32_t now) {
  TouchSample sample;
  while (nextTouchSample(sample)) handleTouchSample(sample);
  checkLongPress(now);
  state.touch.failures = touchFailures();
}

// BOOT button: short press cycles pages, long press toggles the setup portal,
// any press silences a ringing alarm.
void pollButton(uint32_t now) {
  const bool pressed = digitalRead(PIN_BOOT_BUTTON) == LOW;
  if (pressed && !buttonWasDown) {
    buttonAt = now;
    buttonHeld = false;
    if (state.alarmRinging) { stopAlarm(now); buttonHeld = true; }
  }
  if (pressed && now - buttonAt >= 1800 && !buttonHeld) {
    buttonHeld = true;
    if (state.setup.open) closeConfig(); else openConfig();
  }
  if (!pressed && buttonWasDown && !buttonHeld && now - buttonAt > 30) {
    if (state.setup.open) {
      closeConfig();
    } else {
      state.page = (state.page + 1) % PAGE_COUNT;
      state.pet.pickerOpen = false;
    }
    state.lastInteraction = now;
  }
  if (pressed != buttonWasDown) inputPending = true;
  buttonWasDown = pressed;
}

// ---------------------------------------------------------------------------
// Serial console (used by the QA scripts)

void printStatus() {
  Serial.printf("{\"firmware\":\"%s\",\"board\":\"ES3C28P\",\"uptime_ms\":%lu,\"heap\":%u,\"psram\":%u,"
                "\"page\":%u,\"wifi\":%s,\"clock_synced\":%s,\"weather_valid\":%s,\"weather_error\":%u,"
                "\"alarm_ringing\":%s,\"touch_errors\":%u}\n",
                VERSION, static_cast<unsigned long>(millis()), ESP.getFreeHeap(), ESP.getPsramSize(), state.page,
                WiFi.status() == WL_CONNECTED ? "true" : "false", clockReady() ? "true" : "false",
                state.weather.valid ? "true" : "false", static_cast<unsigned>(state.weather.current.error),
                state.alarmRinging ? "true" : "false", state.touch.failures);
}

void printPetStatus() {
  const PetState &pet = state.pet;
  Serial.printf("{\"mascot\":%u,\"expression\":%u,\"reacting\":%s,\"picker_open\":%s}\n", pet.mascot,
                static_cast<uint8_t>(pet.expression), pet.reacting ? "true" : "false",
                pet.pickerOpen ? "true" : "false");
}

void printAssetStatus() {
  Serial.printf("{\"assets_ready\":%s,\"asset_count\":%u,\"asset_bytes\":%lu,\"mascot\":%u,\"frame\":%lu,"
                "\"picker_page\":%u}\n",
                petAssetsReady() ? "true" : "false", petAssetCount(), static_cast<unsigned long>(petAssetBytes()),
                state.pet.mascot, static_cast<unsigned long>(petVisualFrame(state, millis())), state.pet.pickerPage);
}

void printSelfTest() {
  Serial.println(modelSelfTest() ? "SELFTEST PASS: alarm schedule, midnight, daily deduplication, timeout, swipe"
                                 : "SELFTEST FAIL");
}

// Parses "<command> x y" and rejects trailing input or off-screen points.
bool parsePoint(const char *format, int &x, int &y) {
  char extra = 0;
  return sscanf(serialLine.c_str(), format, &x, &y, &extra) == 2 && x >= 0 && x < SCREEN_WIDTH && y >= 0 &&
         y < SCREEN_HEIGHT;
}

void runSerialCommand() {
  int x = 0, y = 0;
  if (serialLine == "status") printStatus();
  else if (serialLine == "ui-stats") printUiStats();
  else if (serialLine == "perf") {
    Serial.printf("{\"perf\":true,\"loops\":%lu,\"renders\":%lu,\"avg_render_us\":%lu,\"max_render_us\":%lu,"
                  "\"max_loop_us\":%lu,\"max_pump_us\":%lu,\"max_save_us\":%lu,\"max_input_latency_ms\":%lu,\"free_heap\":%u}\n",
                  static_cast<unsigned long>(perf.loops), static_cast<unsigned long>(perf.renders),
                  static_cast<unsigned long>(perf.renders ? perf.totalRenderUs / perf.renders : 0),
                  static_cast<unsigned long>(perf.maxRenderUs), static_cast<unsigned long>(perf.maxLoopUs),
                  static_cast<unsigned long>(perf.maxPumpUs),
                  static_cast<unsigned long>(perf.maxSaveUs), static_cast<unsigned long>(perf.maxInputLatencyMs),
                  ESP.getFreeHeap());
    perf = Perf{};
  }
  else if (serialLine == "settings-status") printSettingsStatus();
  else if (serialLine == "asset-selftest") testPetAssets();
  else if (serialLine == "alarm-status") printAlarmStatus();
  else if (serialLine == "pet-status") printPetStatus();
  else if (serialLine == "asset-status") printAssetStatus();
  else if (serialLine == "sound-test") requestSound();
  else if (serialLine == "talk-status") printTalkStatus();
  else if (serialLine == "mic-test") runMicTest();
  else if (serialLine.startsWith("talk-tune ")) {
    unsigned start = 0, end = 0;
    char extra = 0;
    if (sscanf(serialLine.c_str(), "talk-tune %u %u %c", &start, &end, &extra) == 2 && end > 0 && end <= start &&
        start <= 5000) {
      tuneTalk(start, end);
      printTalkStatus();
    } else {
      logError("input", "Use talk-tune <start> <end> with 0 < end <= start <= 5000");
    }
  }
  else if (serialLine == "reboot") ESP.restart();
  else if (serialLine.startsWith("hold ")) {
    if (parsePoint("hold %d %d %c", x, y)) handlePetHold(x, y, millis());
    else logError("input", "Invalid hold coordinates");
  } else if (serialLine.startsWith("tap ")) {
    if (parsePoint("tap %d %d %c", x, y)) handleTap(x, y, millis());
    else logError("input", "Invalid tap coordinates");
  } else if (serialLine == "snapshot" && !state.setup.open) {
    writeSnapshot();
  } else if (serialLine.length() == 6 && serialLine.startsWith("page ") && serialLine[5] >= '0' &&
             serialLine[5] < '0' + PAGE_COUNT) {
    if (state.setup.open) closeConfig();
    state.page = serialLine[5] - '0';
    state.pet.pickerOpen = false;
    state.lastInteraction = millis();
  } else if (serialLine == "selftest") {
    printSelfTest();
  } else if (serialLine == "setup") {
    openConfig();
  } else if (serialLine == "pet") {
    if (state.setup.open) closeConfig();
    state.page = PAGE_PET;
    state.pet.pickerOpen = false;
  } else if (serialLine.startsWith("ap-password ")) {
    const String password = serialLine.substring(12);
    if (password.length() < 8 || password.length() > 63) {
      logError("setup", "AP password must contain 8 to 63 characters");
    } else if (preferences.putString("ap_password", password) == 0) {
      logError("setup", "AP password NVS write failed");
    } else {
      Serial.println("{\"event\":\"ap_password_saved\"}");
      if (state.setup.open) { closeConfig(); openConfig(); }
    }
  } else if (!serialLine.isEmpty()) {
    Serial.println("Commands: status, selftest, setup, pet, talk-status, mic-test, reboot, ap-password <value>");
  }
}

void pollSerial() {
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == '\n') {
      serialLine.trim();
      runSerialCommand();
      inputPending = true;
      serialLine = "";
    } else if (serialLine.length() < 80 && c >= 32 && c <= 126) {
      serialLine += c;
    }
  }
}

// ---------------------------------------------------------------------------
// Boot

void loadSettings() {
  PetState &pet = state.pet;
  DeviceSettings &device = state.device;
  AlarmSchedule &alarm = state.alarm;
  pet.name = preferences.getString("pet", "Mochi");
  pet.mascot = preferences.getUChar("mascot", 0);
  state.weather.place = preferences.getUChar("place", 0);
  if (state.weather.place >= PLACE_COUNT) state.weather.place = 0;
  device.brightness = max(static_cast<uint8_t>(3), preferences.getUChar("bright", 170));
  device.volume = min(static_cast<uint8_t>(100), preferences.getUChar("volume", 40));
  device.muted = preferences.getBool("muted", false);
  device.talk = preferences.getBool("talk", false);
  alarm.hour = preferences.getUChar("alarm_hour", 7);
  alarm.minute = preferences.getUChar("alarm_min", 0);
  alarm.enabled = preferences.getBool("alarm_on", false);
  alarm.lastFiredDay = preferences.getUInt("alarm_day", 0);
  if (alarm.hour > 23 || alarm.minute > 59) {
    logError("settings", "Invalid alarm schedule; alarm disabled");
    alarm = AlarmSchedule{};
  }
  pet.tapCount = preferences.getUInt("pets", 0);
  networkName = preferences.getString("ssid", "");
  networkPassword = preferences.getString("password", "");
}
}  // namespace

void appSetup() {
  Serial.begin(115200);
  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
  pinMode(PIN_SPEAKER_MUTE, OUTPUT);
  digitalWrite(PIN_SPEAKER_MUTE, HIGH);
  rgbLedWrite(PIN_STATUS_LED, 0, 0, 0);
  const bool settingsOpen = preferences.begin("deskpet", false);
  if (settingsOpen) loadSettings();
  // The backlight comes up at the stored level, so a dim room is not flashed at boot.
  if (!initDisplay(state.device.brightness)) { fatalError = true; return; }
  if (!settingsOpen) {
    logError("settings", "NVS open failed");
    showBootProgress("Settings storage failed", 0);
    fatalError = true;
    return;
  }
  showBootProgress("Loading companions", 15);
  initPetAssets();
  // The stored companion may have been removed from a newer asset pack.
  if (petAssetsReady() && state.pet.mascot >= petAssetCount()) {
    state.pet.mascot = 0;
    if (preferences.putUChar("mascot", 0) == 0) logError("settings", "Replacement companion could not be saved");
    Serial.println("{\"event\":\"removed_companion_replaced\",\"mascot\":0}");
  }
  state.pet.animationStartedAt = millis();
  setAudioGain(state.device.muted ? 0 : state.device.volume);
  showBootProgress("Starting touch and sound", 45);
  // Pulse the touch controller reset before starting the shared I2C bus.
  pinMode(PIN_TOUCH_RESET, OUTPUT);
  digitalWrite(PIN_TOUCH_RESET, LOW); delay(10);
  digitalWrite(PIN_TOUCH_RESET, HIGH); delay(180);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  Wire.setTimeOut(20);
  if (!initAudio()) logError("audio", "Speaker disabled because initialization failed");
  else initTalk();
  // Touch sampling starts after the codec setup, the only other user of the I2C bus.
  if (!startTouchInput()) { logError("touch", "Touch task allocation failed"); fatalError = true; return; }
  showBootProgress("Connecting", 75);
  if (!startWeatherWorker()) { logError("weather", "Worker allocation failed"); fatalError = true; return; }
  server.on("/", HTTP_GET, serveSetup);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/save", HTTP_POST, saveSetup);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  setenv("TZ", TIMEZONE, 1);
  tzset();
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  if (networkName.isEmpty()) WiFi.begin(); else WiFi.begin(networkName.c_str(), networkPassword.c_str());
  configTzTime(TIMEZONE, "pool.ntp.org", "time.cloudflare.com", "time.google.com");
  showBootProgress("Ready", 100);
  // If the UI loop is ever blocked for 5 s, reboot (and keep a core dump) instead of freezing.
  const esp_task_wdt_config_t watchdog{5000, 0, true};
  if (esp_task_wdt_reconfigure(&watchdog) != ESP_OK) logError("system", "Watchdog configuration failed");
  enableLoopWDT();
  Serial.printf("{\"event\":\"boot\",\"setup_ms\":%lu,\"reset_reason\":%d}\n",
                static_cast<unsigned long>(millis()), static_cast<int>(esp_reset_reason()));
  state.lastInteraction = millis();
  lastReconnect = millis();
  printSelfTest();
  printStatus();
}

void appLoop() {
  if (fatalError) { delay(500); return; }
  const uint32_t loopStarted = micros();
  const uint32_t inputAt = millis();
  pollTouch(inputAt);
  pollButton(inputAt);
  pollSerial();
  const uint32_t now = millis();
  if (state.setup.open) server.handleClient();
  if (pendingConnectAt != 0 && static_cast<int32_t>(now - pendingConnectAt) >= 0) {
    pendingConnectAt = 0;
    WiFi.disconnect(false, false);
    WiFi.begin(networkName.c_str(), networkPassword.c_str());
    lastReconnect = now;
  }

  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !everConnected) { everConnected = true; Serial.println("{\"event\":\"wifi_connected\"}"); }
  static bool wasConnected = false;
  static uint32_t connectedAt = 0;
  if (connected && !wasConnected) connectedAt = now;  // also after every reconnect
  wasConnected = connected;
  const bool networkSettled = connected && now - connectedAt >= NETWORK_SETTLE;
  // Offer the setup portal once if the device has never reached Wi-Fi and nobody is using it.
  if (!connected && !everConnected && !setupOffered && now > 18000 && !state.setup.open &&
      now - state.lastInteraction > 15000) {
    openConfig();
  }
  if (!connected && now - lastReconnect > 60000) { WiFi.reconnect(); lastReconnect = now; }

  WeatherState &weather = state.weather;
  const uint32_t weatherInterval = weather.valid ? WEATHER_INTERVAL : WEATHER_RETRY_INTERVAL;
  if (networkSettled && clockReady() && !weather.busy &&
      (lastWeatherAttempt == 0 || now - lastWeatherAttempt >= weatherInterval)) {
    requestWeather(now);
  }
  WeatherResult result;
  if (takeWeatherResult(result)) {
    weather.busy = false;
    if (result.place != weather.place) {
      lastWeatherAttempt = 0;  // the place changed mid-request; fetch the new one right away
    } else if (result.error == WeatherError::None) {
      weather.current = result;
      weather.valid = true;
      Serial.println("{\"event\":\"weather_updated\"}");
    } else {
      weather.current.error = result.error;
      Serial.printf("{\"level\":\"error\",\"component\":\"weather\","
                    "\"reason\":\"Request failed after bounded retries\",\"error_code\":%u}\n",
                    static_cast<unsigned>(result.error));
    }
  }

  if (state.pet.reacting && now - state.pet.reactedAt > REACTION_DURATION) state.pet.reacting = false;
  updateAlarm(now);
  updateTalk(now);
  static bool alarmLed = false;
  const bool nextLed = state.alarmRinging && ((now / 500) % 2 == 0);
  if (nextLed != alarmLed) {
    rgbLedWrite(PIN_STATUS_LED, nextLed ? 32 : 0, 0, nextLed ? 10 : 0);
    alarmLed = nextLed;
  }
  if (savePending && now - lastSave > 15000) {
    if (preferences.putUInt("pets", state.pet.tapCount) == 0) logError("settings", "Pet counter write failed");
    else savePending = false;
    lastSave = now;
  }
  const uint32_t saveStarted = micros();
  saveDeviceSettings(now);
  perf.maxSaveUs = max(perf.maxSaveUs, micros() - saveStarted);
  const uint8_t brightness = state.device.brightness;
  const bool idle = !state.alarmRinging && !state.setup.open && state.page != PAGE_SETTINGS &&
                    now - state.lastInteraction > DIM_AFTER;
  const uint8_t backlight = idle ? max(3, brightness / 4) : brightness;
  static int lastBacklight = -1;
  if (backlight != lastBacklight) { setBacklight(backlight); lastBacklight = backlight; }
  // Input is answered on the very next loop; animation runs on the regular frame tick.
  if (inputPending || now - lastFrame >= FRAME_INTERVAL) {
    inputPending = false;
    const uint32_t renderStarted = micros();
    renderFrame(state, now);
    const uint32_t renderUs = micros() - renderStarted;
    perf.maxRenderUs = max(perf.maxRenderUs, renderUs);
    perf.totalRenderUs += renderUs;
    ++perf.renders;
    lastFrame = now;
    const uint32_t beforeRender = renderStarted - loopStarted;
    if (renderUs > SLOW_FRAME_US || beforeRender > SLOW_FRAME_US) {
      const RenderTiming timing = lastRenderTiming();
      Serial.printf("{\"event\":\"slow_frame\",\"before_render_us\":%lu,\"render_us\":%lu,\"drawn\":%s,"
                    "\"draw_us\":%lu,\"decode_us\":%lu,"
                    "\"page\":%u,\"picker\":%s,\"ringing\":%s}\n",
                    static_cast<unsigned long>(beforeRender), static_cast<unsigned long>(renderUs),
                    timing.drawn ? "true" : "false", static_cast<unsigned long>(timing.drawUs),
                    static_cast<unsigned long>(timing.decodeUs),
                    state.page, state.pet.pickerOpen ? "true" : "false", state.alarmRinging ? "true" : "false");
    }
  }
  const uint32_t pumpStarted = micros();
  pumpDisplay(DISPLAY_SLICE_US);
  perf.maxPumpUs = max(perf.maxPumpUs, micros() - pumpStarted);
  ++perf.loops;
  perf.maxLoopUs = max(perf.maxLoopUs, micros() - loopStarted);
  delay(2);
}
