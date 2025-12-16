#include "rollershutter.h"
#include <Arduino.h>
#include <platform/KeyValueStoreManager.h>
#include <esp_log.h>

static const char* TAG = "Shutter";
extern bool hardware_initialized;

// Thread-Safe ISR-Variables
portMUX_TYPE RollerShutter::pulseMux = portMUX_INITIALIZER_UNLOCKED;
volatile int32_t RollerShutter::pulseBuffer = 0;
volatile bool RollerShutter::isr_ready = false; 

void IRAM_ATTR RollerShutter::onPulseInterrupt() {
    if (!isr_ready) return;
    
    portENTER_CRITICAL_ISR(&pulseMux);
    pulseBuffer++;
    portEXIT_CRITICAL_ISR(&pulseMux);
}

RollerShutter::RollerShutter() {
    pins.pulseCounter = CONFIG_PULSE_COUNTER_PIN;
    pins.motorUp = CONFIG_MOTOR_UP_PIN;
    pins.motorDown = CONFIG_MOTOR_DOWN_PIN;
    pins.buttonUp = CONFIG_BUTTON_UP_PIN;
    pins.buttonDown = CONFIG_BUTTON_DOWN_PIN;
}

void RollerShutter::loop() {
    handleInputs();
    handleStateMachine();
    applyMotorAction();
    handleButtonRelease();
}

// --- Public API Implementation ---

void RollerShutter::loadStateFromKVS() {
    size_t len;
    CHIP_ERROR err;

    err = chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Get("max_count", &maxPulseCount, sizeof(maxPulseCount), &len);
    
    if (err == CHIP_ERROR_KEY_NOT_FOUND) {
        maxPulseCount = 0;
        calibrated = false;
        ESP_LOGI(TAG, "No max_count in KVS. Needs calibration.");
    } else if (err == CHIP_NO_ERROR) {
        calibrated = (maxPulseCount > 0);
        
        if (calibrated) {
            ESP_LOGI(TAG, "Loaded max_count = %ld (CALIBRATED)", (long)maxPulseCount);
        } else {
            ESP_LOGW(TAG, "Loaded max_count = 0 (NOT CALIBRATED)");
        }
    }

    uint8_t dir_inv = 0;
    err = chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Get("dir_inv", &dir_inv, sizeof(dir_inv), &len);
    directionInverted = (dir_inv != 0);

    uint8_t logic_val = 0;
    err = chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Get("win_logic", &logic_val, sizeof(logic_val), &len);
    windowLogic = static_cast<WindowOpenLogic>(logic_val);
    
    ESP_LOGI(TAG, "State loaded: direction=%s, windowLogic=%d", 
             directionInverted ? "inverted" : "normal", (int)windowLogic);
}

void RollerShutter::saveStateToKVS() {
    chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Put("max_count", &maxPulseCount, sizeof(maxPulseCount));
    uint8_t dir_inv = directionInverted ? 1 : 0;
    chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Put("dir_inv", &dir_inv, sizeof(dir_inv));
    uint8_t logic_val = static_cast<uint8_t>(windowLogic);
    chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Put("win_logic", &logic_val, sizeof(logic_val));
    ESP_LOGI(TAG, "State saved to KVS.");
}

