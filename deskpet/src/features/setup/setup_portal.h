#pragma once
#include <Arduino.h>
#include "src/app/settings_store.h"
#include "src/app/state.h"

// Wi-Fi setup portal: a SoftAP named DeskPet-XXXX and a web form at
// http://192.168.4.1 for the Wi-Fi network, pet name, weather place and
// brightness. Only clients on the SoftAP subnet are served.

// What the form saved. The portal has already written it to flash.
struct SetupForm {
  String petName;
  uint8_t place = 0;
  uint8_t brightness = 0;
  String ssid;  // empty: keep the current network
  String password;
};
using SetupSavedHandler = void (*)(const SetupForm &form);

// Registers the web routes. `network` is the saved network the form offers to
// keep; `onSaved` applies a saved form to the running device.
void initSetupPortal(AppState &state, const NetworkCredentials &network, SetupSavedHandler onSaved);
// Starts the access point and web server; false when it was already open or the AP failed.
bool openSetupPortal();
void closeSetupPortal();
// Serves pending HTTP requests; call every loop while the portal is open.
void pollSetupPortal();
