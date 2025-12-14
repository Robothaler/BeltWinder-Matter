#pragma once

#include <Arduino.h>
#include "config.h"
#include <platform/KeyValueStoreManager.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

class RollerShutter {
public:
    enum class State { STOPPED, MOVING_UP, MOVING_DOWN, CALIBRATING_UP, CALIBRATING_DOWN };

    RollerShutter();
    void initHardware();
    void loadStateFromKVS();
    void begin() { /* legacy */ }
    void loop();

    void moveToPercent(uint8_t percent);
    void stop();
    void startCalibration();
    void setDirectionInverted(bool inverted);

    uint8_t getCurrentPercent() const;
    bool isCalibrated() const;
    bool isDirectionInverted() const;
    State getCurrentState() const;
    bool hasPositionChanged();

    void setWindowState(bool isOpen);
    void setWindowOpenLogic(WindowOpenLogic logic);
    
    // ISR-Thread-Safety
    static portMUX_TYPE pulseMux;
    static volatile int32_t pulseBuffer;
    static volatile bool isr_ready;

private:
    void handleStateMachine();
    void handleInputs();
    void applyMotorAction();
    void startButtonPress(uint8_t pin);
    void handleButtonRelease();
    void saveStateToKVS();
    void saveState();

    void triggerMoveUp();
    void triggerMoveDown();
    void triggerStop();

    State currentState = State::STOPPED;
    State actualDirection = State::STOPPED;
    State desiredMotorAction = State::STOPPED;

    int32_t currentPulseCount = 0;
    int32_t targetPulseCount = -1;
    int32_t maxPulseCount = 0;
    uint8_t lastReportedPercent = 255;

    unsigned long buttonPressStart = 0;
    const unsigned long BUTTON_PRESS_DURATION = 300;
    bool buttonActive = false;
    uint8_t activeButtonPin = 255;

    bool hardware_initialized_local = false;
    bool calibrated = false;
    bool directionInverted = false;
    bool positionChanged = true;
    bool windowIsOpen = false;
    WindowOpenLogic windowLogic = DEFAULT_WINDOW_LOGIC;

    unsigned long calibrationStartTime = 0;
    const unsigned long CALIBRATION_TIMEOUT = 90000;

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
