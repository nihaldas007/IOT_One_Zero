#include <Arduino.h>
// --- Main System Libraries ---
#include <WiFi.h>
#include <Firebase_ESP_Client.h>

// --- Display Libraries ---
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// --- WiFi & Firebase Config (Copied from your other ESP32) ---
#define WIFI_SSID "Das_House_lite"
#define WIFI_PASSWORD "2444666668888888"
#define API_KEY "AIzaSyD0hOasCnqllLfac7NGqm7cSQlvYnaU-ro"
#define FIREBASE_PROJECT_ID "rickshaw-puller-71731"
#define DATABASE_URL "https://rickshaw-puller-71731-default-rtdb.asia-southeast1.firebaseio.app"
#define APP_ID "default-app-id"
#define DRIVER_USER_ID "pkO9SyuhD4asdw2PoeWxd9Kqr3y1" // Must be the same ID

// --- Display Hardware Pins ---
#define OLED_RESET -1
#define OLED_SCREEN_WIDTH 128
#define OLED_SCREEN_HEIGHT 32 // --- ADJUSTED FOR 128x32 ---
#define OLED_SDA_PIN 21 // Default ESP32 I2C SDA
#define OLED_SCL_PIN 22 // Default ESP32 I2C SCL

// --- Firebase Objects ---
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// --- Display Object ---
Adafruit_SSD1306 display(OLED_SCREEN_WIDTH, OLED_SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- Global State ---
String currentRideStatus = ""; // Start empty to force first update
unsigned long lastFirebaseCheck = 0;
const long firebaseCheckInterval = 3000; // Poll Firebase every 3 seconds
String documentPath = "";

// --- Helper function to display messages on OLED ---
void updateOLED_Message(String line1, String line2 = "") {
    display.clearDisplay();
    display.setTextSize(2); // Big text for line 1
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0); // Start at top
    display.println(line1);
    if (line2 != "") {
        display.setTextSize(1); // Small text for line 2
        display.setCursor(0, 18); // Position below line 1
        display.println(line2);
    }
    display.display();
}

// --- Blocking function to connect to WiFi (runs once at start) ---
bool connectWiFi() {
    updateOLED_Message("Connecting", "WiFi...");
    Serial.print("Connecting to WiFi");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    int counter = 0;
    while (WiFi.status() != WL_CONNECTED && counter < 20) { // 10 sec timeout
        Serial.print(".");
        delay(500);
        counter++;
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\nFailed to connect to WiFi.");
        updateOLED_Message("WiFi FAILED");
        delay(2000);
        return false;
    }
    
    Serial.println("\nWiFi Connected. IP: " + WiFi.localIP().toString());
    updateOLED_Message("WiFi OK!");
    delay(1000);
    return true;
}

// --- Blocking function to connect to Firebase (runs once at start) ---
bool connectFirebase() {
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
    while (!Firebase.ready() && counter < 60) { // 30 sec timeout
        Serial.print(".");
        delay(500);
        counter++;
    }
    Serial.println();

    if (!Firebase.ready()) {
        Serial.println("Firebase connection FAILED!");
        updateOLED_Message("Server FAILED");
        delay(2000);
        return false;
    }

    Serial.println("Firebase connected successfully!");
    updateOLED_Message("Server OK!");
    delay(1000);
    return true;
}

// --- This function is called when the status changes ---
void handleStatusChange(String newStatus) {
    
    if (newStatus == "requesting") {
        Serial.println("OLED: New ride request!");
        updateOLED_Message("New      Request", "");
        
    } else if (newStatus == "accepted") {
        Serial.println("OLED: Ride accepted.");
        updateOLED_Message("Ride      Accepted", "");

    } else if (newStatus == "in_progress") {
        Serial.println("OLED: Pickup confirmed.");
        updateOLED_Message("Pickup OK", "Ride in Progress");

    } else if (newStatus == "rejected") {
        Serial.println("OLED: Ride rejected.");
        updateOLED_Message("Ride      Rejected", "");
        delay(3000); // Show this message for 3 seconds
        // After delay, we'll poll again and it will be "idle"

    } else if (newStatus == "idle") {
        Serial.println("OLED: Ride complete or idle.");
        // Only show "Ride Complete" if the *previous* state was a ride
        if (currentRideStatus == "in_progress" || currentRideStatus == "rejected") {
             updateOLED_Message("Ride      Complete", "");
        } else {
             updateOLED_Message("DRIVER    MONITOR", "");
        }
    }
}

// --- MAIN SETUP ---
void setup() {
    Serial.begin(115200);
    Serial.println("ESP32 Driver Display Booting Up...");

    // --- Init Display Hardware ---
    Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println(F("SSD1306 allocation failed"));
        while(true); // Stop if OLED fails
    } else {
        updateOLED_Message("System Booted", "Starting...");
        Serial.println("OLED Initialized.");
        delay(1000);
    }
    
    // --- Init Firebase Path ---
    documentPath = "artifacts/" + String(APP_ID) + "/public/data/rides/" + String(DRIVER_USER_ID);
    Serial.print("Monitoring Document Path: ");
    Serial.println(documentPath);
    
    // --- Connect to WiFi & Firebase ---
    if (connectWiFi()) {
        if (!connectFirebase()) {
            // Error already shown on OLED
            while(true); // Stop
        }
    } else {
        // Error already shown on OLED
        while(true); // Stop
    }
    
    Serial.println("Setup complete. Starting main polling loop.");
    updateOLED_Message("DRIVER    MONITOR", "");
}


// --- MAIN LOOP: Polls Firebase for status changes ---
void loop() {
    if (millis() - lastFirebaseCheck > firebaseCheckInterval) {
        lastFirebaseCheck = millis();

        if (Firebase.ready()) {
            Serial.print("Polling Firestore... ");
            if (Firebase.Firestore.getDocument(&fbdo, FIREBASE_PROJECT_ID, "", documentPath.c_str(), "")) {
                
                String newStatus = "idle"; // Default to idle

                if (fbdo.httpCode() == 200) {
                    // Document was found
                    FirebaseJson json;
                    json.setJsonData(fbdo.payload().c_str());
                    FirebaseJsonData statusData;
                    
                    if (json.get(statusData, "fields/status/stringValue")) {
                        newStatus = statusData.stringValue;
                    }
                
                } else {
                    // Document was not found (http 404), which also means we are "idle"
                    Serial.println("Document not found (idle).");
                    newStatus = "idle";
                }

                // Check if the status has actually changed
                if (newStatus != currentRideStatus) {
                    Serial.printf("Status changed from '%s' to '%s'\n", currentRideStatus.c_str(), newStatus.c_str());
                    handleStatusChange(newStatus); // Call OLED update function
                    currentRideStatus = newStatus; // Save the new state
                } else {
                    Serial.printf("Status unchanged: '%s'\n", currentRideStatus.c_str());
                }

            } else {
                // Real error (not 404)
                Serial.println("Error getting document: " + fbdo.errorReason());
                updateOLED_Message("Link Error", fbdo.errorReason().c_str());
            }
        } else {
            Serial.println("Firebase not ready. Trying to reconnect...");
            updateOLED_Message("Link Error", "Reconnecting...");
            connectFirebase(); // Try to reconnect
        }
    }
}