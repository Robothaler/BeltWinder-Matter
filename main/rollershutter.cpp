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
volatile uint32_t RollerShutter::isr_trigger_count = 0;
volatile uint32_t RollerShutter::isr_rejected_count = 0;
volatile uint32_t RollerShutter::isr_pulse_count = 0;

void IRAM_ATTR RollerShutter::onPulseInterrupt() {
    isr_trigger_count++;
    
    if (!isr_ready) {
        isr_rejected_count++;
        return;
    }
    
    portENTER_CRITICAL_ISR(&pulseMux);
    pulseBuffer++;
    isr_pulse_count++;
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
    periodicSave();
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

    err = chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Get("current_count", &currentPulseCount, sizeof(currentPulseCount), &len);
    
    if (err == CHIP_ERROR_KEY_NOT_FOUND) {
        // Fallback: Wenn nicht gespeichert, auf 0 setzen
        currentPulseCount = 0;
        ESP_LOGW(TAG, "No current_count in KVS. Starting at 0%%.");
    } else if (err == CHIP_NO_ERROR) {
        // Validiere dass currentPulseCount im gültigen Bereich ist
        if (currentPulseCount < 0) {
            ESP_LOGW(TAG, "currentPulseCount negative (%ld), correcting to 0", (long)currentPulseCount);
            currentPulseCount = 0;
        } else if (calibrated && currentPulseCount > maxPulseCount) {
            ESP_LOGW(TAG, "currentPulseCount (%ld) > maxPulseCount (%ld), correcting", 
                     (long)currentPulseCount, (long)maxPulseCount);
            currentPulseCount = maxPulseCount;
        }
        
        ESP_LOGI(TAG, "Loaded current_count = %ld (%d%%)", 
                 (long)currentPulseCount, getCurrentPercent());
    }

    uint8_t dir_inv = 0;
    err = chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Get("dir_inv", &dir_inv, sizeof(dir_inv), &len);
    directionInverted = (dir_inv != 0);

    uint8_t logic_val = 0;
    err = chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Get("win_logic", &logic_val, sizeof(logic_val), &len);
    windowLogic = static_cast<WindowOpenLogic>(logic_val);

    err = chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Get("top_history", topLimitHistory, sizeof(topLimitHistory), &len);
    if (err != CHIP_NO_ERROR) {
        memset(topLimitHistory, 0, sizeof(topLimitHistory));
    }
    
    err = chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Get("bottom_history", bottomLimitHistory, sizeof(bottomLimitHistory), &len);
    if (err != CHIP_NO_ERROR) {
        memset(bottomLimitHistory, 0, sizeof(bottomLimitHistory));
    }
    
    err = chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Get("top_idx", &topLimitHistoryIndex, sizeof(topLimitHistoryIndex), &len);
    if (err != CHIP_NO_ERROR) {
        topLimitHistoryIndex = 0;
    }
    
    err = chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Get("bottom_idx", &bottomLimitHistoryIndex, sizeof(bottomLimitHistoryIndex), &len);
    if (err != CHIP_NO_ERROR) {
        bottomLimitHistoryIndex = 0;
    }
    
    err = chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Get("cycle_count", &fullCycleCount, sizeof(fullCycleCount), &len);
    if (err != CHIP_NO_ERROR) {
        fullCycleCount = 0;
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   STATE LOADED FROM NVS           ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "  maxPulseCount:      %ld", (long)maxPulseCount);
    ESP_LOGI(TAG, "  currentPulseCount:  %ld", (long)currentPulseCount);
    ESP_LOGI(TAG, "  Current Position:   %d%%", getCurrentPercent());
    ESP_LOGI(TAG, "  Calibrated:         %s", calibrated ? "YES" : "NO");
    ESP_LOGI(TAG, "  Direction:          %s", directionInverted ? "INVERTED" : "NORMAL");
    ESP_LOGI(TAG, "  Window Logic:       %d", (int)windowLogic);
    ESP_LOGI(TAG, "");
}

void RollerShutter::saveStateToKVS() {
    chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Put("max_count", &maxPulseCount, sizeof(maxPulseCount));
    chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Put("current_count", &currentPulseCount, sizeof(currentPulseCount));
    uint8_t dir_inv = directionInverted ? 1 : 0;
    chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Put("dir_inv", &dir_inv, sizeof(dir_inv));
    uint8_t logic_val = static_cast<uint8_t>(windowLogic);
    chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Put("win_logic", &logic_val, sizeof(logic_val));

    chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Put("top_history", topLimitHistory, sizeof(topLimitHistory));
    chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Put("bottom_history", bottomLimitHistory, sizeof(bottomLimitHistory));
    chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Put("top_idx", &topLimitHistoryIndex, sizeof(topLimitHistoryIndex));
    chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Put("bottom_idx", &bottomLimitHistoryIndex, sizeof(bottomLimitHistoryIndex));
    chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Put("cycle_count", &fullCycleCount, sizeof(fullCycleCount));
    ESP_LOGI(TAG, "State saved to KVS (max=%ld, current=%ld)", 
             (long)maxPulseCount, (long)currentPulseCount);
}

