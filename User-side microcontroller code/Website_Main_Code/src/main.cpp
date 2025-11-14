// --- Main Rickshaw System Libraries ---
#include <Arduino.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <Adafruit_NeoPixel.h>

// --- Authentication System Libraries ---
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// --- WiFi & Firebase Config ---
#define WIFI_SSID "Das_House_lite"
#define WIFI_PASSWORD "2444666668888888"
#define API_KEY "AIzaSyD0hOasCnqllLfac7NGqm7cSQlvYnaU-ro"
#define FIREBASE_PROJECT_ID "rickshaw-puller-71731"
#define DATABASE_URL "https://rickshaw-puller-71731-default-rtdb.asia-southeast1.firebaseio.app"
#define APP_ID "default-app-id"
#define DRIVER_USER_ID "pkO9SyuhD4asdw2PoeWxd9Kqr3y1"

// --- Main Rickshaw Hardware Pins ---
#define RIDE_BUTTON_PIN 4 // --- RE-ENABLED: Physical button to send ride request ---
#define NEOPIXEL_PIN 19   // Addressable LED for ride status
#define NEOPIXEL_COUNT 1
#define BUZZER_PIN 26 // Buzzer for audio feedback

// --- Authentication System Hardware Pins ---
#define OLED_RESET -1
#define OLED_SCREEN_WIDTH 128
#define OLED_SCREEN_HEIGHT 64
#define OLED_SDA_PIN 21
#define OLED_SCL_PIN 22
#define AUTH_TRIGGER_PIN 13 // Ultrasonic Trigger
#define AUTH_ECHO_PIN 12    // Ultrasonic Echo
#define AUTH_LED_PIN 2      // Simple LED for auth status
#define AUTH_LDR_PIN 34     // LDR for verification

// --- Authentication System Logic ---
#define DISTANCE_THRESHOLD_M 0.2
#define TIME_THRESHOLD_MS 3000
#define PRIVILEGE_WAIT_MS 10000

