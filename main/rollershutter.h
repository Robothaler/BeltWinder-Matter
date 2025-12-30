// rollershutter.h

#ifndef ROLLERSHUTTER_H
#define ROLLERSHUTTER_H

#pragma once

#include <Arduino.h>
#include "config.h"
#include <platform/KeyValueStoreManager.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <ArduinoJson.h>

class RollerShutter {
public:
    enum class State : uint8_t {
    STOPPED,
    MOVING_UP,
    MOVING_DOWN,
    CALIBRATING_UP,
    CALIBRATING_DOWN,
    CALIBRATING_VALIDATION
};

    RollerShutter();
    void initHardware();
    void loadStateFromKVS();
    void begin() { /* legacy */ }
    void loop();

    void moveToPercent(uint8_t percent);
    void stop();
    void startCalibration();
    void setDirectionInverted(bool inverted);

    void recordTopLimit();
    void recordBottomLimit();
    void resetDriftHistory();

    uint8_t getCurrentPercent() const;
    bool isCalibrated() const;
    bool isDirectionInverted() const;
    State getCurrentState() const;
    bool hasPositionChanged();

    void setWindowState(bool isOpen);
    void setWindowOpenLogic(WindowOpenLogic logic);

    // ════════════════════════════════════════════════════════════════
    // Intelligente Update-Strategie
    // ════════════════════════════════════════════════════════════════
    
    bool shouldSendMatterUpdate() const;
    void markMatterUpdateSent();

    typedef void (*CalibrationCompleteCallback)(bool success);
    
    void setCalibrationCompleteCallback(CalibrationCompleteCallback cb) {
        _calibrationCompleteCallback = cb;
    }
    
    // ISR-Thread-Safety
    static portMUX_TYPE pulseMux;
    static volatile int32_t pulseBuffer;
    static volatile bool isr_ready;

    int32_t getMaxPulseCount() const { return maxPulseCount; }
    uint8_t getFullCycleCount() const { return fullCycleCount; }
    
    int32_t calculateCurrentAverage() const {
        int64_t sum = 0;
        int validCount = 0;
        
        for (int i = 0; i < DRIFT_HISTORY_SIZE; i++) {
            if (bottomLimitHistory[i] > 0) {
                sum += bottomLimitHistory[i];
                validCount++;
            }
        }
        
        if (validCount == 0) return maxPulseCount;
        return sum / validCount;
    }
    
    // Drift-Statistiken als JSON
    String getDriftStatisticsJson() const {
        StaticJsonDocument<512> doc;
        
        doc["calibrated"] = calibrated;
        doc["maxPulseCount"] = maxPulseCount;
        doc["currentPulseCount"] = currentPulseCount;
        doc["fullCycleCount"] = fullCycleCount;
        
        int32_t avg = calculateCurrentAverage();
        doc["measuredAverage"] = avg;
        
        if (maxPulseCount > 0) {
            int32_t diff = abs(avg - maxPulseCount);
            float diffPercent = (float)diff / maxPulseCount * 100.0f;
            doc["driftPercent"] = diffPercent;
            doc["driftPulses"] = diff;
        }
        
        // History
        JsonArray topHistory = doc.createNestedArray("topHistory");
        for (int i = 0; i < DRIFT_HISTORY_SIZE; i++) {
            if (topLimitHistory[i] > 0) {
                topHistory.add(topLimitHistory[i]);
            }
        }
        
        JsonArray bottomHistory = doc.createNestedArray("bottomHistory");
        for (int i = 0; i < DRIFT_HISTORY_SIZE; i++) {
            if (bottomLimitHistory[i] > 0) {
                bottomHistory.add(bottomLimitHistory[i]);
            }
        }
        
        String output;
        serializeJson(doc, output);
        return output;
    }

private:
    void handleStateMachine();
    void handleInputs();
    void applyMotorAction();
    void startButtonPress(uint8_t pin);
    void handleButtonRelease();
    void saveStateToKVS();
    void saveState();
    void periodicSave();

    void triggerMoveUp();
    void triggerMoveDown();
    void triggerStop();

    void checkAndAdjustMaxPulseCount();

    State currentState = State::STOPPED;
    State actualDirection = State::STOPPED;
    State desiredMotorAction = State::STOPPED;

    int32_t currentPulseCount = 0;
    int32_t targetPulseCount = -1;
    int32_t maxPulseCount = 0;
    uint8_t lastReportedPercent = 255;

    static volatile uint32_t isr_trigger_count;
    static volatile uint32_t isr_rejected_count;
    static volatile uint32_t isr_pulse_count; 

    unsigned long buttonPressStart = 0;
    const unsigned long BUTTON_PRESS_DURATION = 300;
    bool buttonActive = false;
    uint8_t activeButtonPin = 255;
    uint32_t buttonReleaseTime = 0;
    bool buttonPostReleaseWait = false;
    static const uint32_t BUTTON_POST_RELEASE_DELAY = 500;  
    uint32_t motorStartTime = 0;
    static const uint32_t MOTOR_MIN_RUN_TIME = 1000;

    // Kalibrierungs-Validierung
    int32_t calibrationUpPulses = 0;
    int32_t calibrationDownPulses = 0;
    static constexpr float CALIBRATION_MAX_DIFF_PERCENT = 3.0f;  // Max 3% Abweichung
    CalibrationCompleteCallback _calibrationCompleteCallback = nullptr;
    
    // Drift-Korrektur
    static const uint8_t DRIFT_HISTORY_SIZE = 10;         // Letzte 10 Messungen
    static constexpr uint8_t DRIFT_MIN_CYCLES = 10;       // Mindestens 10 Zyklen
    static constexpr float DRIFT_WARNING_THRESHOLD = 3.0f;  // Warnung ab 3%
    static constexpr float DRIFT_CORRECTION_THRESHOLD = 10.0f;  // Korrektur ab 10%
    int32_t topLimitHistory[DRIFT_HISTORY_SIZE];
    int32_t bottomLimitHistory[DRIFT_HISTORY_SIZE];
    uint8_t topLimitHistoryIndex = 0;
    uint8_t bottomLimitHistoryIndex = 0;
    uint8_t fullCycleCount = 0;

    // ════════════════════════════════════════════════════════════════
    // Smart Update Strategy
    // ════════════════════════════════════════════════════════════════
    
    uint32_t lastMatterUpdateTime = 0;
    uint8_t lastReportedPercentForMatter = 255;  // 255 = ungültig
    
    static const uint32_t MATTER_UPDATE_INTERVAL_MS = 500;  // Max 1x/500ms
    static const uint8_t MATTER_UPDATE_HYSTERESIS = 2;       // Min 1% Änderung


    bool hardware_initialized_local = false;
    bool calibrated = false;
    bool directionInverted = false;
    bool positionChanged = true;
    bool windowIsOpen = false;
    WindowOpenLogic windowLogic = DEFAULT_WINDOW_LOGIC;

    unsigned long calibrationStartTime = 0;
    const unsigned long CALIBRATION_TIMEOUT = 90000; // 90 Sekunden

    State lastActualDirection = State::STOPPED;
    uint8_t directionStableCounter = 0;
    static constexpr uint8_t DIRECTION_STABILITY_THRESHOLD = 3; // 3 Samples

    struct Pins {
        uint8_t pulseCounter;
        uint8_t motorUp;
        uint8_t motorDown;
        uint8_t buttonUp;
        uint8_t buttonDown;
    } pins;

    static void IRAM_ATTR onPulseInterrupt();
};

#endif // ROLLERSHUTTER_H