void RollerShutter::moveToPercent(uint8_t percent) {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║                moveToPercent() CALLED                     ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "INPUT:");
    ESP_LOGI(TAG, "  → Requested percent:    %d%%", percent);
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "CURRENT STATE:");
    ESP_LOGI(TAG, "  → currentPulseCount:    %ld", (long)currentPulseCount);
    ESP_LOGI(TAG, "  → maxPulseCount:        %ld", (long)maxPulseCount);
    ESP_LOGI(TAG, "  → Current percent:      %d%%", getCurrentPercent());
    ESP_LOGI(TAG, "  → calibrated:           %s", calibrated ? "YES" : "NO");
    ESP_LOGI(TAG, "  → currentState:         %d", (int)currentState);
    ESP_LOGI(TAG, "  → targetPulseCount:     %ld", (long)targetPulseCount);
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "CONFIGURATION:");
    ESP_LOGI(TAG, "  → windowIsOpen:         %s", windowIsOpen ? "YES" : "NO");
    ESP_LOGI(TAG, "  → windowLogic:          %d", (int)windowLogic);
    ESP_LOGI(TAG, "  → directionInverted:    %s", directionInverted ? "YES" : "NO");
    ESP_LOGI(TAG, "");
    
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
    ESP_LOGI(TAG, "Current: %d%% (%ld pulses)", currentPercent, (long)currentPulseCount);
    ESP_LOGI(TAG, "Current position: %d%%", currentPercent);
    ESP_LOGI(TAG, "Target position:  %d%%", percent);
    ESP_LOGI(TAG, "Direction: %s", percent > currentPercent ? "DOWN ↓" : "UP ↑");
    
    // Calculate target pulses
    int32_t newTarget = (maxPulseCount * percent) / 100;
    newTarget = constrain(newTarget, 0, maxPulseCount);
    
    ESP_LOGI(TAG, "Current pulses: %ld", (long)currentPulseCount);
    ESP_LOGI(TAG, "Target pulses:  %ld", (long)newTarget);
    ESP_LOGI(TAG, "Delta: %ld pulses", (long)abs(newTarget - currentPulseCount));
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

    positionChanged = true;
}