void RollerShutter::moveToPercent(uint8_t percent) {
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "MOVE COMMAND RECEIVED");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    
    if (!calibrated) {
        ESP_LOGE(TAG, "✗ ABORT: Not calibrated!");
        ESP_LOGE(TAG, "  → Run calibration first");
        return;
    }
    
    if (percent > 100) {
        ESP_LOGW(TAG, "⚠ Invalid percentage: %d%% → Clamping to 100%%", percent);
        percent = 100;
    }
    
    uint8_t currentPercent = getCurrentPercent();
    ESP_LOGI(TAG, "Current position: %d%%", currentPercent);
    ESP_LOGI(TAG, "Target position:  %d%%", percent);
    ESP_LOGI(TAG, "Direction: %s", percent > currentPercent ? "DOWN ↓" : "UP ↑");
    
    // Calculate target pulses
    int32_t newTarget = (maxPulseCount * percent) / 100;
    newTarget = constrain(newTarget, 0, maxPulseCount);
    
    ESP_LOGI(TAG, "Current pulses: %ld", (long)currentPulseCount);
    ESP_LOGI(TAG, "Target pulses:  %ld", (long)newTarget);
    ESP_LOGI(TAG, "Max pulses:     %ld", (long)maxPulseCount);
    
    // Check if already at target
    if (abs(newTarget - currentPulseCount) <= 1) {
        ESP_LOGI(TAG, "✓ Already at target position (tolerance: ±1 pulse)");
        ESP_LOGI(TAG, "  → No movement needed");
        targetPulseCount = -1;
        return;
    }
    
    // Window sensor logic
    if (percent > currentPercent && windowIsOpen && 
        windowLogic != WindowOpenLogic::LOGIC_DISABLED) {
        
        ESP_LOGW(TAG, "═══════════════════════════════════");
        ESP_LOGW(TAG, "⚠ WINDOW OPEN - APPLYING LOGIC");
        ESP_LOGW(TAG, "═══════════════════════════════════");
        ESP_LOGW(TAG, "Window state: OPEN");
        ESP_LOGW(TAG, "Movement direction: DOWNWARD");
        ESP_LOGW(TAG, "Active logic: %d", (int)windowLogic);
        
        switch (windowLogic) {
            case WindowOpenLogic::BLOCK_DOWNWARD:
                ESP_LOGI(TAG, "→ Logic: BLOCK_DOWNWARD");
                ESP_LOGI(TAG, "  ✗ Command rejected - window is open");
                return;
                
            case WindowOpenLogic::OPEN_FULLY:
                ESP_LOGI(TAG, "→ Logic: OPEN_FULLY");
                ESP_LOGI(TAG, "  ✓ Overriding target: %d%% → 0%%", percent);
                percent = 0;
                newTarget = 0;
                break;
                
            case WindowOpenLogic::VENTILATION_POSITION:
                ESP_LOGI(TAG, "→ Logic: VENTILATION_POSITION");
                ESP_LOGI(TAG, "  ✓ Overriding target: %d%% → %d%%", percent, VENTILATION_PERCENTAGE);
                percent = VENTILATION_PERCENTAGE;
                newTarget = (maxPulseCount * VENTILATION_PERCENTAGE) / 100;
                break;
                
            default: 
                break;
        }
        ESP_LOGW(TAG, "═══════════════════════════════════");
    }
    
    targetPulseCount = newTarget;
    
    int32_t delta = abs(newTarget - currentPulseCount);
    float estimatedTime = (float)delta / 10.0f;  // Assuming ~10 pulses/sec
    
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "✓ MOVEMENT STARTED");
    ESP_LOGI(TAG, "  Target: %d%% (%ld pulses)", percent, (long)newTarget);
    ESP_LOGI(TAG, "  Distance: %ld pulses", (long)delta);
    ESP_LOGI(TAG, "  Est. time: %.1f seconds", estimatedTime);
    ESP_LOGI(TAG, "═══════════════════════════════════");
}

void RollerShutter::stop() {
    targetPulseCount = currentPulseCount;
    ESP_LOGI(TAG, "Stop command received.");
}

void RollerShutter::startCalibration() {
    if (currentState != State::STOPPED) {
        ESP_LOGW(TAG, "Already moving. Ignoring calibration command.");
        return;
    }
    ESP_LOGI(TAG, "Starting calibration sequence.");
    calibrated = false;
    currentState = State::CALIBRATING_UP;
    calibrationStartTime = millis();
    triggerMoveUp();
}

void RollerShutter::setDirectionInverted(bool inverted) {
    if (directionInverted != inverted) {
        directionInverted = inverted;
        saveState();
        ESP_LOGI(TAG, "Direction set to: %s", inverted ? "Inverted" : "Normal");
    }
}

void RollerShutter::setWindowState(bool isOpen) {
    if (windowIsOpen != isOpen) {
        windowIsOpen = isOpen;
        ESP_LOGI(TAG, "Window state changed to: %s", isOpen ? "OPEN" : "CLOSED");
    }
}

void RollerShutter::setWindowOpenLogic(WindowOpenLogic logic) {
    if (windowLogic != logic) {
        windowLogic = logic;
        saveState();
        ESP_LOGI(TAG, "Window logic changed to: %d", (int)logic);
    }
}

