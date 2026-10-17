#include "src/features/setup/setup_portal.h"

#include <ArduinoJson.h>
#include <WebServer.h>
#include <WiFi.h>
#include <algorithm>
#include <sys/time.h>
#include <time.h>
#include <vector>
#include "src/core/system/config.h"
#include "src/core/system/log.h"
#include "src/core/system/timekeeping.h"

namespace {
WebServer server(80);
AppState *appState = nullptr;
const NetworkCredentials *currentNetwork = nullptr;
SetupSavedHandler savedHandler = nullptr;
String formToken;

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
  return appState->setup.open && address[0] == 192 && address[1] == 168 && address[2] == 4;
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

  const String defaultLabel = currentNetwork->ssid.isEmpty()
                                  ? "-- Select a Wi-Fi network --"
                                  : "-- Keep current network (" + escapeHtml(currentNetwork->ssid) + ") --";
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
  html += currentNetwork->ssid.isEmpty() ? "Enter Wi-Fi name" : "Leave blank to keep current network";
  html += R"HTML("></div>)HTML";

  html += R"HTML(<label for="password">Wi-Fi password</label>)HTML"
          R"HTML(<input name="password" id="password" type="password" maxlength="63" autocomplete="new-password" )HTML"
          R"HTML(placeholder="Not shown or logged">)HTML"
          R"HTML(<label for="pet">Pet name</label><input name="pet" id="pet" maxlength="12" required value=")HTML";
  html += escapeHtml(appState->pet.name);
  html += R"HTML("><small>Use English letters, numbers or spaces.</small>)HTML"
          R"HTML(<label for="place">Weather location</label><select name="place" id="place">)HTML";
  for (uint8_t i = 0; i < PLACE_COUNT; ++i) {
    html += "<option value=" + String(i) + (appState->weather.place == i ? " selected>" : ">") + PLACES[i].name +
            "</option>";
  }
  html += R"HTML(</select><label for="brightness">Brightness</label><input type="range" name="brightness" id="brightness" min="3" max="255" value=)HTML";
  html += String(appState->device.brightness);
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
  if (!saveSetupForm(name, newPlace, newBrightness)) {
    logError("settings", "NVS write failed");
    server.send(500, "text/plain", "Settings could not be saved.");
    return;
  }
  if (!ssid.isEmpty() && !saveNetwork({ssid, password})) {
    logError("wifi", "NVS write failed");
    server.send(500, "text/plain", "Network settings could not be saved.");
    return;
  }
  WiFi.scanDelete();
  SetupForm form;
  form.petName = name;
  form.place = newPlace;
  form.brightness = newBrightness;
  form.ssid = ssid;
  form.password = password;
  savedHandler(form);
  const timeval tv{static_cast<time_t>(epoch), 0};
  settimeofday(&tv, nullptr);
  server.send(200, "text/html",
              "<meta name=viewport content='width=device-width,initial-scale=1'><h1>Saved!</h1>"
              "<p>Phone time synchronized. Check the device for Wi-Fi connection status, then tap DONE. "
              "You can now reconnect your phone to your usual Wi-Fi.</p>");
  Serial.println("{\"event\":\"settings_saved\"}");
}
}  // namespace

void initSetupPortal(AppState &state, const NetworkCredentials &network, SetupSavedHandler onSaved) {
  appState = &state;
  currentNetwork = &network;
  savedHandler = onSaved;
  server.on("/", HTTP_GET, serveSetup);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/save", HTTP_POST, saveSetup);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
}

bool openSetupPortal() {
  SetupPortalState &setup = appState->setup;
  if (setup.open) return false;
  char suffix[9];
  snprintf(suffix, sizeof(suffix), "%04X", static_cast<unsigned>(ESP.getEfuseMac() & 0xFFFF));
  setup.apName = "DeskPet-" + String(suffix);
  setup.apPassword = loadApPassword();
  if (setup.apPassword.isEmpty()) {
    constexpr char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    for (int i = 0; i < 12; ++i) setup.apPassword += alphabet[esp_random() % (sizeof(alphabet) - 1)];
  }
  formToken = String(esp_random(), HEX) + String(esp_random(), HEX);
  WiFi.mode(WIFI_AP_STA);
  if (!WiFi.softAP(setup.apName.c_str(), setup.apPassword.c_str())) {
    logError("setup", "Access point start failed");
    return false;
  }
  setup.open = true;
  triggerWifiScan();
  server.begin();
  Serial.println("{\"event\":\"setup_open\",\"url\":\"http://192.168.4.1\"}");
  return true;
}

void closeSetupPortal() {
  server.stop();
  WiFi.scanDelete();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  appState->setup.open = false;
  appState->setup.apPassword = "";
  formToken = "";
  appState->page = PAGE_PET;
  appState->lastInteraction = millis();
}

void pollSetupPortal() { server.handleClient(); }