void RollerShutter::stop() {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "════════════════════════════════════");
    ESP_LOGI(TAG, "  🛑 STOP CALLED");
    ESP_LOGI(TAG, "════════════════════════════════════");
    ESP_LOGI(TAG, "Current State: %d", (int)currentState);
    ESP_LOGI(TAG, "actualDirection: %d", (int)actualDirection);
    ESP_LOGI(TAG, "Motor Status: UP=%d, DOWN=%d", 
             digitalRead(CONFIG_MOTOR_UP_PIN) == LOW,
             digitalRead(CONFIG_MOTOR_DOWN_PIN) == LOW);
    ESP_LOGI(TAG, "");
    
    // State setzen (BEVOR triggerStop(), damit actualDirection noch valide ist)
    if (currentState == State::MOVING_UP || 
        currentState == State::MOVING_DOWN ||
        currentState == State::CALIBRATING_UP ||
        currentState == State::CALIBRATING_DOWN) {
        
        ESP_LOGI(TAG, "→ Triggering stop button press...");
        
        // triggerStop() wählt den richtigen Button basierend auf actualDirection
        triggerStop();
        
        currentState = State::STOPPED;
        
        ESP_LOGI(TAG, "✓ Stop initiated");
        ESP_LOGI(TAG, "✓ State changed to STOPPED");
        ESP_LOGI(TAG, "");
        
        saveStateToKVS();  // Position sichern
        
        positionChanged = true;
        
    } else {
        ESP_LOGW(TAG, "⚠ Stop called but already in state: %d", (int)currentState);
        ESP_LOGI(TAG, "  actualDirection: %d", (int)actualDirection);
        
        // ────────────────────────────────────────────────────────────
        // ⚠️ Edge Case: State ist STOPPED, aber Motor läuft noch
        // ────────────────────────────────────────────────────────────
        if (actualDirection != State::STOPPED) {
            ESP_LOGW(TAG, "");
            ESP_LOGW(TAG, "⚠️ EDGE CASE DETECTED:");
            ESP_LOGW(TAG, "  State = STOPPED, but actualDirection = %d", (int)actualDirection);
            ESP_LOGW(TAG, "  → Motor is still running! Forcing stop...");
            ESP_LOGW(TAG, "");
            
            triggerStop();
            
            ESP_LOGI(TAG, "✓ Force-stop initiated");
        }
        
        ESP_LOGI(TAG, "");
    }
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

    // ═══════════════════════════════════════════════════════════════
    // DEBUG: ISR-Statistiken alle 500ms ausgeben
    // ═══════════════════════════════════════════════════════════════
    static uint32_t last_isr_debug = 0;
    if (millis() - last_isr_debug >= 500) {
        last_isr_debug = millis();
        
        ESP_LOGD(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGD(TAG, "║   ISR STATISTICS                  ║");
        ESP_LOGD(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGD(TAG, "  ISR Triggers:     %lu", (unsigned long)isr_trigger_count);
        ESP_LOGD(TAG, "  ISR Rejected:     %lu", (unsigned long)isr_rejected_count);
        ESP_LOGD(TAG, "  ISR Pulses:       %lu", (unsigned long)isr_pulse_count);
        ESP_LOGD(TAG, "  pulseBuffer:      %ld", (long)pulseBuffer);
        ESP_LOGD(TAG, "  currentPulseCount: %ld", (long)currentPulseCount);
        ESP_LOGD(TAG, "  isr_ready:        %s", isr_ready ? "YES" : "NO");
        ESP_LOGD(TAG, "  hardware_init:    %s", hardware_initialized_local ? "YES" : "NO");
        ESP_LOGD(TAG, "");
    }

    // ═══════════════════════════════════════════════════════════════
    // DEBUG: Motor-Pins (alle 100ms)
    // ═══════════════════════════════════════════════════════════════
    static uint32_t last_motor_debug = 0;
    if (millis() - last_motor_debug >= 100) {
        last_motor_debug = millis();
        int up = digitalRead(pins.motorUp);
        int down = digitalRead(pins.motorDown);
        int pulse_pin = digitalRead(pins.pulseCounter);
        
        ESP_LOGI(TAG, "Motor: UP=%d, DOWN=%d, Pulse=%d, actualDir=%d", 
                 up, down, pulse_pin, (int)actualDirection);
    }

    // ═══════════════════════════════════════════════════════════════
    // Pulse-Verarbeitung (Atomic Read & Reset)
    // ═══════════════════════════════════════════════════════════════
    int32_t pulses;
    portENTER_CRITICAL(&pulseMux);
    pulses = pulseBuffer;
    pulseBuffer = 0;
    portEXIT_CRITICAL(&pulseMux);

    if (pulses > 0) {
        ESP_LOGI(TAG, "✓✓✓ Received %ld pulses! ✓✓✓", (long)pulses);
    }

    // ═══════════════════════════════════════════════════════════════
    // Motor Direction Detection (Hardware-basiert)
    // ═══════════════════════════════════════════════════════════════
    State detectedDirection;
    if (digitalRead(pins.motorDown) == LOW) {
        detectedDirection = State::MOVING_DOWN;
    } else if (digitalRead(pins.motorUp) == LOW) {
        detectedDirection = State::MOVING_UP;
    } else {
        detectedDirection = State::STOPPED;
    }

    // ═══════════════════════════════════════════════════════════════
    // Debouncing: Direction must be stable for several loops
    // ═══════════════════════════════════════════════════════════════
    if (detectedDirection == lastActualDirection) {
        if (directionStableCounter < DIRECTION_STABILITY_THRESHOLD) {
            directionStableCounter++;
        }
    } else {
        directionStableCounter = 0;
        lastActualDirection = detectedDirection;
        ESP_LOGD(TAG, "Direction changed: %d → %d (counter reset)", 
                 (int)actualDirection, (int)detectedDirection);
    }

    // Only update actualDirection if stable
    if (directionStableCounter >= DIRECTION_STABILITY_THRESHOLD) {
        if (actualDirection != detectedDirection) {
            ESP_LOGI(TAG, "Direction stable: %d → %d", 
                     (int)actualDirection, (int)detectedDirection);
            actualDirection = detectedDirection;
        }
    }

    // ═══════════════════════════════════════════════════════════════
    // PULSE COUNT UPDATE - STATE-AWARE LOGIC
    // ═══════════════════════════════════════════════════════════════
    if (pulses > 0) {
        
        // ────────────────────────────────────────────────────────────
        // CALIBRATING_UP: Pulse ZÄHLEN (für bidirektionale Validierung)
        // ────────────────────────────────────────────────────────────
        if (currentState == State::CALIBRATING_UP) {
            // UP-Phase: Pulse zählen
            calibrationUpPulses += pulses;
            ESP_LOGI(TAG, "→ CALIBRATING_UP: Added %ld pulses, total=%ld", 
                    (long)pulses, (long)calibrationUpPulses);
            positionChanged = true;
        
        // ────────────────────────────────────────────────────────────
        // CALIBRATING_DOWN: Pulse ZÄHLEN (für bidirektionale Validierung)
        // ────────────────────────────────────────────────────────────
        } else if (currentState == State::CALIBRATING_DOWN) {
            calibrationDownPulses += pulses;
            ESP_LOGI(TAG, "→ CALIBRATING_DOWN: Added %ld pulses, total=%ld", 
                     (long)pulses, (long)calibrationDownPulses);
            positionChanged = true;
        }
        
        // ────────────────────────────────────────────────────────────
        // NORMALE BEWEGUNG: Verwende desiredMotorAction für Richtung
        // ────────────────────────────────────────────────────────────
        else if (currentState == State::MOVING_UP || 
                 currentState == State::MOVING_DOWN) {
            
            State directionForPulses = desiredMotorAction;
            
            if (directionForPulses == State::MOVING_DOWN) {
                currentPulseCount += pulses;
                ESP_LOGI(TAG, "→ DOWN: Added %ld pulses, count=%ld", 
                         (long)pulses, (long)currentPulseCount);
                
            } else if (directionForPulses == State::MOVING_UP) {
                currentPulseCount -= pulses;
                if (currentPulseCount < 0) currentPulseCount = 0;
                ESP_LOGI(TAG, "→ UP: Subtracted %ld pulses, count=%ld", 
                         (long)pulses, (long)currentPulseCount);
                
            } else {
                ESP_LOGW(TAG, "⚠ Pulse received but desiredMotorAction=STOPPED, discarding %ld pulses", 
                         (long)pulses);
            }

            // Constrain to valid range
            currentPulseCount = constrain(currentPulseCount, 0, 
                calibrated ? maxPulseCount : INT32_MAX);
            positionChanged = true;
        }

        // ────────────────────────────────────────────────────────────
        // MANUELLE BEWEGUNG (State=STOPPED, aber Motor läuft!)
        // ────────────────────────────────────────────────────────────
        else if (currentState == State::STOPPED && actualDirection != State::STOPPED) {
            ESP_LOGI(TAG, "═══════════════════════════════════");
            ESP_LOGI(TAG, "⚙ MANUAL MOVEMENT DETECTED");
            ESP_LOGI(TAG, "═══════════════════════════════════");
            
            if (actualDirection == State::MOVING_DOWN) {
                currentPulseCount += pulses;
                ESP_LOGI(TAG, "→ MANUAL DOWN: Added %ld pulses, count=%ld", 
                         (long)pulses, (long)currentPulseCount);
                
            } else if (actualDirection == State::MOVING_UP) {
                currentPulseCount -= pulses;
                if (currentPulseCount < 0) currentPulseCount = 0;
                ESP_LOGI(TAG, "→ MANUAL UP: Subtracted %ld pulses, count=%ld", 
                         (long)pulses, (long)currentPulseCount);
            }

            currentPulseCount = constrain(currentPulseCount, 0, 
                calibrated ? maxPulseCount : INT32_MAX);
            positionChanged = true;
            
            ESP_LOGI(TAG, "═══════════════════════════════════");
        }
        
        // ────────────────────────────────────────────────────────────
        // STOPPED: Pulse verwerfen (sollte normalerweise nicht vorkommen)
        // ────────────────────────────────────────────────────────────
        else {
            ESP_LOGW(TAG, "⚠ %ld pulses DISCARDED (state=%d, motor should be stopped)", 
                     (long)pulses, (int)currentState);
        }
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

    if (currentState == State::STOPPED && actualDirection == State::STOPPED && 
        positionChanged) {
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

        case State::MOVING_UP: {
            if (targetPulseCount != -1 && currentPulseCount <= targetPulseCount) {
                ESP_LOGI(TAG, "Reached UP target (%ld pulses). Stopping.", (long)targetPulseCount);
                triggerStop();
                targetPulseCount = -1;
                currentState = State::STOPPED;
            }
            else if (actualDirection == State::STOPPED && 
                     (millis() - motorStartTime) > MOTOR_MIN_RUN_TIME) {
                
                bool motorReallyStopped = (digitalRead(pins.motorUp) == HIGH && 
                                            digitalRead(pins.motorDown) == HIGH);
                
                if (motorReallyStopped) {
                    ESP_LOGW(TAG, "Motor stopped unexpectedly (UP)!");
                    targetPulseCount = -1;
                    currentState = State::STOPPED;
                }
            }
            break;
        }

        case State::MOVING_DOWN: {
            // Prüfe ZUERST ob Ziel erreicht
            if (targetPulseCount != -1 && currentPulseCount >= targetPulseCount) {
                ESP_LOGI(TAG, "Reached DOWN target (%ld pulses). Stopping.", (long)targetPulseCount);
                triggerStop();
                targetPulseCount = -1;
                currentState = State::STOPPED;
            }
            // Grace Period für Motor-Start
            else if (actualDirection == State::STOPPED && 
                     (millis() - motorStartTime) > MOTOR_MIN_RUN_TIME) {
                
                bool motorReallyStopped = (digitalRead(pins.motorUp) == HIGH && 
                                            digitalRead(pins.motorDown) == HIGH);
                
                if (motorReallyStopped) {
                    ESP_LOGW(TAG, "Motor stopped unexpectedly (DOWN)!");
                    targetPulseCount = -1;
                    currentState = State::STOPPED;
                }
            }
            break;
        }

        case State::CALIBRATING_UP: {
            // Debug-Output alle 200ms
            static uint32_t last_cal_up_debug = 0;
            if (millis() - last_cal_up_debug >= 200) {
                last_cal_up_debug = millis();
                ESP_LOGI(TAG, "CALIBRATING_UP: time=%lums, pulses=%ld", 
                        millis() - calibrationStartTime, 
                        (long)calibrationUpPulses);
            }
            
            // Hardware-Check ob Motor wirklich gestoppt ist
            bool motorReallyStopped = (digitalRead(pins.motorUp) == HIGH && 
                                        digitalRead(pins.motorDown) == HIGH);
            
            if (actualDirection == State::STOPPED && 
                desiredMotorAction == State::MOVING_UP &&
                motorReallyStopped) {
                
                ESP_LOGI(TAG, "");
                ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
                ESP_LOGI(TAG, "║   CALIBRATION: TOP LIMIT REACHED  ║");
                ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
                ESP_LOGI(TAG, "  UP Pulses: %ld", (long)calibrationUpPulses);
                ESP_LOGI(TAG, "");
                
                // Warte 1 Sekunde
                vTaskDelay(pdMS_TO_TICKS(1000));
                
                // Setze Position auf 0 (ganz oben)
                currentPulseCount = 0;
                positionChanged = true;
                
                // Wechsle SOFORT zu CALIBRATING_DOWN!
                currentState = State::CALIBRATING_DOWN;
                ESP_LOGI(TAG, "→ Starting CALIBRATING_DOWN");
                
                // Warte nochmal 1 Sekunde
                vTaskDelay(pdMS_TO_TICKS(1000));
                
                // Starte DOWN-Fahrt
                triggerMoveDown();
            }
            break;
        }

        case State::CALIBRATING_DOWN: {
            // Debug-Output alle 200ms
            static uint32_t last_cal_down_debug = 0;
            if (millis() - last_cal_down_debug >= 200) {
                last_cal_down_debug = millis();
                ESP_LOGI(TAG, "CALIBRATING_DOWN: time=%lums, pulses=%ld", 
                         millis() - calibrationStartTime, 
                         (long)calibrationDownPulses);
            }
            
            bool motorReallyStopped = (digitalRead(pins.motorUp) == HIGH && 
                                   digitalRead(pins.motorDown) == HIGH);
            
            if (actualDirection == State::STOPPED && 
                desiredMotorAction == State::MOVING_DOWN &&
                motorReallyStopped) {
                
                ESP_LOGI(TAG, "");
                ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
                ESP_LOGI(TAG, "║  CALIBRATION: BOTTOM LIMIT REACHED║");
                ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
                ESP_LOGI(TAG, "  DOWN Pulses: %ld", (long)calibrationDownPulses);
                ESP_LOGI(TAG, "");
                
                currentState = State::CALIBRATING_VALIDATION;
            }
            break;
        }

        case State::CALIBRATING_VALIDATION: {
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
            ESP_LOGI(TAG, "║   CALIBRATION VALIDATION          ║");
            ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "UP Pulses:   %ld", (long)calibrationUpPulses);
            ESP_LOGI(TAG, "DOWN Pulses: %ld", (long)calibrationDownPulses);
            ESP_LOGI(TAG, "");
            
            // Berechne Differenz
            int32_t diff = abs(calibrationUpPulses - calibrationDownPulses);
            float diffPercent = (float)diff / calibrationUpPulses * 100.0f;
            
            ESP_LOGI(TAG, "Difference: %ld pulses (%.2f%%)", (long)diff, diffPercent);
            ESP_LOGI(TAG, "");
            
            // Validierung: Max 5% Abweichung
            if (diffPercent <= DRIFT_CORRECTION_THRESHOLD) {
                // Verwende Durchschnitt
                maxPulseCount = (calibrationUpPulses + calibrationDownPulses) / 2;
                currentPulseCount = maxPulseCount;  // Aktuell ganz unten
                
                ESP_LOGI(TAG, "✓ VALIDATION PASSED!");
                ESP_LOGI(TAG, "  Using average: %ld pulses", (long)maxPulseCount);
                ESP_LOGI(TAG, "");
                
                calibrated = true;
                saveState();
                currentState = State::STOPPED;
                
                ESP_LOGI(TAG, "✓ Calibration complete!");
                ESP_LOGI(TAG, "");
                if (_calibrationCompleteCallback) {
                    ESP_LOGI(TAG, "→ Calling calibration complete callback (success)");
                    _calibrationCompleteCallback(true);
                }
                
            } else {
                // ❌ INVALID: Abweichung zu groß!
                ESP_LOGE(TAG, "✗ VALIDATION FAILED!");
                ESP_LOGE(TAG, "  Difference too large: %.2f%% (max 5%%)", diffPercent);
                ESP_LOGE(TAG, "");
                ESP_LOGE(TAG, "Possible causes:");
                ESP_LOGE(TAG, "  • Belt slipping");
                ESP_LOGE(TAG, "  • Sensor malfunction");
                ESP_LOGE(TAG, "  • Mechanical issue");
                ESP_LOGE(TAG, "");
                ESP_LOGE(TAG, "→ Please run calibration again!");
                ESP_LOGE(TAG, "");
                
                calibrated = false;
                currentState = State::STOPPED;

                if (_calibrationCompleteCallback) {
                    ESP_LOGI(TAG, "→ Calling calibration complete callback (failed)");
                    _calibrationCompleteCallback(false);
                }
            }
            
            // Reset Kalibrierungs-Variablen
            calibrationUpPulses = 0;
            calibrationDownPulses = 0;
            
            break;
        }
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
        if (action == State::MOVING_UP) {
            motorStartTime = millis();
            triggerMoveUp();
        }
        else if (action == State::MOVING_DOWN) {
            motorStartTime = millis();
            triggerMoveDown();
        }
        else if (desiredMotorAction != State::STOPPED) {
            triggerStop();
        }

        desiredMotorAction = action;
    }
}

void RollerShutter::triggerMoveUp() {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   triggerMoveUp() CALLED          ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "  directionInverted:      %s", directionInverted ? "YES" : "NO");
    ESP_LOGI(TAG, "  Actual button pin:      GPIO %d", directionInverted ? pins.buttonDown : pins.buttonUp);
    ESP_LOGI(TAG, "  Button name:            %s", directionInverted ? "DOWN (inverted!)" : "UP");
    ESP_LOGI(TAG, "");
    
    uint8_t pin = directionInverted ? pins.buttonDown : pins.buttonUp;
    startButtonPress(pin);
}