// --- Getters ---

uint8_t RollerShutter::getCurrentPercent() const {
    if (maxPulseCount == 0) return 0;
    return (uint8_t)((currentPulseCount * 100) / maxPulseCount);
}

bool RollerShutter::isCalibrated() const { return calibrated; }
bool RollerShutter::isDirectionInverted() const { return directionInverted; }
RollerShutter::State RollerShutter::getCurrentState() const { return currentState; }

bool RollerShutter::hasPositionChanged() {
    uint8_t current = getCurrentPercent();
    if (abs(current - lastReportedPercent) >= 1) {
        lastReportedPercent = current;
        return true;
    }
    return false;
}

// --- Internal Logic ---

void RollerShutter::handleInputs() {
    if (!hardware_initialized_local) return;

    // atomic read and reset of pulseBuffer
    int32_t pulses;
    portENTER_CRITICAL(&pulseMux);
    pulses = pulseBuffer;
    pulseBuffer = 0;
    portEXIT_CRITICAL(&pulseMux);

    // motor direction detection
    State detectedDirection;
    if (digitalRead(pins.motorDown) == LOW) {
        detectedDirection = State::MOVING_DOWN;
    } else if (digitalRead(pins.motorUp) == LOW) {
        detectedDirection = State::MOVING_UP;
    } else {
        detectedDirection = State::STOPPED;
    }

    // Debouncing: direction must be stable for several loops
    if (detectedDirection == lastActualDirection) {
        if (directionStableCounter < DIRECTION_STABILITY_THRESHOLD) {
            directionStableCounter++;
        }
    } else {
        directionStableCounter = 0;
        lastActualDirection = detectedDirection;
    }

    // only update actualDirection if stable
    if (directionStableCounter >= DIRECTION_STABILITY_THRESHOLD) {
        actualDirection = detectedDirection;
    }

    // pulse count update
    if (pulses > 0 && actualDirection != State::STOPPED) {
        if (actualDirection == State::MOVING_DOWN) {
            currentPulseCount += pulses;
        } else {
            currentPulseCount -= pulses;
        }

        currentPulseCount = constrain(currentPulseCount, 0, 
            calibrated ? maxPulseCount : INT32_MAX);
        positionChanged = true;
    }
}

void RollerShutter::handleStateMachine() {
    // global timeout for calibration
    if ((currentState == State::CALIBRATING_UP || currentState == State::CALIBRATING_DOWN) &&
        (millis() - calibrationStartTime > CALIBRATION_TIMEOUT)) {
        
        ESP_LOGE(TAG, "Calibration timeout after %lums. Aborting.", 
                 millis() - calibrationStartTime);
        
        triggerStop();
        currentState = State::STOPPED;
        calibrated = false;
        return;
    }

    switch (currentState) {
        case State::STOPPED:
            if (targetPulseCount != -1) {
                if (targetPulseCount > currentPulseCount) {
                    currentState = State::MOVING_DOWN;
                } else if (targetPulseCount < currentPulseCount) {
                    currentState = State::MOVING_UP;
                } else {
                    targetPulseCount = -1;
                }
            }
            break;

        case State::MOVING_UP:
            if (actualDirection == State::STOPPED || 
                (targetPulseCount != -1 && currentPulseCount <= targetPulseCount)) {
                triggerStop();
                targetPulseCount = -1;
                currentState = State::STOPPED;
                ESP_LOGI(TAG, "Reached UP target. Stopping.");
            }
            break;

        case State::MOVING_DOWN:
            if (actualDirection == State::STOPPED || 
                (targetPulseCount != -1 && currentPulseCount >= targetPulseCount)) {
                triggerStop();
                targetPulseCount = -1;
                currentState = State::STOPPED;
                ESP_LOGI(TAG, "Reached DOWN target. Stopping.");
            }
            break;

        case State::CALIBRATING_UP:
            if (actualDirection == State::STOPPED && desiredMotorAction == State::MOVING_UP) {
                ESP_LOGI(TAG, "Calibration: Top limit reached.");
                currentPulseCount = 0;
                positionChanged = true;
                currentState = State::CALIBRATING_DOWN;
                triggerMoveDown();
            }
            break;

        case State::CALIBRATING_DOWN:
            if (actualDirection == State::STOPPED && desiredMotorAction == State::MOVING_DOWN) {
                ESP_LOGI(TAG, "Calibration: Bottom limit reached.");
                                maxPulseCount = currentPulseCount;
                calibrated = true;
                saveState();
                currentState = State::STOPPED;
                ESP_LOGI(TAG, "Calibration complete. Max pulses: %ld", (long)maxPulseCount);
            }
            break;
    }
}