// --- Firebase Objects ---
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// --- Rickshaw Hardware Objects ---
Adafruit_NeoPixel strip(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
uint32_t COLOR_RED = strip.Color(255, 0, 0);
uint32_t COLOR_GREEN = strip.Color(0, 255, 0);
uint32_t COLOR_YELLOW = strip.Color(80, 255, 0);
uint32_t COLOR_OFF = strip.Color(0, 0, 0);

// --- Auth Hardware Objects ---
Adafruit_SSD1306 display(OLED_SCREEN_WIDTH, OLED_SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- Main State Machine ---
enum SystemState
{
    STATE_WAITING_FOR_AUTH,
    STATE_WAITING_FOR_DRIVER,
    STATE_RIDE_COMPLETE
};
SystemState currentState = STATE_WAITING_FOR_AUTH;

// --- Rickshaw State ---
String currentRideStatus = "idle";
unsigned long lastFirebaseCheck = 0;
const long firebaseCheckInterval = 5000;
String documentPath = "";

// --- NEW: Ride Button Debounce ---
int rideButtonState;
int lastRideButtonState = HIGH;
unsigned long lastRideButtonDebounce = 0;
unsigned long rideButtonDebounceDelay = 50;

// --- Auth System State ---
bool isPersonInRange = false;
bool isTriggerActive = false;
unsigned long rangeEntryTime = 0;
bool isWaitingForPrivilege = false;
bool privilegeGranted = false; // This is now the main "gate"
bool privilegeResultShown = false;
unsigned long privilegeWaitStartTime = 0;
float currentDistanceM = 0.0;

// --- Non-Blocking Timers ---
unsigned long lastOledUpdate = 0;
unsigned long lastResultShowTime = 0;
unsigned long lastAuthDebugPrint = 0;

// ---
// --- FUNCTION PROTOTYPES ---
// ---
void checkAuthSystem();
void updateOLED_Auth();
void resetAuthSystem();
void checkFirebase();
float getDistanceInMeters();
void checkPresenceLogic(bool inRange);
void checkPrivilegeLogic();
void checkRideRequestButton(); // --- NEW ---
// --- END OF PROTOTYPES ---

// --- RICKSHAW SYSTEM FUNCTIONS ---

void playAcceptTone()
{
    tone(BUZZER_PIN, 10, 150);
}
void playRejectTone()
{
    tone(BUZZER_PIN, 50, 500);
}
void playRequestTone()
{
    tone(BUZZER_PIN, 20, 200);
}
void stopTone()
{
    noTone(BUZZER_PIN);
}

void setNeoPixelColor(uint32_t color)
{
    for (int i = 0; i < strip.numPixels(); i++)
    {
        strip.setPixelColor(i, color);
    }
    strip.show();
}

void handleStatusChange(String newStatus)
{
    if (newStatus == "requesting")
    {
        Serial.println("ACTION: New ride request! LED Yellow.");
        setNeoPixelColor(COLOR_YELLOW);
        playRequestTone();
    }
    else if (newStatus == "accepted")
    {
        Serial.println("ACTION: Ride accepted. LED Green.");
        setNeoPixelColor(COLOR_GREEN);
        playAcceptTone();
    }
    else if (newStatus == "in_progress")
    {
        Serial.println("ACTION: Ride in progress. LED stays Green.");
        setNeoPixelColor(COLOR_GREEN);
        stopTone();
    }
    else if (newStatus == "rejected")
    {
        Serial.println("ACTION: Ride rejected by driver. LED Red.");
        setNeoPixelColor(COLOR_RED);
        playRejectTone();
    }
    else if (newStatus == "idle")
    {
        Serial.println("ACTION: Ride complete or system idle. LED Off.");
        setNeoPixelColor(COLOR_OFF);
        stopTone();
    }
}

// --- Helper function to display messages on OLED
void updateOLED_Message(String line1, String line2 = "")
{
    display.clearDisplay();
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 16);
    display.println(line1);
    if (line2 != "")
    {
        display.setTextSize(1);
        display.setCursor(0, 40);
        display.println(line2);
    }
    display.display();
}

// --- Blocking function to connect to WiFi
bool connectWiFi()
{
    updateOLED_Message("Connecting", "WiFi...");
    Serial.print("Connecting to WiFi");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int counter = 0;
    while (WiFi.status() != WL_CONNECTED && counter < 20)
    { // 10 sec timeout
        Serial.print(".");
        delay(500);
        counter++;
    }

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("\nFailed to connect to WiFi.");
        updateOLED_Message("WiFi FAILED", "Check credentials");
        delay(2000);
        return false;
    }

    Serial.println("\nWiFi Connected. IP: " + WiFi.localIP().toString());
    updateOLED_Message("WiFi OK!", WiFi.localIP().toString());
    delay(1000);
    return true;
}

// --- Blocking function to connect to Firebase
bool connectFirebase()
{
    updateOLED_Message("Connecting", "Server...");
    Serial.print("Waiting for Firebase to connect...");

    config.api_key = API_KEY;
    config.database_url = DATABASE_URL;
    config.time_zone = 0;
    auth.user.email = "nihaldas007@gmail.com";
    auth.user.password = "12345678";
    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);

    int counter = 0;
    while (!Firebase.ready() && counter < 60)
    { // 30 sec timeout
        Serial.print(".");
        delay(500);
        counter++;
    }
    Serial.println();

    if (!Firebase.ready())
    {
        Serial.println("Firebase connection FAILED!");
        updateOLED_Message("Server FAILED", "Check settings");
        delay(2000);
        return false;
    }

    Serial.println("Firebase connected successfully!");
    updateOLED_Message("Server OK!", "Ready.");
    delay(1000);
    return true;
}

// --- Modified to be blocking and run once
bool sendRideRequest()
{
    if (!Firebase.ready())
    {
        Serial.println("Firebase not ready. Cannot send request.");
        playRejectTone();
        return false;
    }

    Serial.println("Sending new ride request to Firestore...");
    updateOLED_Message("Requesting", "Ride...");
    setNeoPixelColor(COLOR_YELLOW);
    playRequestTone();

    FirebaseJson content;
    content.set("fields/status/stringValue", "requesting");
    content.set("fields/request_id/stringValue", "esp32-button-" + String(millis()));
    content.set("fields/pickup_location/stringValue", "Pahartoli, Chattogram");
    content.set("fields/dropoff_location/stringValue", "Noapara, Chattogram");
    String jsonPayload;
    content.toString(jsonPayload);

    if (Firebase.Firestore.patchDocument(&fbdo, FIREBASE_PROJECT_ID, "", documentPath.c_str(), jsonPayload.c_str(), ""))
    {
        Serial.println("Ride request SENT successfully!");
        updateOLED_Message("Request      Sent!");
        setNeoPixelColor(COLOR_YELLOW);
        delay(1000);
        return true;
    }
    else
    {
        Serial.println("FAILED to send ride request: " + fbdo.errorReason());
        updateOLED_Message("Request      FAILED");
        setNeoPixelColor(COLOR_OFF);
        playRejectTone();
        delay(2000);
        return false;
    }
}