void RollerShutter::triggerMoveDown() {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   triggerMoveDown() CALLED        ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "  directionInverted:      %s", directionInverted ? "YES" : "NO");
    ESP_LOGI(TAG, "  Actual button pin:      GPIO %d", directionInverted ? pins.buttonUp : pins.buttonDown);
    ESP_LOGI(TAG, "  Button name:            %s", directionInverted ? "UP (inverted!)" : "DOWN");
    ESP_LOGI(TAG, "");
    
    uint8_t pin = directionInverted ? pins.buttonUp : pins.buttonDown;
    startButtonPress(pin);
}

void RollerShutter::triggerStop() {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   triggerStop() CALLED            ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "  Current State:      %d", (int)currentState);
    ESP_LOGI(TAG, "  actualDirection:    %d", (int)actualDirection);
    ESP_LOGI(TAG, "  desiredMotorAction: %d", (int)desiredMotorAction);
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════
    // Bestimme welche Taste gedrückt werden muss (basierend auf AKTUELLER Bewegung)
    // ════════════════════════════════════════════════════════════════
    
    uint8_t pin = 0;
    
    // Verwende actualDirection (Hardware-Status), NICHT currentState!
    if (actualDirection == State::MOVING_UP) {
        // Motor läuft UP → UP-Taste drücken
        pin = directionInverted ? pins.buttonDown : pins.buttonUp;
        ESP_LOGI(TAG, "→ Motor running UP, pressing %s button to stop", 
                 directionInverted ? "DOWN (inverted)" : "UP");
        
    } else if (actualDirection == State::MOVING_DOWN) {
        // Motor läuft DOWN → DOWN-Taste drücken
        pin = directionInverted ? pins.buttonUp : pins.buttonDown;
        ESP_LOGI(TAG, "→ Motor running DOWN, pressing %s button to stop", 
                 directionInverted ? "UP (inverted)" : "DOWN");
        
    } else {
        // Motor steht bereits → Nichts tun
        ESP_LOGI(TAG, "⚠ Motor already stopped (actualDirection = STOPPED)");
        ESP_LOGI(TAG, "  → No button press needed");
        ESP_LOGI(TAG, "");
        return;
    }
    
    ESP_LOGI(TAG, "  Selected pin: GPIO%d", pin);
    ESP_LOGI(TAG, "");
    
    // ════════════════════════════════════════════════════════════════
    // Button-Press mit Priorität
    // ════════════════════════════════════════════════════════════════
    
    // Stop hat PRIORITÄT - überschreibe buttonPostReleaseWait!
    if (buttonActive || buttonPostReleaseWait) {
        ESP_LOGI(TAG, "→ Cancelling active button operation (STOP has priority)");
        
        // Aktuellen Button sofort freigeben
        if (buttonActive) {
            digitalWrite(activeButtonPin, HIGH);
            buttonActive = false;
        }
        buttonPostReleaseWait = false;
    }
    
    // Starte Stop-Button-Press
    startButtonPress(pin);
    
    ESP_LOGI(TAG, "✓ Stop button press initiated (GPIO%d)", pin);
    ESP_LOGI(TAG, "");
}