void RollerShutter::applyMotorAction() {
    State action = State::STOPPED;

    if (currentState == State::MOVING_UP || currentState == State::CALIBRATING_UP) {
        action = State::MOVING_UP;
    } else if (currentState == State::MOVING_DOWN || currentState == State::CALIBRATING_DOWN) {
        action = State::MOVING_DOWN;
    }

    if (action != desiredMotorAction) {
        if (action == State::MOVING_UP) triggerMoveUp();
        else if (action == State::MOVING_DOWN) triggerMoveDown();
        else if (desiredMotorAction != State::STOPPED) triggerStop();

        desiredMotorAction = action;
    }
}

void RollerShutter::triggerMoveUp() {
    ESP_LOGD(TAG, "Triggering UP button.");
    uint8_t pin = directionInverted ? pins.buttonDown : pins.buttonUp;
    startButtonPress(pin);
}

void RollerShutter::triggerMoveDown() {
    ESP_LOGD(TAG, "Triggering DOWN button.");
    uint8_t pin = directionInverted ? pins.buttonUp : pins.buttonDown;
    startButtonPress(pin);
}

void RollerShutter::triggerStop() {
    ESP_LOGD(TAG, "Triggering STOP button.");
    uint8_t pin = directionInverted ? pins.buttonUp : pins.buttonDown;
    startButtonPress(pin);
}

void RollerShutter::startButtonPress(uint8_t pin) {
    if (buttonActive) return;
    digitalWrite(pin, LOW);
    buttonPressStart = millis();
    buttonActive = true;
    activeButtonPin = pin;
    ESP_LOGD(TAG, "Button pressed: pin %d", pin);
}

void RollerShutter::handleButtonRelease() {
    if (buttonActive && (millis() - buttonPressStart >= BUTTON_PRESS_DURATION)) {
        digitalWrite(activeButtonPin, HIGH);
        buttonActive = false;
        ESP_LOGD(TAG, "Button released: pin %d", activeButtonPin);
    }
}

void RollerShutter::saveState() {
    static int32_t lastMax = -1;
    static bool lastInverted = false;
    static WindowOpenLogic lastLogic = WindowOpenLogic::LOGIC_DISABLED;

    if (maxPulseCount != lastMax || directionInverted != lastInverted || windowLogic != lastLogic) {
        saveStateToKVS();
        lastMax = maxPulseCount;
        lastInverted = directionInverted;
        lastLogic = windowLogic;
    }
}

void RollerShutter::initHardware() {
    ESP_LOGI(TAG, "Initializing hardware...");
    
    isr_ready = false;

    gpio_isr_handler_remove((gpio_num_t)pins.pulseCounter);
    
    detachInterrupt(digitalPinToInterrupt(pins.pulseCounter));

    pinMode(pins.pulseCounter, INPUT_PULLUP);
    pinMode(pins.motorUp, INPUT_PULLUP);
    pinMode(pins.motorDown, INPUT_PULLUP);
    pinMode(pins.buttonUp, OUTPUT);
    digitalWrite(pins.buttonUp, HIGH);
    pinMode(pins.buttonDown, OUTPUT);
    digitalWrite(pins.buttonDown, HIGH);

    // reset pulse buffer
    portENTER_CRITICAL(&pulseMux);
    pulseBuffer = 0;
    portEXIT_CRITICAL(&pulseMux);

    // install ISR service
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to install ISR service: %d", err);
    }

    // attach ISR handler
    attachInterrupt(digitalPinToInterrupt(pins.pulseCounter), 
                    RollerShutter::onPulseInterrupt, FALLING);

    // activate ISR
    isr_ready = true;
    
    hardware_initialized_local = true;
    ESP_LOGI(TAG, "Hardware ready (ISR on GPIO%d).", pins.pulseCounter);
}

                