// ---
// --- AUTHENTICATION SYSTEM FUNCTIONS ---
// ---

float getDistanceInMeters()
{
    digitalWrite(AUTH_TRIGGER_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(AUTH_TRIGGER_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(AUTH_TRIGGER_PIN, LOW);
    long duration_us = pulseIn(AUTH_ECHO_PIN, HIGH, 30000);
    float distance_m = duration_us * 0.0001715;

    if (millis() - lastAuthDebugPrint > 1000)
    {
        Serial.printf("[AUTH DEBUG] Duration: %ld us, Distance: %.2f m\n", duration_us, distance_m);
    }
    return distance_m;
}

void resetAuthSystem()
{
    isPersonInRange = false;
    isTriggerActive = false;
    isWaitingForPrivilege = false;
    privilegeGranted = false;
    privilegeResultShown = false;
    digitalWrite(AUTH_LED_PIN, LOW);
    currentDistanceM = 0.0;
}

void checkPresenceLogic(bool inRange)
{
    if (inRange)
    {
        if (!isPersonInRange)
        {
            isPersonInRange = true;
            rangeEntryTime = millis();
            Serial.println("[AUTH DEBUG] Person ENTERED range.");
        }
        else
        {
            unsigned long continuousTime = millis() - rangeEntryTime;
            if (continuousTime >= TIME_THRESHOLD_MS && !isTriggerActive)
            {
                isTriggerActive = true;
                isWaitingForPrivilege = true;
                privilegeWaitStartTime = millis();
                digitalWrite(AUTH_LED_PIN, HIGH);
                Serial.println("[AUTH DEBUG] Time threshold MET. Waiting for LDR.");
            }
        }
    }
    else
    {
        if (isPersonInRange)
        {
            Serial.println("[AUTH DEBUG] Person LEFT range. Resetting.");
            resetAuthSystem();
        }
    }
}

void checkPrivilegeLogic()
{
    unsigned long waitTime = millis() - privilegeWaitStartTime;
    if (waitTime >= PRIVILEGE_WAIT_MS)
    {
        privilegeGranted = false;
        isWaitingForPrivilege = false;
        privilegeResultShown = true;
        return;
    }
    int ldrValue = analogRead(AUTH_LDR_PIN);

    if (millis() - lastAuthDebugPrint > 1000)
    {
        Serial.printf("[AUTH DEBUG] Waiting for LDR. Current Value: %d\n", ldrValue);
    }

    if (ldrValue >= 3000 && ldrValue <= 4095)
    {
        privilegeGranted = true;
        isWaitingForPrivilege = false;
        privilegeResultShown = true;
        Serial.println("[AUTH DEBUG] LDR check PASSED. Privilege granted!");
    }
}

void updateOLED_Auth()
{
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.print("Dist: ");
    float distance_cm = currentDistanceM * 100.0;
    display.print(distance_cm, 1);
    display.println(" cm");
    display.drawFastHLine(0, 10, display.width(), SSD1306_WHITE);
    display.setCursor(0, 16);
    display.setTextSize(2);

    if (privilegeResultShown)
    {
        if (privilegeGranted)
        {
            // --- NEW: Wait for button press ---
            display.println("GRANTED!");
            display.setTextSize(1);
            display.setCursor(0, 40);
            display.println("Press button to request ride.");
        }
        else
        {
            display.println("NOT GRANTED");
        }
    }
    else if (isWaitingForPrivilege)
    {
        unsigned long elapsed = millis() - privilegeWaitStartTime;
        float elapsedSec = elapsed / 1000.0;
        display.println("Check LDR");
        display.setTextSize(1);
        display.print("(");
        display.print(elapsedSec, 1);
        display.print(" / 10.0s)");
    }
    else if (isPersonInRange)
    {
        unsigned long continuousTime = millis() - rangeEntryTime;
        float elapsedSec = continuousTime / 1000.0;
        display.print(elapsedSec, 1);
        display.print(" / 3.0s");
    }
    else
    {
        display.println("Waiting...");
    }
    display.display();
}

// --- MAIN SETUP ---

void setup()
{
    Serial.begin(115200);
    Serial.println("ESP32 Rickshaw Device Booting Up...");

    // --- Init Rickshaw Hardware ---
    strip.begin();
    strip.setBrightness(50);
    setNeoPixelColor(COLOR_OFF);
    Serial.println("Addressable LED on GPIO19 initialized.");
    pinMode(BUZZER_PIN, OUTPUT);
    Serial.println("Buzzer on GPIO27 initialized.");
    // --- RE-ENABLED RIDE BUTTON ---
    pinMode(RIDE_BUTTON_PIN, INPUT_PULLUP);
    Serial.println("Ride Button on GPIO4 initialized.");

    // --- Init Auth Hardware ---
    Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
    {
        Serial.println(F("SSD1306 allocation failed"));
        while (true)
            ; // Stop if OLED fails
    }
    else
    {
        updateOLED_Message("Booting...", "");
        Serial.println("OLED Initialized.");
        delay(2000);
    }
    pinMode(AUTH_LED_PIN, OUTPUT);
    pinMode(AUTH_TRIGGER_PIN, OUTPUT);
    pinMode(AUTH_ECHO_PIN, INPUT);
    digitalWrite(AUTH_LED_PIN, LOW);
    Serial.println("Auth sensors initialized.");

    // --- Init Firebase Path (but don't connect yet) ---
    documentPath = "artifacts/" + String(APP_ID) + "/public/data/rides/" + String(DRIVER_USER_ID);
    Serial.print("Monitoring Document Path: ");
    Serial.println(documentPath);

    Serial.println("Setup complete. Waiting for authentication.");
    currentState = STATE_WAITING_FOR_AUTH;
}

// --- MAIN NON-BLOCKING LOOP FUNCTIONS ---

void checkFirebase()
{
    if (millis() - lastFirebaseCheck > firebaseCheckInterval)
    {
        lastFirebaseCheck = millis();
        if (Firebase.ready())
        {
            Serial.print("Polling Firestore... ");
            if (Firebase.Firestore.getDocument(&fbdo, FIREBASE_PROJECT_ID, "", documentPath.c_str(), ""))
            {
                if (fbdo.httpCode() == 200)
                {
                    FirebaseJson json;
                    json.setJsonData(fbdo.payload().c_str());
                    FirebaseJsonData statusData;
                    String newStatus = "";
                    if (json.get(statusData, "fields/status/stringValue"))
                    {
                        newStatus = statusData.stringValue;
                    }
                    if (newStatus == "")
                        newStatus = "idle";
                    Serial.printf("Status: '%s'\n", newStatus.c_str());
                    if (newStatus != currentRideStatus)
                    {
                        Serial.println("--- STATUS CHANGED! ---");
                        currentRideStatus = newStatus;
                        handleStatusChange(currentRideStatus);

                        if (newStatus == "idle" || newStatus == "rejected")
                        {
                            currentState = STATE_RIDE_COMPLETE;
                            lastResultShowTime = millis();
                            updateOLED_Message(newStatus == "idle" ? "Ride      Complete" : "Ride      Rejected", "");
                        }
                    }
                }
                else
                {
                    String error = fbdo.errorReason();
                    if (error.indexOf("not found") != -1)
                    {
                        Serial.println("Document not found (idle).");
                        if (currentRideStatus != "idle")
                        {
                            Serial.println("--- STATUS CHANGED! ---");
                            currentRideStatus = "idle";
                            handleStatusChange(currentRideStatus);
                            currentState = STATE_RIDE_COMPLETE;
                            lastResultShowTime = millis();
                            updateOLED_Message("Ride      Complete", "...");
                        }
                    }
                    else
                    {
                        Serial.println("Error getting document: " + error);
                    }
                }
            }
            else
            {
                Serial.println("Error getting document: " + fbdo.errorReason());
            }
        }
        else
        {
            Serial.println("Firebase not ready.");
            currentState = STATE_RIDE_COMPLETE;
            lastResultShowTime = millis();
            updateOLED_Message("Link Error", "Shutting down...");
        }
    }
}

void checkAuthSystem()
{
    if (isWaitingForPrivilege)
    {
        checkPrivilegeLogic();
    }
    else if (privilegeResultShown)
    {
        if (lastResultShowTime == 0)
        {
            lastResultShowTime = millis();
        }
        if (millis() - lastResultShowTime > 2000)
        {
            resetAuthSystem();
            lastResultShowTime = 0;
        }
    }
    else
    {
        float newDistance = getDistanceInMeters();

        if (newDistance > 0.0)
        {
            currentDistanceM = newDistance;
        }
        bool inRange = (currentDistanceM > 0 && currentDistanceM <= DISTANCE_THRESHOLD_M);
        checkPresenceLogic(inRange);
    }
}

// --- NEW: Ride Request Button Logic ---
void checkRideRequestButton()
{
    int reading = digitalRead(RIDE_BUTTON_PIN);
    if (reading != lastRideButtonState)
    {
        lastRideButtonDebounce = millis();
    }
    if ((millis() - lastRideButtonDebounce) > rideButtonDebounceDelay)
    {
        if (reading != rideButtonState)
        {
            rideButtonState = reading;
            if (rideButtonState == LOW)
            { // Button was pressed

                // --- THIS IS THE AUTHENTICATION GATE ---
                if (privilegeGranted)
                {
                    Serial.println("Privilege granted! Button pressed. Connecting...");
                    playAcceptTone(); // Play "auth success" tone

                    // --- NOW, CONNECT AND SEND ---
                    if (connectWiFi())
                    {
                        if (connectFirebase())
                        {
                            if (sendRideRequest())
                            {
                                // Success! Move to waiting state
                                currentState = STATE_WAITING_FOR_DRIVER;
                                resetAuthSystem();
                                lastFirebaseCheck = millis(); // Start polling timer
                            }
                            else
                            {
                                // Failed to send, go back to auth
                                currentState = STATE_WAITING_FOR_AUTH;
                                resetAuthSystem();
                            }
                        }
                        else
                        {
                            // Failed to connect to Firebase, go back to auth
                            currentState = STATE_WAITING_FOR_AUTH;
                            resetAuthSystem();
                        }
                    }
                    else
                    {
                        // Failed to connect to WiFi, go back to auth
                        currentState = STATE_WAITING_FOR_AUTH;
                        resetAuthSystem();
                    }
                }
                else
                {
                    Serial.println("Button pressed, but not authenticated!");
                    playRejectTone(); // Play a "fail" sound
                    // We don't reset auth, just let them know it didn't work
                }
            }
        }
    }
    lastRideButtonState = reading;
}

// --- MAIN LOOP ---
void loop()
{
    switch (currentState)
    {
    case STATE_WAITING_FOR_AUTH:
        checkAuthSystem();        // Run the auth state machine
        checkRideRequestButton(); // --- NEW: Check for the button press ---

        if (millis() - lastOledUpdate > 100)
        {
            lastOledUpdate = millis();
            updateOLED_Auth(); // Update the auth-specific OLED screen
        }

        // --- REMOVED: No longer automatically connects on privilegeGranted ---
        /*
        if (privilegeGranted) {
            // ... logic removed ...
        }
        */
        break;

    case STATE_WAITING_FOR_DRIVER:
        // Now we just poll Firebase for status updates
        checkFirebase();
        break;

    case STATE_RIDE_COMPLETE:
        // Wait for 5 seconds to show the "Ride Complete" message
        if (millis() - lastResultShowTime > 5000)
        {
            Serial.println("Shutting down WiFi. Returning to auth mode.");
            WiFi.disconnect(true);
            setNeoPixelColor(COLOR_OFF);
            stopTone();
            resetAuthSystem();
            currentState = STATE_WAITING_FOR_AUTH;
        }
        break;
    }

    if (millis() - lastAuthDebugPrint > 1000)
    {
        lastAuthDebugPrint = millis();
    }
}