void RollerShutter::startButtonPress(uint8_t pin) {
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "startButtonPress() CALLED");
    ESP_LOGD(TAG, "  pin: %d", pin);
    ESP_LOGD(TAG, "  buttonActive: %s", buttonActive ? "YES" : "NO");
    ESP_LOGD(TAG, "  buttonPostReleaseWait: %s", buttonPostReleaseWait ? "YES" : "NO");
    
    if (buttonActive || buttonPostReleaseWait) {
        ESP_LOGW(TAG, "  → REJECTED! (button in use or cooling down)");
        ESP_LOGI(TAG, "═══════════════════════════════════");
        return;
    }
    
    digitalWrite(pin, LOW);
    buttonPressStart = millis();
    buttonActive = true;
    activeButtonPin = pin;
    
    ESP_LOGI(TAG, "  → Button pressed! (pin %d set to LOW)", pin);
    ESP_LOGI(TAG, "═══════════════════════════════════");
}


void RollerShutter::handleButtonRelease() {
    // Phase 1: Release Button
    if (buttonActive && !buttonPostReleaseWait && 
        (millis() - buttonPressStart >= BUTTON_PRESS_DURATION)) {
        
        digitalWrite(activeButtonPin, HIGH);
        buttonActive = false;
        buttonReleaseTime = millis();
        buttonPostReleaseWait = true;
        ESP_LOGD(TAG, "Button released: pin %d (cooling down 500ms)", activeButtonPin);
        return;
    }
    
    // Phase 2: Cooldown Period
    if (buttonPostReleaseWait && 
        (millis() - buttonReleaseTime >= BUTTON_POST_RELEASE_DELAY)) {
        buttonPostReleaseWait = false;
        ESP_LOGD(TAG, "Button cooldown complete - ready for next press");
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
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   INITIALIZING HARDWARE           ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    isr_ready = false;

    // ISR entfernen (falls vorhanden)
    gpio_isr_handler_remove((gpio_num_t)pins.pulseCounter);
    detachInterrupt(digitalPinToInterrupt(pins.pulseCounter));

    // Pins konfigurieren
    ESP_LOGI(TAG, "Configuring GPIO pins:");
    ESP_LOGI(TAG, "  Pulse Counter: GPIO%d (INPUT_PULLUP)", pins.pulseCounter);
    pinMode(pins.pulseCounter, INPUT_PULLUP);
    
    ESP_LOGI(TAG, "  Motor UP:      GPIO%d (INPUT_PULLUP)", pins.motorUp);
    pinMode(pins.motorUp, INPUT_PULLUP);
    
    ESP_LOGI(TAG, "  Motor DOWN:    GPIO%d (INPUT_PULLUP)", pins.motorDown);
    pinMode(pins.motorDown, INPUT_PULLUP);
    
    ESP_LOGI(TAG, "  Button UP:     GPIO%d (OUTPUT, HIGH)", pins.buttonUp);
    pinMode(pins.buttonUp, OUTPUT);
    digitalWrite(pins.buttonUp, HIGH);
    
    ESP_LOGI(TAG, "  Button DOWN:   GPIO%d (OUTPUT, HIGH)", pins.buttonDown);
    pinMode(pins.buttonDown, OUTPUT);
    digitalWrite(pins.buttonDown, HIGH);
    
    ESP_LOGI(TAG, "");

    // Pin-Status nach Konfiguration prüfen
    ESP_LOGD(TAG, "Pin Status after configuration:");
    ESP_LOGD(TAG, "  Pulse Counter (GPIO%d): %d", pins.pulseCounter, digitalRead(pins.pulseCounter));
    ESP_LOGD(TAG, "  Motor UP (GPIO%d):      %d", pins.motorUp, digitalRead(pins.motorUp));
    ESP_LOGD(TAG, "  Motor DOWN (GPIO%d):    %d", pins.motorDown, digitalRead(pins.motorDown));
    ESP_LOGD(TAG, "  Button UP (GPIO%d):     %d", pins.buttonUp, digitalRead(pins.buttonUp));
    ESP_LOGD(TAG, "  Button DOWN (GPIO%d):   %d", pins.buttonDown, digitalRead(pins.buttonDown));
    ESP_LOGD(TAG, "");

    // Reset pulse buffer
    portENTER_CRITICAL(&pulseMux);
    pulseBuffer = 0;
    portEXIT_CRITICAL(&pulseMux);

    // Reset ISR counters
    isr_trigger_count = 0;
    isr_rejected_count = 0;
    isr_pulse_count = 0;

    // Install ISR service
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "✗ Failed to install ISR service: %d", err);
        return;
    }
    ESP_LOGI(TAG, "✓ ISR service installed");

    hardware_initialized_local = true;
    ESP_LOGI(TAG, "✓ hardware_initialized_local = true");

    // Attach interrupt
    ESP_LOGI(TAG, "Attaching interrupt to GPIO%d (FALLING edge)...", pins.pulseCounter);
    attachInterrupt(digitalPinToInterrupt(pins.pulseCounter), 
                    RollerShutter::onPulseInterrupt, FALLING);
    ESP_LOGI(TAG, "✓ Interrupt attached");

    // Activate ISR
    isr_ready = true;
    ESP_LOGI(TAG, "✓ isr_ready = true");
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "✓ Hardware initialization complete");
    ESP_LOGI(TAG, "");
    
    // Kurz warten und dann ISR testen
    vTaskDelay(pdMS_TO_TICKS(500));
    
    ESP_LOGD(TAG, "ISR Status after 500ms:");
    ESP_LOGD(TAG, "  isr_trigger_count:  %lu", (unsigned long)isr_trigger_count);
    ESP_LOGD(TAG, "  isr_rejected_count: %lu", (unsigned long)isr_rejected_count);
    ESP_LOGD(TAG, "  isr_pulse_count:    %lu", (unsigned long)isr_pulse_count);
    ESP_LOGD(TAG, "");
    
    if (isr_trigger_count == 0) {
        ESP_LOGW(TAG, "⚠️  WARNING: ISR was NOT triggered yet!");
        ESP_LOGW(TAG, "   This is normal if motor is not running.");
    }
}
                
void RollerShutter::recordTopLimit() {
    // Speichere aktuelle Pulse-Position wenn ganz oben
    topLimitHistory[topLimitHistoryIndex] = currentPulseCount;
    topLimitHistoryIndex = (topLimitHistoryIndex + 1) % DRIFT_HISTORY_SIZE;
    
    ESP_LOGD(TAG, "Recorded top limit: %ld pulses", (long)currentPulseCount);
    
    // Prüfe ob wir genug Daten haben
    checkAndAdjustMaxPulseCount();
}

void RollerShutter::recordBottomLimit() {
    // Speichere aktuelle Pulse-Position wenn ganz unten
    bottomLimitHistory[bottomLimitHistoryIndex] = currentPulseCount;
    bottomLimitHistoryIndex = (bottomLimitHistoryIndex + 1) % DRIFT_HISTORY_SIZE;
    
    fullCycleCount++;
    
    ESP_LOGD(TAG, "Recorded bottom limit: %ld pulses (cycle %d)", 
             (long)currentPulseCount, fullCycleCount);
    
    // Prüfe ob wir genug Daten haben
    checkAndAdjustMaxPulseCount();
}

void RollerShutter::checkAndAdjustMaxPulseCount() {
    // Erst nach mindestens 20 vollständigen Zyklen
    if (fullCycleCount < 20) {
        return;
    }
    
    // Berechne Durchschnitt der letzten 20 Bottom-Limits
    int64_t sum = 0;
    int validCount = 0;
    
    for (int i = 0; i < DRIFT_HISTORY_SIZE; i++) {
        if (bottomLimitHistory[i] > 0) {
            sum += bottomLimitHistory[i];
            validCount++;
        }
    }
    
    if (validCount < 10) {
        return;  // Zu wenig Daten
    }
    
    int32_t average = sum / validCount;
    
    // Berechne Abweichung vom aktuellen maxPulseCount
    int32_t diff = abs(average - maxPulseCount);
    float diffPercent = (float)diff / maxPulseCount * 100.0f;
    
    ESP_LOGD(TAG, "");
    ESP_LOGD(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGD(TAG, "║   DRIFT CORRECTION CHECK          ║");
    ESP_LOGD(TAG, "╚═══════════════════════════════════╝");
    ESP_LOGD(TAG, "");
    ESP_LOGD(TAG, "Full cycles completed: %d", fullCycleCount);
    ESP_LOGD(TAG, "Valid measurements: %d", validCount);
    ESP_LOGD(TAG, "Current maxPulseCount: %ld", (long)maxPulseCount);
    ESP_LOGD(TAG, "Measured average:      %ld", (long)average);
    ESP_LOGD(TAG, "Difference:            %ld pulses (%.2f%%)", (long)diff, diffPercent);
    ESP_LOGD(TAG, "");
    
    // Wenn Abweichung > 5% → Drift erkannt!
    if (diffPercent > DRIFT_CORRECTION_THRESHOLD) {
        ESP_LOGW(TAG, "⚠ DRIFT DETECTED!");
        ESP_LOGW(TAG, "  Adjusting maxPulseCount: %ld → %ld", 
                 (long)maxPulseCount, (long)average);
        ESP_LOGW(TAG, "");
        
        maxPulseCount = average;
        saveState();
        
        // Reset History und Counter
        fullCycleCount = 0;
        topLimitHistoryIndex = 0;
        bottomLimitHistoryIndex = 0;
        memset(topLimitHistory, 0, sizeof(topLimitHistory));
        memset(bottomLimitHistory, 0, sizeof(bottomLimitHistory));
        
        ESP_LOGI(TAG, "✓ maxPulseCount updated and saved");
        ESP_LOGI(TAG, "  History reset, starting new measurement cycle");
        ESP_LOGI(TAG, "");
        
    } else if (diffPercent > DRIFT_WARNING_THRESHOLD) {
        ESP_LOGI(TAG, "ℹ Minor drift detected (%.2f%%), monitoring...", diffPercent);
        ESP_LOGI(TAG, "  Will auto-correct if drift exceeds 5%%");
        ESP_LOGI(TAG, "");
        
    } else {
        ESP_LOGI(TAG, "✓ No significant drift detected");
        ESP_LOGI(TAG, "  System is stable");
        ESP_LOGI(TAG, "");
        
        // Reset Counter nach erfolgreicher Validierung
        fullCycleCount = 0;
    }
}

void RollerShutter::resetDriftHistory() {
    ESP_LOGI(TAG, "Resetting drift history...");
    
    fullCycleCount = 0;
    topLimitHistoryIndex = 0;
    bottomLimitHistoryIndex = 0;
    
    memset(topLimitHistory, 0, sizeof(topLimitHistory));
    memset(bottomLimitHistory, 0, sizeof(bottomLimitHistory));
    
    saveState();
    
    ESP_LOGI(TAG, "✓ Drift history reset");
}

void RollerShutter::periodicSave() {
    static int32_t lastSavedPulseCount = 0;
    static uint32_t lastSaveTime = 0;
    
    // Speichere nur während Bewegung
    if (currentState != State::MOVING_UP && 
        currentState != State::MOVING_DOWN) {
        return;
    }
    
    // Rate-Limiting: Max 1x pro Sekunde
    if (millis() - lastSaveTime < 1000) {
        return;
    }
    
    // Speichere alle 5 Pulse
    int32_t pulseDelta = abs(currentPulseCount - lastSavedPulseCount);
    
    if (pulseDelta >= 5) {
        ESP_LOGV(TAG, "Periodic save triggered: %ld pulses", (long)currentPulseCount);
        
        saveStateToKVS();
        
        lastSavedPulseCount = currentPulseCount;
        lastSaveTime = millis();
    }
}

// ════════════════════════════════════════════════════════════════════
// Smart Matter Update Strategy
// ════════════════════════════════════════════════════════════════════

bool RollerShutter::shouldSendMatterUpdate() const {
    // ────────────────────────────────────────────────────────────────
    // 1. Keine Updates während Kalibrierung
    // ────────────────────────────────────────────────────────────────
    if (currentState == State::CALIBRATING_UP || 
        currentState == State::CALIBRATING_DOWN ||
        currentState == State::CALIBRATING_VALIDATION) {
        return false;
    }
    
    // ────────────────────────────────────────────────────────────────
    // 2. Aktuelle Position berechnen
    // ────────────────────────────────────────────────────────────────
    uint8_t currentPercent = getCurrentPercent();
    
    // ────────────────────────────────────────────────────────────────
    // 3. CASE A: Matter-initiierte Bewegung
    //    → Live-Updates mit Rate-Limiting + Hysterese
    // ────────────────────────────────────────────────────────────────
    if (targetPulseCount != -1 && 
        (currentState == State::MOVING_UP || currentState == State::MOVING_DOWN)) {
        
        // Rate-Limiting: Max 1x pro Sekunde
        if (millis() - lastMatterUpdateTime < MATTER_UPDATE_INTERVAL_MS) {
            return false;
        }
        
        // Hysterese: Min 2% Änderung
        if (lastReportedPercentForMatter != 255) {  // Nicht beim ersten Mal
            uint8_t delta = abs(currentPercent - lastReportedPercentForMatter);
            if (delta < MATTER_UPDATE_HYSTERESIS) {
                return false;
            }
        }
        
        // ✅ Sende Live-Update
        ESP_LOGD(TAG, "→ Live-Update: %d%% (Matter-initiated movement)", currentPercent);
        return true;
    }
    
    // ────────────────────────────────────────────────────────────────
    // 4. CASE B: Manuelle Bewegung
    //    → NUR bei Stillstand senden (Feedback-Loop-Safe)
    // ────────────────────────────────────────────────────────────────
    if (currentState == State::STOPPED && 
        actualDirection == State::STOPPED &&
        positionChanged) {
        
        // Hysterese: Min 1% Änderung (bei manuell weniger streng)
        if (lastReportedPercentForMatter != 255) {
            uint8_t delta = abs(currentPercent - lastReportedPercentForMatter);
            if (delta < 1) {
                return false;
            }
        }
        
        // ✅ Sende Update bei Stillstand
        ESP_LOGD(TAG, "→ Stopped-Update: %d%% (manual movement completed)", currentPercent);
        return true;
    }
    
    // ────────────────────────────────────────────────────────────────
    // 5. CASE C: Position hat sich geändert, aber Motor läuft noch manuell
    //    → KEINE Updates (warte auf Stillstand)
    // ────────────────────────────────────────────────────────────────
    if (currentState == State::STOPPED && 
        actualDirection != State::STOPPED &&
        positionChanged) {
        ESP_LOGV(TAG, "→ Manual movement in progress - skipping update");
        return false;
    }
    
    // ────────────────────────────────────────────────────────────────
    // 6. Sonst: Keine Updates
    // ────────────────────────────────────────────────────────────────
    return false;
}

void RollerShutter::markMatterUpdateSent() {
    lastMatterUpdateTime = millis();
    lastReportedPercentForMatter = getCurrentPercent();
    positionChanged = false;  // Reset Flag
}

