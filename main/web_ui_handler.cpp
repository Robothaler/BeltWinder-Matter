#include <Arduino.h>
#include <WiFi.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_chip_info.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_flash.h>
#include <esp_mac.h>
#include <esp_task_wdt.h>
#include <string.h>
#include <app/server/Server.h>
#include <Matter.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "web_ui_handler.h"

static const char* TAG = "WebUI";

static const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>BeltWinder Matter</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, 'Inter', sans-serif;
      background: #0a0a0a;
      color: #e8e8e8;
      line-height: 1.6;
      overflow-x: hidden;
      position: relative;
    }
    
    /* Animated Background */
    body::before {
      content: '';
      position: fixed;
      top: 0;
      left: 0;
      width: 100%;
      height: 100%;
      background: 
        radial-gradient(circle at 20% 50%, rgba(33, 150, 243, 0.08) 0%, transparent 50%),
        radial-gradient(circle at 80% 80%, rgba(76, 175, 80, 0.06) 0%, transparent 50%),
        radial-gradient(circle at 40% 20%, rgba(156, 39, 176, 0.05) 0%, transparent 50%);
      pointer-events: none;
      z-index: 0;
      animation: bgShift 20s ease infinite;
    }
    
    @keyframes bgShift {
      0%, 100% { opacity: 1; transform: scale(1); }
      50% { opacity: 0.8; transform: scale(1.1); }
    }
    
    .container { 
      max-width: 1000px; 
      margin: 0 auto; 
      padding: 20px;
      position: relative;
      z-index: 1;
    }
    
    /* Header with Glassmorphism */
    .header {
      text-align: center;
      margin-bottom: 30px;
      padding: 40px 30px;
      background: rgba(255, 255, 255, 0.03);
      backdrop-filter: blur(20px);
      border-radius: 24px;
      border: 1px solid rgba(255, 255, 255, 0.08);
      box-shadow: 
        0 8px 32px rgba(0, 0, 0, 0.3),
        inset 0 1px 0 rgba(255, 255, 255, 0.1);
      position: relative;
      overflow: hidden;
    }
    
    .header::before {
      content: '';
      position: absolute;
      top: -50%;
      left: -50%;
      width: 200%;
      height: 200%;
      background: linear-gradient(
        45deg,
        transparent,
        rgba(33, 150, 243, 0.1),
        transparent
      );
      animation: headerShine 3s ease-in-out infinite;
    }
    
    @keyframes headerShine {
      0% { transform: translateX(-100%) translateY(-100%) rotate(45deg); }
      100% { transform: translateX(100%) translateY(100%) rotate(45deg); }
    }
    
    .header h1 { 
      font-size: 2.5em;
      font-weight: 700;
      background: linear-gradient(135deg, #2196F3 0%, #21CBF3 50%, #4CAF50 100%);
      -webkit-background-clip: text;
      -webkit-text-fill-color: transparent;
      background-clip: text;
      margin-bottom: 10px;
      position: relative;
      z-index: 1;
    }
    
    .header .subtitle {
      font-size: 0.95em;
      color: #888;
      position: relative;
      z-index: 1;
    }
    
    /* Modern Navigation */
    .nav {
      display: flex;
      justify-content: center;
      background: rgba(255, 255, 255, 0.03);
      backdrop-filter: blur(20px);
      padding: 8px;
      border-radius: 16px;
      margin-bottom: 30px;
      overflow-x: auto;
      gap: 8px;
      border: 1px solid rgba(255, 255, 255, 0.05);
      scrollbar-width: none;
    }
    
    .nav::-webkit-scrollbar { display: none; }
    
    .nav button {
      background: transparent;
      border: none;
      color: #888;
      font-size: 0.9em;
      padding: 12px 20px;
      cursor: pointer;
      transition: all .3s cubic-bezier(0.4, 0, 0.2, 1);
      white-space: nowrap;
      border-radius: 12px;
      font-weight: 500;
      position: relative;
    }
    
    .nav button::before {
      content: '';
      position: absolute;
      inset: 0;
      border-radius: 12px;
      background: linear-gradient(135deg, #2196F3, #21CBF3);
      opacity: 0;
      transition: opacity .3s;
    }
    
    .nav button span {
      position: relative;
      z-index: 1;
    }
    
    .nav button:hover {
      color: #2196F3;
      transform: translateY(-2px);
    }
    
    .nav button.active {
      color: white;
      background: rgba(33, 150, 243, 0.2);
    }
    
    .nav button.active::before {
      opacity: 0.15;
    }
    
    /* Card Design */
    .card {
      background: rgba(255, 255, 255, 0.03);
      backdrop-filter: blur(20px);
      border-radius: 20px;
      padding: 30px;
      margin-bottom: 25px;
      border: 1px solid rgba(255, 255, 255, 0.08);
      box-shadow: 0 8px 32px rgba(0, 0, 0, 0.3);
      transition: transform .3s, box-shadow .3s;
    }
    
    .card:hover {
      transform: translateY(-4px);
      box-shadow: 0 12px 48px rgba(0, 0, 0, 0.4);
    }
    
    .card h2 { 
      font-size: 1.4em;
      margin-bottom: 20px;
      background: linear-gradient(135deg, #2196F3, #21CBF3);
      -webkit-background-clip: text;
      -webkit-text-fill-color: transparent;
      background-clip: text;
      font-weight: 600;
    }
    
    /* Status Grid with Animation */
    .status-grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(140px, 1fr));
      gap: 15px;
      margin-bottom: 25px;
    }
    
    .status-item {
      background: rgba(255, 255, 255, 0.04);
      padding: 20px 16px;
      border-radius: 16px;
      text-align: center;
      border: 1px solid rgba(255, 255, 255, 0.05);
      transition: all .3s;
      position: relative;
      overflow: hidden;
    }
    
    .status-item::before {
      content: '';
      position: absolute;
      top: 0;
      left: -100%;
      width: 100%;
      height: 100%;
      background: linear-gradient(90deg, transparent, rgba(255,255,255,0.05), transparent);
      transition: left .5s;
    }
    
    .status-item:hover::before {
      left: 100%;
    }
    
    .status-item:hover {
      background: rgba(255, 255, 255, 0.06);
      transform: translateY(-2px);
    }
    
    .status-label { 
      font-size: 0.75em;
      color: #888;
      text-transform: uppercase;
      letter-spacing: 1px;
      font-weight: 600;
    }
    
    .status-value { 
      font-size: 1.6em;
      font-weight: 700;
      margin-top: 8px;
      transition: all .3s;
    }
    
    .status-value.commissioned { 
      color: #4CAF50;
      text-shadow: 0 0 20px rgba(76, 175, 80, 0.5);
    }
    
    .status-value.not-commissioned { 
      color: #FF9800;
      text-shadow: 0 0 20px rgba(255, 152, 0, 0.5);
    }
    
    /* Modern Buttons */
    .btn-grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(120px, 1fr));
      gap: 15px;
      margin-top: 25px;
    }
    
    .btn {
      background: linear-gradient(135deg, rgba(76, 175, 80, 0.2), rgba(56, 142, 60, 0.3));
      color: white;
      border: 1px solid rgba(76, 175, 80, 0.3);
      padding: 18px 12px;
      font-size: 1em;
      border-radius: 14px;
      cursor: pointer;
      transition: all .3s cubic-bezier(0.4, 0, 0.2, 1);
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 8px;
      font-weight: 600;
      position: relative;
      overflow: hidden;
    }
    
    .btn::before {
      content: '';
      position: absolute;
      inset: 0;
      background: linear-gradient(135deg, #4CAF50, #66BB6A);
      opacity: 0;
      transition: opacity .3s;
    }
    
    .btn span {
      position: relative;
      z-index: 1;
    }
    
    .btn:hover {
      transform: translateY(-3px) scale(1.02);
      box-shadow: 0 8px 24px rgba(76, 175, 80, 0.4);
      border-color: #4CAF50;
    }
    
    .btn:hover::before {
      opacity: 0.3;
    }
    
    .btn:active {
      transform: translateY(-1px) scale(0.98);
    }
    
    .btn:disabled {
      background: rgba(255, 255, 255, 0.05);
      border-color: rgba(255, 255, 255, 0.1);
      cursor: not-allowed;
      opacity: 0.4;
      transform: none;
    }
    
    .btn.stop, .btn.danger { 
      background: linear-gradient(135deg, rgba(244, 67, 54, 0.2), rgba(211, 47, 47, 0.3));
      border-color: rgba(244, 67, 54, 0.3);
    }
    
    .btn.stop:hover, .btn.danger:hover {
      box-shadow: 0 8px 24px rgba(244, 67, 54, 0.4);
      border-color: #f44336;
    }
    
    .btn.stop::before, .btn.danger::before {
      background: linear-gradient(135deg, #f44336, #e57373);
    }
    
    .btn.secondary { 
      background: linear-gradient(135deg, rgba(96, 125, 139, 0.2), rgba(69, 90, 100, 0.3));
      border-color: rgba(96, 125, 139, 0.3);
    }
    
    .btn.secondary:hover {
      box-shadow: 0 8px 24px rgba(96, 125, 139, 0.4);
      border-color: #607D8B;
    }
    
    .btn.secondary::before {
      background: linear-gradient(135deg, #607D8B, #78909C);
    }
    
    .btn.primary { 
      background: linear-gradient(135deg, rgba(33, 150, 243, 0.2), rgba(25, 118, 210, 0.3));
      border-color: rgba(33, 150, 243, 0.3);
    }
    
    .btn.primary:hover {
      box-shadow: 0 8px 24px rgba(33, 150, 243, 0.4);
      border-color: #2196F3;
    }
    
    .btn.primary::before {
      background: linear-gradient(135deg, #2196F3, #42A5F5);
    }
    
    /* Direction Selector */
    .direction-btn {
      padding: 20px;
      font-size: 1em;
      text-align: center;
      border-radius: 14px;
      cursor: pointer;
      transition: all .3s;
      border: 2px solid rgba(255, 255, 255, 0.1);
      background: rgba(255, 255, 255, 0.03);
    }
    
    .direction-btn.active { 
      border-color: #4CAF50;
      background: rgba(76, 175, 80, 0.15);
      box-shadow: 0 0 30px rgba(76, 175, 80, 0.3);
    }
    
    .direction-btn:hover { 
      background: rgba(255, 255, 255, 0.06);
      transform: translateY(-2px);
    }
    
    /* Info Table */
    .info-table {
      width: 100%;
      border-collapse: collapse;
      font-size: 0.95em;
    }
    
    .info-table tr { 
      border-bottom: 1px solid rgba(255, 255, 255, 0.05);
      transition: background .3s;
    }
    
    .info-table tr:hover {
      background: rgba(255, 255, 255, 0.02);
    }
    
    .info-table td { padding: 14px 8px; }
    .info-table td:first-child { 
      font-weight: 600;
      color: #aaa;
      width: 45%;
    }
    .info-table td:last-child { 
      text-align: right;
      color: #e0e0e0;
      font-family: 'Courier New', monospace;
    }
    
    .hidden { display: none !important; }
    
    /* Modal Design */
    .modal {
      position: fixed;
      top: 0;
      left: 0;
      width: 100%;
      height: 100%;
      background: rgba(0, 0, 0, 0.85);
      backdrop-filter: blur(10px);
      align-items: center;
      justify-content: center;
      z-index: 1000;
      padding: 20px;
      overflow-y: auto;
      animation: modalFadeIn .3s;
    }
    
    @keyframes modalFadeIn {
      from { opacity: 0; }
      to { opacity: 1; }
    }
    
    .modal:not(.hidden) { display: flex; }
    
    .modal-box {
      background: rgba(20, 20, 20, 0.95);
      backdrop-filter: blur(20px);
      padding: 35px;
      border-radius: 24px;
      max-width: 500px;
      width: 100%;
      max-height: 90vh;
      overflow-y: auto;
      border: 1px solid rgba(255, 255, 255, 0.1);
      box-shadow: 0 20px 60px rgba(0, 0, 0, 0.5);
      animation: modalSlideUp .3s cubic-bezier(0.4, 0, 0.2, 1);
    }
    
    @keyframes modalSlideUp {
      from { 
        opacity: 0;
        transform: translateY(30px);
      }
      to {
        opacity: 1;
        transform: translateY(0);
      }
    }
    
    .modal-box h3 { 
      margin-bottom: 25px;
      background: linear-gradient(135deg, #2196F3, #21CBF3);
      -webkit-background-clip: text;
      -webkit-text-fill-color: transparent;
      background-clip: text;
      font-size: 1.6em;
      font-weight: 600;
    }
    
    .modal-buttons {
      display: flex;
      gap: 15px;
      justify-content: center;
      margin-top: 25px;
      flex-wrap: wrap;
    }
    
    .modal-btn {
      padding: 14px 28px;
      border: none;
      border-radius: 12px;
      cursor: pointer;
      font-size: 1em;
      font-weight: 600;
      transition: all .3s;
    }
    
    .modal-btn-primary { 
      background: linear-gradient(135deg, #2196F3, #1976D2);
      color: white;
    }
    
    .modal-btn-primary:hover { 
      transform: translateY(-2px);
      box-shadow: 0 6px 20px rgba(33, 150, 243, 0.4);
    }
    
    .modal-btn-secondary { 
      background: rgba(255, 255, 255, 0.1);
      color: white;
      border: 1px solid rgba(255, 255, 255, 0.2);
    }
    
    .modal-btn-secondary:hover { 
      background: rgba(255, 255, 255, 0.15);
    }
    
    .modal-btn-danger { 
      background: linear-gradient(135deg, #f44336, #d32f2f);
      color: white;
    }
    
    .modal-btn-danger:hover { 
      transform: translateY(-2px);
      box-shadow: 0 6px 20px rgba(244, 67, 54, 0.4);
    }
    
    /* QR Code Styling */
    .qr-container {
      text-align: center;
      margin: 25px 0;
      padding: 25px;
      background: rgba(255, 255, 255, 0.05);
      border-radius: 16px;
      border: 1px solid rgba(255, 255, 255, 0.1);
    }
    
    .qr-code {
      max-width: 300px;
      width: 100%;
      height: auto;
      margin: 0 auto;
      display: block;
      background: white;
      padding: 15px;
      border-radius: 16px;
      box-shadow: 0 8px 32px rgba(0, 0, 0, 0.3);
    }
    
    .pairing-code {
      font-family: 'Courier New', monospace;
      font-size: 1.4em;
      color: #4CAF50;
      margin: 20px 0;
      padding: 18px;
      background: rgba(76, 175, 80, 0.1);
      border-radius: 12px;
      letter-spacing: 3px;
      border: 1px solid rgba(76, 175, 80, 0.3);
    }
    
    /* Alert Styles */
    .alert {
      padding: 18px;
      border-radius: 12px;
      margin: 20px 0;
      background: rgba(255, 255, 255, 0.05);
      border-left: 4px solid #2196F3;
    }
    
    .alert.warning { border-left-color: #FF9800; }
    .alert.error { border-left-color: #f44336; }
    .alert.success { border-left-color: #4CAF50; }
    
    .badge {
      display: inline-block;
      padding: 6px 12px;
      border-radius: 8px;
      font-size: 0.7em;
      font-weight: 700;
      margin-left: 10px;
      text-transform: uppercase;
      letter-spacing: 0.5px;
    }
    
    .badge.commissioned { 
      background: linear-gradient(135deg, #4CAF50, #66BB6A);
      color: white;
      box-shadow: 0 0 20px rgba(76, 175, 80, 0.4);
    }
    
    .badge.not-commissioned { 
      background: linear-gradient(135deg, #FF9800, #FFA726);
      color: white;
      box-shadow: 0 0 20px rgba(255, 152, 0, 0.4);
    }
    
    /* BLE Device List */
    .device-list {
      list-style: none;
      padding: 0;
      max-height: 400px;
      overflow-y: auto;
    }
    
    .device-item {
      background: rgba(255, 255, 255, 0.04);
      padding: 18px;
      border-radius: 14px;
      margin-bottom: 12px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 15px;
      border: 1px solid rgba(255, 255, 255, 0.05);
      transition: all .3s;
    }
    
    .device-item:hover {
      background: rgba(255, 255, 255, 0.06);
      transform: translateX(5px);
    }
    
    .device-info {
      flex: 1;
      min-width: 0;
    }
    
    .device-name {
      font-weight: 700;
      font-size: 1.1em;
      margin-bottom: 6px;
    }
    
    .device-details {
      font-size: 0.85em;
      color: #888;
      margin-top: 6px;
    }
    
    .device-actions {
      display: flex;
      gap: 10px;
      flex-shrink: 0;
    }
    
    .signal-strength {
      display: inline-block;
      padding: 3px 10px;
      border-radius: 6px;
      font-size: 0.75em;
      font-weight: 700;
    }
    
    .signal-excellent { background: #4CAF50; color: white; }
    .signal-good { background: #8BC34A; color: white; }
    .signal-fair { background: #FF9800; color: white; }
    .signal-poor { background: #f44336; color: white; }
    
    /* Input Groups */
    .input-group {
      margin: 20px 0;
    }
    
    .input-group label {
      display: block;
      margin-bottom: 10px;
      color: #aaa;
      font-size: 0.9em;
      font-weight: 600;
      text-transform: uppercase;
      letter-spacing: 0.5px;
    }
    
    .input-group input {
      width: 100%;
      padding: 14px;
      background: rgba(255, 255, 255, 0.05);
      border: 1px solid rgba(255, 255, 255, 0.1);
      border-radius: 12px;
      color: #e0e0e0;
      font-size: 1em;
      transition: all .3s;
    }
    
    .input-group input:focus {
      outline: none;
      border-color: #2196F3;
      box-shadow: 0 0 0 3px rgba(33, 150, 243, 0.1);
      background: rgba(255, 255, 255, 0.08);
    }
    
    .input-group input:disabled {
      opacity: 0.5;
      cursor: not-allowed;
    }
    
    /* Spinner */
    .spinner {
      display: inline-block;
      width: 20px;
      height: 20px;
      border: 3px solid rgba(255, 255, 255, 0.2);
      border-top-color: #2196F3;
      border-radius: 50%;
      animation: spin 1s linear infinite;
    }
    
    @keyframes spin {
      to { transform: rotate(360deg); }
    }
    
    /* Scrollbar Styling */
    ::-webkit-scrollbar {
      width: 8px;
      height: 8px;
    }
    
    ::-webkit-scrollbar-track {
      background: rgba(255, 255, 255, 0.02);
    }
    
    ::-webkit-scrollbar-thumb {
      background: rgba(255, 255, 255, 0.1);
      border-radius: 4px;
    }
    
    ::-webkit-scrollbar-thumb:hover {
      background: rgba(255, 255, 255, 0.2);
    }
    
    /* Responsive Design */
    @media (max-width: 768px) {
      .header h1 { font-size: 2em; }
      .status-grid { grid-template-columns: 1fr 1fr; }
      .btn-grid { grid-template-columns: 1fr; }
      .nav { justify-content: flex-start; }
    }
  </style>
</head>
<body>
  <div class="container">
    <div class="header">
      <h1>🎚️ BeltWinder Matter</h1>
      <div class="subtitle">Smart Shutter Control System</div>
    </div>

    <div class="nav">
      <button onclick="show('overview')" class="active" id="nav-overview">
        <span>📊 Overview</span>
      </button>
      <button onclick="show('matter')" id="nav-matter">
        <span>🔗 Matter</span>
      </button>
      <button onclick="show('ble')" id="nav-ble">
        <span>📡 BLE Sensor</span>
      </button>
      <button onclick="show('system')" id="nav-system">
        <span>💻 System</span>
      </button>
      <button onclick="show('settings')" id="nav-settings">
        <span>⚙️ Settings</span>
      </button>
    </div>

    <!-- Overview Tab -->
    <div id="overview">
      <div class="card">
        <h2>Status</h2>
        <div class="status-grid">
          <div class="status-item">
            <div class="status-label">Position</div>
            <div class="status-value" id="pos">0%</div>
          </div>
          <div class="status-item">
            <div class="status-label">Calibrated</div>
            <div class="status-value" id="calib">No</div>
          </div>
          <div class="status-item">
            <div class="status-label">Direction</div>
            <div class="status-value" id="inv">Normal</div>
          </div>
          <div class="status-item">
            <div class="status-label">Matter Status</div>
            <div class="status-value" id="matter-status">Checking...</div>
          </div>
        </div>
        <div class="btn-grid">
          <button class="btn" onclick="send('up')">
            <span>⬆ UP</span>
          </button>
          <button class="btn" onclick="send('down')">
            <span>⬇ DOWN</span>
          </button>
          <button class="btn stop" onclick="send('stop')">
            <span>⏹ STOP</span>
          </button>
        </div>
      </div>
    </div>

    <!-- Matter Tab -->
    <div id="matter" class="hidden">
      <div class="card">
        <h2>Matter Pairing Information</h2>
        <div id="matter-status-detail"></div>
        <div id="matter-pairing-info"></div>
      </div>
    </div>

    <!-- BLE Sensor Tab -->
    <div id="ble" class="hidden">
      <div class="card">
        <h2>BLE Window Sensor</h2>
        
        <div id="ble-sensor-status" class="hidden">
          <h3 style="margin-top:0;color:#888">Current Sensor</h3>
          
          <div class="status-grid" style="grid-template-columns: repeat(auto-fit, minmax(120px, 1fr)); margin-bottom: 15px;">
            <div class="status-item">
              <div class="status-label">Contact</div>
              <div class="status-value" id="ble-contact">Unknown</div>
            </div>
            <div class="status-item">
              <div class="status-label">Battery</div>
              <div class="status-value" id="ble-battery">--%</div>
            </div>
            <div class="status-item">
              <div class="status-label">Signal</div>
              <div class="status-value" id="ble-rssi">-- dBm</div>
            </div>
          </div>
          
          <div class="status-grid" style="grid-template-columns: repeat(auto-fit, minmax(120px, 1fr)); margin-bottom: 20px;">
            <div class="status-item">
              <div class="status-label">Brightness</div>
              <div class="status-value" id="ble-lux">-- lux</div>
            </div>
            <div class="status-item">
              <div class="status-label">Rotation</div>
              <div class="status-value" id="ble-rotation">--°</div>
            </div>
            <div class="status-item">
              <div class="status-label">Last Update</div>
              <div class="status-value" id="ble-last-update">Never</div>
            </div>
          </div>

          <div class="status-grid" style="grid-template-columns: repeat(auto-fit, minmax(120px, 1fr)); margin-bottom: 15px;">
            <div class="status-item">
              <div class="status-label">Packet ID</div>
              <div class="status-value" id="ble-packet-id">--</div>
            </div>
            <div class="status-item" id="ble-button-container" style="display:none">
              <div class="status-label">Button Event</div>
              <div class="status-value" id="ble-button-event">None</div>
            </div>
          </div>
          
          <div style="background:rgba(255,255,255,0.04);padding:18px;border-radius:14px;margin-bottom:15px;border:1px solid rgba(255,255,255,0.08)">
            <div style="display:flex;justify-content:space-between;align-items:center">
              <div>
                <div style="font-weight:bold;font-size:1.1em" id="ble-device-name">Unknown Device</div>
                <div style="color:#888;font-size:0.9em;margin-top:5px" id="ble-device-address">--:--:--:--:--:--</div>
              </div>
              <button class="btn danger" onclick="unpairBLE()" style="padding:12px 24px">
                <span>🔓 Unpair</span>
              </button>
            </div>
          </div>
        </div>
        
        <!-- Scan Interface -->
        <div id="ble-scan-interface">
          <p style="color:#888;margin-bottom:20px;line-height:1.8">
            Search for nearby Shelly Blu Door/Window sensors to integrate window state detection with your smart shutter.
          </p>
          
          <button class="btn primary" id="ble-scan-btn" onclick="startBLEScan()" style="width:100%;margin-bottom:25px">
            <span>🔍 Start Scan</span>
          </button>
          
          <h3 style="margin-top:30px;color:#888;font-size:1.1em;font-weight:600">Discovered Devices</h3>
          <ul class="device-list" id="ble-discovered-devices">
            <li style="text-align:center;color:#666;padding:40px 20px">
              <div style="font-size:3em;margin-bottom:10px;opacity:0.3">📡</div>
              <div>No scan performed yet</div>
            </li>
          </ul>
        </div>
      </div>
    </div>

    <!-- System Tab -->
    <div id="system" class="hidden">
      <div class="card">
        <h2>System Information</h2>
        <table class="info-table" id="sysinfo"></table>
      </div>
    </div>

    <!-- Settings Tab -->
    <div id="settings" class="hidden">
      <div class="card">
        <h2>Calibration</h2>
        <p style="color:#888;margin-bottom:20px;line-height:1.8">
          Calibrate the shutter to learn its full travel range. The system will automatically move the shutter up and down to determine minimum and maximum positions.
        </p>
        <button class="btn primary" onclick="send('calibrate')" style="width:100%">
          <span>🎯 START CALIBRATION</span>
        </button>
      </div>

      <div class="card">
        <h2>Direction Control</h2>
        <p style="color:#888;margin-bottom:20px;line-height:1.8">
          Change motor direction if UP/DOWN controls are reversed. This swaps the polarity without rewiring.
        </p>
        <div style="display:grid;grid-template-columns:1fr 1fr;gap:15px">
          <div class="direction-btn" id="dir-normal" onclick="setDirection(false)">
            <div style="font-size:2em;margin-bottom:8px">✅</div>
            <div style="font-weight:bold;font-size:1.1em">Normal</div>
            <div style="font-size:0.85em;color:#888;margin-top:5px">Standard direction</div>
          </div>
          <div class="direction-btn" id="dir-inverted" onclick="setDirection(true)">
            <div style="font-size:2em;margin-bottom:8px">🔄</div>
            <div style="font-weight:bold;font-size:1.1em">Inverted</div>
            <div style="font-size:0.85em;color:#888;margin-top:5px">Reversed direction</div>
          </div>
        </div>
      </div>

      <div class="card">
        <h2>Factory Reset</h2>
        <p style="color:#888;margin-bottom:20px;line-height:1.8">
          ⚠️ This will remove the device from Matter network and erase all settings including calibration data. This action cannot be undone.
        </p>
        <button class="btn danger" onclick="showConfirm()" style="width:100%">
          <span>🗑️ FACTORY RESET</span>
        </button>
      </div>
    </div>
    
  </div>

  <!-- Factory Reset Confirmation Modal -->
  <div id="confirm-reset" class="modal hidden">
    <div class="modal-box">
      <h3>⚠️ Confirm Factory Reset</h3>
      <p style="margin:20px 0;font-size:1.05em;line-height:1.6">
        This will permanently delete all device data and settings:
      </p>
      <ul style="margin:15px 0 15px 25px;color:#888;line-height:1.8">
        <li>Remove device from Matter network</li>
        <li>Erase all calibration data</li>
        <li>Reset direction settings to defaults</li>
        <li>Clear BLE sensor pairing</li>
      </ul>
      <div class="alert error" style="margin:20px 0">
        <strong>⚠️ Warning:</strong> This action cannot be undone! You will need to re-pair the device with your Matter controller.
      </div>
      <div class="modal-buttons">
        <button class="modal-btn modal-btn-secondary" onclick="hideConfirm()">Cancel</button>
        <button class="modal-btn modal-btn-danger" onclick="confirmReset()">Reset Device</button>
      </div>
    </div>
  </div>

  <!-- Matter QR Code Modal -->
  <div id="matter-qr-modal" class="modal hidden">
    <div class="modal-box">
      <h3>🔗 Matter Pairing</h3>
      <div class="alert success">
        <strong>✓ Device is ready!</strong>
        <p style="margin-top:8px">Scan the QR code below with your Matter-compatible controller (Apple Home, Google Home, Amazon Alexa, etc.)</p>
      </div>
      
      <div class="qr-container">
        <img class="qr-code" id="matter-qr-img" src="" alt="QR Code">
        <div style="margin-top:15px;color:#888;font-size:0.9em">Scan with your Matter controller app</div>
        <a id="qr-web-link" href="" target="_blank" 
          style="display:inline-block;margin-top:15px;color:#2196F3;text-decoration:none;font-weight:600;transition:all .3s">
          🌐 Open in CHIP QR Generator
        </a>
      </div>
      
      <div style="margin-top:25px">
        <div style="color:#888;margin-bottom:12px;font-weight:600;font-size:0.9em;text-transform:uppercase;letter-spacing:1px">Manual Pairing Code:</div>
        <div class="pairing-code" id="matter-pairing-code">Loading...</div>
        <div style="color:#666;font-size:0.85em;text-align:center;margin-top:10px">
          Enter this code if your controller doesn't support QR scanning
        </div>
      </div>
      
      <div class="modal-buttons">
        <button class="modal-btn modal-btn-secondary" onclick="closeMatterQR()">Close</button>
      </div>
    </div>
  </div>

  <!-- BLE Pairing Modal -->
  <div id="ble-pair-modal" class="modal hidden">
    <div class="modal-box">
      <h3>📱 Pair BLE Sensor</h3>
      
      <div class="input-group">
        <label>Device Name</label>
        <input type="text" id="ble-pair-name" readonly>
      </div>
      
      <div class="input-group">
        <label>MAC Address</label>
        <input type="text" id="ble-pair-address" readonly>
      </div>
      
      <div class="input-group">
        <label>Bindkey (32 hex characters)</label>
        <input type="text" id="ble-pair-bindkey" 
               placeholder="Leave empty if not encrypted" 
               maxlength="32" 
               oninput="validateBLEPairForm()">
      </div>
      
      <div id="ble-pair-bindkey-hint" class="alert warning hidden">
        <strong>🔒 Encrypted Device</strong>
        <p style="margin-top:8px">This sensor uses encryption. Please enter the 32-character bindkey from the Shelly app (Settings → Device → Bluetooth).</p>
      </div>
      
      <div class="modal-buttons">
        <button class="modal-btn modal-btn-secondary" onclick="closeBLEPairModal()">Cancel</button>
        <button class="modal-btn modal-btn-primary" id="ble-pair-confirm-btn" 
                onclick="confirmBLEPair()" disabled>Pair Device</button>
      </div>
    </div>
  </div>

  <script>
  let ws;
  let reconnectInterval;
  let statusInterval;
  let matterCommissioned = false;

  function connectWebSocket() {
    ws = new WebSocket('ws://' + location.host + '/ws');

    ws.onopen = () => {
      console.log('WebSocket connected');
      clearInterval(reconnectInterval);
      
      setTimeout(() => {
        if (ws.readyState === WebSocket.OPEN) {
          ws.send('status');
          ws.send('matter_status');
        }
      }, 100);
      
      statusInterval = setInterval(() => {
        if (ws.readyState === WebSocket.OPEN) {
          ws.send('status');
        }
      }, 2000);
    };

    ws.onclose = () => {
      console.log('WebSocket disconnected. Reconnecting...');
      clearInterval(statusInterval);
      reconnectInterval = setInterval(connectWebSocket, 5000);
    };

    ws.onerror = (e) => {
      console.error('WebSocket error:', e);
    };

    ws.onmessage = e => {
      console.log('WebSocket message received:', e.data);
      let d;
      
      try {
          d = JSON.parse(e.data);
      } catch (err) {
          console.error('Failed to parse JSON:', err);
          return;
      }
      
      if (!d || !d.type) {
          console.error('Invalid message format - missing type:', d);
          return;
      }
      
      if (d.type === 'status') {
          document.getElementById('pos').innerText = d.pos + '%';
          document.getElementById('calib').innerText = d.cal ? 'Yes' : 'No';
          document.getElementById('inv').innerText = d.inv ? 'Inverted' : 'Normal';
          updateDirectionButtons(d.inv);
      } 
      else if (d.type === 'matter_status') {
          matterCommissioned = d.commissioned;
          let statusEl = document.getElementById('matter-status');
          
          if (d.commissioned) {
              statusEl.innerHTML = 'Paired <span class="badge commissioned">✓</span>';
              statusEl.className = 'status-value commissioned';
          } else {
              statusEl.innerHTML = 'Not Paired <span class="badge not-commissioned">!</span>';
              statusEl.className = 'status-value not-commissioned';
          }
          
          updateMatterInfo(d);
      } 
      else if (d.type === 'info') {
          renderSystemInfo(d);
      }
      else if (d.type === 'ble_discovered') {
          renderBLEDiscoveredDevices(d.devices);
      }
      else if (d.type === 'ble_status') {
          renderBLESensorStatus(d);
      }
      else if (d.type === 'ble_scan_complete') {
          let btn = document.getElementById('ble-scan-btn');
          btn.disabled = false;
          btn.innerHTML = '<span>🔍 Start Scan</span>';
      }
    };
  }

  connectWebSocket();

  function show(id) {
    document.querySelectorAll('#overview, #matter, #ble, #system, #settings').forEach(e => e.classList.add('hidden'));
    document.getElementById(id).classList.remove('hidden');
    
    document.querySelectorAll('.nav button').forEach(b => b.classList.remove('active'));
    document.getElementById('nav-' + id).classList.add('active');
    
    if (id === 'system' && ws.readyState === WebSocket.OPEN) {
      ws.send('info');
    }
    if (id === 'matter' && ws.readyState === WebSocket.OPEN) {
      ws.send('matter_status');
    }
    if (id === 'ble' && ws.readyState === WebSocket.OPEN) {
      ws.send('ble_status');
    }
  }

  function send(c) {
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(c);
    }
  }

  function setDirection(inverted) {
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(inverted ? 'invert_on' : 'invert_off');
      updateDirectionButtons(inverted);
    }
  }

  function updateDirectionButtons(inverted) {
    document.getElementById('dir-normal').className = inverted ? 'direction-btn' : 'direction-btn active';
    document.getElementById('dir-inverted').className = inverted ? 'direction-btn active' : 'direction-btn';
  }

  function showConfirm() {
    document.getElementById('confirm-reset').classList.remove('hidden');
  }

  function hideConfirm() {
    document.getElementById('confirm-reset').classList.add('hidden');
  }

  function confirmReset() {
    if (ws.readyState === WebSocket.OPEN) {
      ws.send('reset');
      hideConfirm();
    }
  }

  function showMatterQR(qrUrl, qrImageUrl, pairingCode) {
    document.getElementById('matter-qr-img').src = qrImageUrl;
    document.getElementById('matter-pairing-code').innerText = pairingCode;
    
    let linkEl = document.getElementById('qr-web-link');
    if (linkEl) {
      linkEl.href = qrUrl;
      linkEl.style.display = qrUrl ? 'inline-block' : 'none';
    }
    
    document.getElementById('matter-qr-modal').classList.remove('hidden');
  }

  function closeMatterQR() {
    document.getElementById('matter-qr-modal').classList.add('hidden');
  }

  function updateMatterInfo(data) {
    if (!data) return;
    
    let statusHtml = '<div class="alert ' + (data.commissioned ? 'success' : 'warning') + '">';
    
    if (data.commissioned) {
      statusHtml += '<strong>✓ Device is paired with Matter</strong>';
      statusHtml += '<p style="margin-top:10px;color:#ccc">Your shutter is connected and can be controlled by your Matter ecosystem.</p>';
      statusHtml += '<p style="margin-top:8px;color:#888">Active Fabrics: ' + (data.fabrics || 0) + '</p>';
    } else {
      statusHtml += '<strong>! Device not yet paired</strong>';
      statusHtml += '<p style="margin-top:10px;color:#ccc">Scan the QR code below or use the manual pairing code to add this device to your Matter controller.</p>';
    }
    statusHtml += '</div>';
    
    document.getElementById('matter-status-detail').innerHTML = statusHtml;
    
    let pairingHtml = '';
    
    if (!data.commissioned) {
      if (data.qr_image && data.qr_image.length > 0) {
        pairingHtml = '<div style="margin-top:20px">';
        pairingHtml += '<button class="btn primary" onclick="showMatterQR(\'' + 
                      (data.qr_url || '') + '\',\'' + 
                      data.qr_image + '\',\'' + 
                      (data.pairing_code || '') + 
                      '\')" style="width:100%"><span>📱 Show QR Code & Pairing Info</span></button>';
        pairingHtml += '</div>';
      } else {
        pairingHtml = '<div class="alert warning" style="margin-top:20px">';
        pairingHtml += '<strong>⚠️ QR Code not available</strong><br>';
        pairingHtml += '<p style="margin-top:10px;color:#ccc">Pairing Code: ' + (data.pairing_code || 'N/A') + '</p>';
        pairingHtml += '<p style="margin-top:5px;color:#888">Please check the serial console for pairing information.</p>';
        pairingHtml += '</div>';
      }
    } else {
      pairingHtml = '<div class="alert success" style="margin-top:20px">';
      pairingHtml += '<strong>✓ Device is successfully paired</strong><br>';
      pairingHtml += '<p style="margin-top:10px;color:#ccc">To remove this device, use Factory Reset in Settings.</p>';
      pairingHtml += '</div>';
    }
    
    document.getElementById('matter-pairing-info').innerHTML = pairingHtml;
  }

  function renderSystemInfo(d) {
    let html = '';
    html += '<tr><td>Chip ID:</td><td>' + d.chip + '</td></tr>';
    html += '<tr><td>Uptime:</td><td>' + d.uptime + 's</td></tr>';
    html += '<tr><td>Free Heap:</td><td>' + (d.heap / 1024).toFixed(1) + ' KB</td></tr>';
    html += '<tr><td>Min Free Heap:</td><td>' + (d.minheap / 1024).toFixed(1) + ' KB</td></tr>';
    html += '<tr><td>Flash Size:</td><td>' + (d.flash / 1024 / 1024).toFixed(1) + ' MB</td></tr>';
    html += '<tr><td>Firmware Version:</td><td>' + d.ver + '</td></tr>';
    html += '<tr><td>Reset Reason:</td><td>' + d.reset + '</td></tr>';
    document.getElementById('sysinfo').innerHTML = html;
  }

  let currentBLEPairDevice = null;

  function startBLEScan() {
    if (ws.readyState === WebSocket.OPEN) {
        ws.send('ble_scan');
        
        let btn = document.getElementById('ble-scan-btn');
        btn.disabled = true;
        btn.innerHTML = '<span class="spinner"></span> <span>Scanning...</span>';
        
        setTimeout(() => {
            btn.disabled = false;
            btn.innerHTML = '<span>🔍 Start Scan</span>';
        }, 12000);
    }
  }

  function openBLEPairModal(deviceJson) {
    let device;
    try {
        device = typeof deviceJson === 'string' ? JSON.parse(deviceJson) : deviceJson;
    } catch (e) {
        console.error('Failed to parse device JSON:', e, deviceJson);
        alert('Error: Invalid device data');
        return;
    }
    
    currentBLEPairDevice = device;
    
    document.getElementById('ble-pair-name').value = device.name || 'Unknown';
    document.getElementById('ble-pair-address').value = device.address || 'Unknown';
    document.getElementById('ble-pair-bindkey').value = '';
    
    let hintEl = document.getElementById('ble-pair-bindkey-hint');
    if (device.encrypted) {
        hintEl.classList.remove('hidden');
    } else {
        hintEl.classList.add('hidden');
    }
    
    validateBLEPairForm();
    document.getElementById('ble-pair-modal').classList.remove('hidden');
  }

  function closeBLEPairModal() {
    document.getElementById('ble-pair-modal').classList.add('hidden');
    currentBLEPairDevice = null;
  }

  function validateBLEPairForm() {
    if (!currentBLEPairDevice) return;
    
    let bindkey = document.getElementById('ble-pair-bindkey').value.trim();
    let btn = document.getElementById('ble-pair-confirm-btn');
    
    if (currentBLEPairDevice.encrypted) {
      btn.disabled = (bindkey.length !== 32);
    } else {
      btn.disabled = false;
    }
  }

  function confirmBLEPair() {
    if (!currentBLEPairDevice) return;
    
    let bindkey = document.getElementById('ble-pair-bindkey').value.trim();
    
    if (currentBLEPairDevice.encrypted && bindkey.length !== 32) {
      alert('Encrypted device requires a 32-character bindkey');
      return;
    }
    
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({
        cmd: 'ble_pair',
        address: currentBLEPairDevice.address,
        bindkey: bindkey
      }));
      
      closeBLEPairModal();
      
      setTimeout(() => {
        ws.send('ble_status');
      }, 1000);
    }
  }

  function unpairBLE() {
    if (!confirm('Remove this sensor? The shutter will no longer react to window open/close events.')) {
      return;
    }
    
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({cmd: 'ble_unpair'}));
      
      setTimeout(() => {
        ws.send('ble_status');
      }, 500);
    }
  }

  function renderBLEDiscoveredDevices(devices) {
    let html = '';
    
    if (devices.length === 0) {
        html = '<li style="text-align:center;color:#666;padding:40px 20px">';
        html += '<div style="font-size:3em;margin-bottom:10px;opacity:0.3">🔍</div>';
        html += '<div>No devices found. Click "Start Scan" to search.</div>';
        html += '</li>';
    } else {
        devices.forEach((dev) => {
            let signalClass = 'signal-poor';
            if (dev.rssi >= -60) signalClass = 'signal-excellent';
            else if (dev.rssi >= -70) signalClass = 'signal-good';
            else if (dev.rssi >= -80) signalClass = 'signal-fair';
            
            html += `<li class="device-item">
              <div class="device-info">
                <div class="device-name">${dev.name}`;
            
            if (dev.encrypted) {
                html += `<span class="badge" style="background:rgba(255,152,0,0.2);color:#FF9800;border:1px solid rgba(255,152,0,0.3)">🔒 Encrypted</span>`;
            }
            
            html += `</div>
                <div class="device-details">
                  MAC: ${dev.address} | 
                  <span class="${signalClass}">${dev.rssi} dBm</span>
                </div>
              </div>
              <div class="device-actions">
                <button class="btn secondary" onclick='openBLEPairModal(${JSON.stringify(dev)})'  style="padding:10px 20px"><span>Pair</span></button>
              </div>
            </li>`;
        });
    }
    
    document.getElementById('ble-discovered-devices').innerHTML = html;
  }

  function renderBLESensorStatus(data) {
  if (data.paired) {
    document.getElementById('ble-sensor-status').classList.remove('hidden');
    document.getElementById('ble-scan-interface').classList.add('hidden');
    
    document.getElementById('ble-device-name').innerText = data.name;
    document.getElementById('ble-device-address').innerText = data.address;
    
    let contactEl = document.getElementById('ble-contact');
    if (data.sensor_data && data.sensor_data.valid) {
      contactEl.innerText = data.sensor_data.window_open ? '🔓 OPEN' : '🔒 CLOSED';
      contactEl.className = 'status-value ' + (data.sensor_data.window_open ? 'not-commissioned' : 'commissioned');
      
      document.getElementById('ble-battery').innerText = data.sensor_data.battery + '%';
      document.getElementById('ble-rssi').innerText = data.sensor_data.rssi + ' dBm';
      document.getElementById('ble-lux').innerText = data.sensor_data.illuminance + ' lux';
      document.getElementById('ble-rotation').innerText = data.sensor_data.rotation + '°';
      
      let secondsAgo = Math.floor((Date.now() - data.sensor_data.last_update) / 1000);
      let timeStr;
      if (secondsAgo < 60) timeStr = secondsAgo + 's ago';
      else if (secondsAgo < 3600) timeStr = Math.floor(secondsAgo / 60) + 'm ago';
      else timeStr = Math.floor(secondsAgo / 3600) + 'h ago';
      
      document.getElementById('ble-last-update').innerText = timeStr;
      
      // ============================================================
      // NEU: Packet ID anzeigen
      // ============================================================
      if (typeof data.sensor_data.packet_id !== 'undefined') {
        document.getElementById('ble-packet-id').innerText = data.sensor_data.packet_id;
      } else {
        document.getElementById('ble-packet-id').innerText = '--';
      }
      
      // ============================================================
      // NEU: Button Events anzeigen
      // ============================================================
      if (data.sensor_data.has_button_event) {
        let eventName;
        let eventEmoji;
        
        switch (data.sensor_data.button_event) {
          case 0:
            eventName = 'None';
            eventEmoji = '';
            break;
          case 1:
            eventName = 'Single Press';
            eventEmoji = '👆';
            break;
          case 128:
          case 254:  // Firmware < 1.0.20
            eventName = 'Hold';
            eventEmoji = '⏸️';
            break;
          default:
            eventName = 'Unknown (' + data.sensor_data.button_event + ')';
            eventEmoji = '❓';
        }
        
        document.getElementById('ble-button-event').innerText = eventEmoji + ' ' + eventName;
        document.getElementById('ble-button-container').style.display = 'block';
        
        // Optional: Highlight für kurze Zeit
        let btnContainer = document.getElementById('ble-button-container');
        btnContainer.style.background = 'rgba(33, 150, 243, 0.2)';
        btnContainer.style.borderColor = '#2196F3';
        
        setTimeout(() => {
          btnContainer.style.background = '';
          btnContainer.style.borderColor = '';
        }, 3000);  // Highlight für 3 Sekunden
        
      } else {
        document.getElementById('ble-button-container').style.display = 'none';
      }
      
    } else {
      // Keine gültigen Sensor-Daten
      contactEl.innerText = 'No Data';
      contactEl.className = 'status-value';
      document.getElementById('ble-battery').innerText = '--%';
      document.getElementById('ble-rssi').innerText = '-- dBm';
      document.getElementById('ble-lux').innerText = '-- lux';
      document.getElementById('ble-rotation').innerText = '--°';
      document.getElementById('ble-last-update').innerText = 'Waiting...';
      document.getElementById('ble-packet-id').innerText = '--';  // NEU
      document.getElementById('ble-button-container').style.display = 'none';  // NEU
    }
  } else {
    document.getElementById('ble-sensor-status').classList.add('hidden');
    document.getElementById('ble-scan-interface').classList.remove('hidden');
  }
}
  </script>
</body>
</html>
)rawliteral";

// ============================================================================
// Static Close Callback
// ============================================================================

static void ws_close_callback(httpd_handle_t hd, int sockfd) {
    ESP_LOGI("WebUI", "Socket %d closed by HTTP server", sockfd);
}

// ============================================================================
// WebUIHandler Implementation
// ============================================================================

WebUIHandler::WebUIHandler(app_driver_handle_t h, ShellyBLEManager* ble) 
    : handle(h), bleManager(ble), server(nullptr) {
    client_mutex = xSemaphoreCreateMutex();
    if (!client_mutex) {
        ESP_LOGE(TAG, "Failed to create client mutex");
    }
}

WebUIHandler::~WebUIHandler() {
    if (server) {
        httpd_stop(server);
    }
    if (client_mutex) {
        vSemaphoreDelete(client_mutex);
    }
}

void WebUIHandler::begin() {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    
    // ✓ Socket-Handling verbessern
    cfg.max_open_sockets = 5;             // Weniger Sockets (5 statt 7)
    cfg.lru_purge_enable = true;          // LRU Purge aktivieren
    cfg.max_uri_handlers = 4;
    cfg.stack_size = 8192;                // Stack erhöhen
    cfg.ctrl_port = 32768;                // Control Socket Port
    cfg.close_fn = nullptr;               // ✓ Kein manueller Close!
    cfg.uri_match_fn = nullptr;
    cfg.keep_alive_enable = false;        // ✓ Keep-Alive deaktivieren
    cfg.keep_alive_idle = 0;
    cfg.keep_alive_interval = 0;
    cfg.keep_alive_count = 0;
    cfg.lru_purge_enable = true;          // ✓ Alte Verbindungen aufräumen

    if (httpd_start(&server, &cfg) == ESP_OK) {
        httpd_uri_t root = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = root_handler,
            .user_ctx  = this
        };
        httpd_register_uri_handler(server, &root);

        httpd_uri_t ws = {
            .uri          = "/ws",
            .method       = HTTP_GET,
            .handler      = ws_handler,
            .user_ctx     = this,
            .is_websocket = true,
            .handle_ws_control_frames = true  // ✓ Control Frames handlen
        };
        httpd_register_uri_handler(server, &ws);
        
        ESP_LOGI(TAG, "HTTP server started");
        ESP_LOGI(TAG, "  Max open sockets: %d", cfg.max_open_sockets);
        ESP_LOGI(TAG, "  LRU purge: %s", cfg.lru_purge_enable ? "enabled" : "disabled");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }
}


void WebUIHandler::register_client(int fd) {
    if (xSemaphoreTake(client_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (active_clients.size() >= MAX_CLIENTS) {
            ESP_LOGW(TAG, "✗ WebSocket limit reached (%d/%d) - rejecting fd=%d", 
                     active_clients.size(), MAX_CLIENTS, fd);
            xSemaphoreGive(client_mutex);
            
            close(fd);
            return;
        }
        
        ClientInfo client;
        client.fd = fd;
        client.last_activity = millis();
        
        active_clients.push_back(client);
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "Client connected: fd=%d (total: %d)", fd, active_clients.size());
        ESP_LOGI(TAG, "═══════════════════════════════════");
        xSemaphoreGive(client_mutex);
    }
}

void WebUIHandler::unregister_client(int fd) {
    if (xSemaphoreTake(client_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        ESP_LOGI(TAG, "Closing socket fd=%d...", fd);
        
        active_clients.erase(
            std::remove_if(active_clients.begin(), active_clients.end(),
                          [fd](const ClientInfo& c) { return c.fd == fd; }),
            active_clients.end()
        );
        
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "Client disconnected: fd=%d (remaining: %d)", fd, active_clients.size());
        ESP_LOGI(TAG, "═══════════════════════════════════");
        xSemaphoreGive(client_mutex);
    }
}

void WebUIHandler::broadcast_to_all_clients(const char* message) {
    if (!server || !message) return;
    
    if (xSemaphoreTake(client_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)message,
            .len = strlen(message)
        };
        
        std::vector<ClientInfo> clients_copy = active_clients;
        xSemaphoreGive(client_mutex);
        
        std::vector<int> failed_fds;
        int sent_count = 0;
        
        for (const auto& client : clients_copy) {
            if (sent_count++ % 5 == 0) {
                esp_task_wdt_reset();
            }
            
            esp_err_t ret = httpd_ws_send_frame_async(server, client.fd, &frame);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to send to fd=%d (error: %s)", 
                         client.fd, esp_err_to_name(ret));
                failed_fds.push_back(client.fd);
            }
        }
        
        for (int fd : failed_fds) {
            unregister_client(fd);
        }
        
        esp_task_wdt_reset();
    }
}


esp_err_t WebUIHandler::root_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t WebUIHandler::ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        WebUIHandler* self = (WebUIHandler*)req->user_ctx;
        int fd = httpd_req_to_sockfd(req);
        
        struct sockaddr_in6 addr;
        socklen_t addr_len = sizeof(addr);
        if (getpeername(fd, (struct sockaddr*)&addr, &addr_len) == 0) {
            char ip_str[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &addr.sin6_addr, ip_str, sizeof(ip_str));
            ESP_LOGI(TAG, "═══════════════════════════════════");
            ESP_LOGI(TAG, "WebSocket: NEW WEBSOCKET CONNECTION");
            ESP_LOGI(TAG, "WebSocket: Client IP: %s", ip_str);
            ESP_LOGI(TAG, "WebSocket: Socket FD: %d", fd);
            ESP_LOGI(TAG, "═══════════════════════════════════");
        }
        
        self->register_client(fd);
        return ESP_OK;
    }

    // Frame empfangen
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket: Failed to receive frame header");
        return ret;
    }
    
    // WICHTIG: Close Frame behandeln
    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "WebSocket: Close frame received");
        WebUIHandler* self = (WebUIHandler*)req->user_ctx;
        int fd = httpd_req_to_sockfd(req);
        self->unregister_client(fd);
        return ESP_OK;
    }
    
    // Ping/Pong frames ignorieren
    if (ws_pkt.type == HTTPD_WS_TYPE_PING || 
        ws_pkt.type == HTTPD_WS_TYPE_PONG) {
        return ESP_OK;
    }
    
    // Nur Text-Frames verarbeiten
    if (ws_pkt.type != HTTPD_WS_TYPE_TEXT) {
        ESP_LOGW(TAG, "WebSocket: Ignoring non-text frame type: %d", ws_pkt.type);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "WebSocket: Receiving text frame...");
    
    if (ws_pkt.len == 0 || ws_pkt.len > 512) {
        ESP_LOGE(TAG, "WebSocket: Invalid frame length: %d", ws_pkt.len);
        return ESP_ERR_INVALID_SIZE;
    }
    
    uint8_t* buf = (uint8_t*)malloc(ws_pkt.len + 1);
    if (!buf) {
        ESP_LOGE(TAG, "WebSocket: Failed to allocate buffer");
        return ESP_ERR_NO_MEM;
    }
    
    ws_pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket: Failed to receive frame payload: %s", esp_err_to_name(ret));
        free(buf);
        return ret;
    }
    
    buf[ws_pkt.len] = '\0';
    char* cmd = (char*)buf;
    
    WebUIHandler* self = (WebUIHandler*)req->user_ctx;
    int fd = httpd_req_to_sockfd(req);
    
    ESP_LOGI(TAG, "WebSocket: Received command from fd=%d: '%s'", fd, cmd);

    // Last activity aktualisieren
    if (xSemaphoreTake(self->client_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (auto& client : self->active_clients) {
            if (client.fd == fd) {
                client.last_activity = millis();
                break;
            }
        }
        xSemaphoreGive(self->client_mutex);
    }
    
    // ========================================================================
    // Command Handling (ERWEITERT MIT DEBUG)
    // ========================================================================

    // Shutter Commands
    if (strcmp(cmd, "up") == 0) {
        ESP_LOGI(TAG, "→ Command: UP (move to 0%%)");
        esp_err_t result = shutter_driver_go_to_lift_percent(self->handle, 0);
        ESP_LOGI(TAG, "← Result: %s", result == ESP_OK ? "SUCCESS" : "FAILED");
    } 
    else if (strcmp(cmd, "down") == 0) {
        ESP_LOGI(TAG, "→ Command: DOWN (move to 100%%)");
        esp_err_t result = shutter_driver_go_to_lift_percent(self->handle, 100);
        ESP_LOGI(TAG, "← Result: %s", result == ESP_OK ? "SUCCESS" : "FAILED");
    }
    else if (strncmp(cmd, "pos:", 4) == 0) {
        int target_pos = atoi(cmd + 4);
        if (target_pos < 0) target_pos = 0;
        if (target_pos > 100) target_pos = 100;
        ESP_LOGI(TAG, "→ Command: SLIDER POSITION (move to %d%%)", target_pos);
        esp_err_t result = shutter_driver_go_to_lift_percent(self->handle, target_pos);
        ESP_LOGI(TAG, "← Result: %s", result == ESP_OK ? "SUCCESS" : "FAILED");
    }
    else if (strcmp(cmd, "stop") == 0) {
        ESP_LOGI(TAG, "→ Command: STOP");
        esp_err_t result = shutter_driver_stop_motion(self->handle);
        uint8_t current_pos = shutter_driver_get_current_percent(self->handle);
        ESP_LOGI(TAG, "← Stopped at %d%% | Result: %s", current_pos, 
                result == ESP_OK ? "SUCCESS" : "FAILED");
    } 
    else if (strcmp(cmd, "calibrate") == 0) {
        ESP_LOGI(TAG, "→ Command: START CALIBRATION");
        bool was_calibrated = shutter_driver_is_calibrated(self->handle);
        esp_err_t result = shutter_driver_start_calibration(self->handle);
        ESP_LOGI(TAG, "← Calibration started | Previously calibrated: %s | Result: %s",
                was_calibrated ? "YES" : "NO",
                result == ESP_OK ? "SUCCESS" : "FAILED");
    } 
    else if (strcmp(cmd, "invert_on") == 0) {
        ESP_LOGI(TAG, "→ Command: INVERT DIRECTION → ON");
        shutter_driver_set_direction(self->handle, true);
        bool current = shutter_driver_get_direction_inverted(self->handle);
        ESP_LOGI(TAG, "← Direction now: %s", current ? "INVERTED ✓" : "NORMAL (ERROR!)");
    }
    else if (strcmp(cmd, "invert_off") == 0) {
        ESP_LOGI(TAG, "→ Command: INVERT DIRECTION → OFF");
        shutter_driver_set_direction(self->handle, false);
        bool current = shutter_driver_get_direction_inverted(self->handle);
        ESP_LOGI(TAG, "← Direction now: %s", current ? "INVERTED (ERROR!)" : "NORMAL ✓");
    }
    else if (strcmp(cmd, "reset") == 0) {
        ESP_LOGW(TAG, "=== Factory Reset Initiated via WebUI ===");
        
        // send confirmation message to client
        const char* confirm_msg = "{\"type\":\"info\",\"message\":\"Resetting device...\"}";
        httpd_ws_frame_t confirm_frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)confirm_msg,
            .len = strlen(confirm_msg)
        };
        httpd_ws_send_frame_async(req->handle, fd, &confirm_frame);
        
        // wait a moment to ensure message is sent
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        esp_matter::factory_reset();
        
        // will never reach here (factory_reset() makes esp_restart())
    }
    else if (strcmp(cmd, "status") == 0) {
        char status_buf[128];
        snprintf(status_buf, sizeof(status_buf), 
                 "{\"type\":\"status\",\"pos\":%d,\"cal\":%s,\"inv\":%s}",
                 shutter_driver_get_current_percent(self->handle),
                 shutter_driver_is_calibrated(self->handle) ? "true" : "false",
                 shutter_driver_get_direction_inverted(self->handle) ? "true" : "false");
        
        httpd_ws_frame_t status_frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)status_buf,
            .len = strlen(status_buf)
        };
        httpd_ws_send_frame_async(req->handle, fd, &status_frame);
    }
    else if (strcmp(cmd, "matter_status") == 0) {
        uint8_t fabric_count = chip::Server::GetInstance().GetFabricTable().FabricCount();
        bool commissioned = Matter.isDeviceCommissioned() && (fabric_count > 0);
        
        String qrUrl = "";
        String qrImageUrl = "";
        String pairingCode = "";
        
        ESP_LOGI(TAG, "=== matter_status Command ===");
        ESP_LOGI(TAG, "Commissioned: %s", commissioned ? "true" : "false");
        ESP_LOGI(TAG, "Fabrics: %d", fabric_count);
        
        if (!commissioned) {
            String fullUrl = Matter.getOnboardingQRCodeUrl();
            pairingCode = Matter.getManualPairingCode();
            
            ESP_LOGI(TAG, "Full URL: %s", fullUrl.c_str());
            ESP_LOGI(TAG, "Pairing Code: %s", pairingCode.c_str());
            
            qrUrl = fullUrl;
            
            int dataIdx = fullUrl.indexOf("data=");
            if (dataIdx > 0) {
                String payload = fullUrl.substring(dataIdx + 5);
                
                qrImageUrl = "https://quickchart.io/qr?text=" + payload + "&size=300";
                
                ESP_LOGI(TAG, "Payload: %s", payload.c_str());
                ESP_LOGI(TAG, "Image URL: %s", qrImageUrl.c_str());
            } else {
                ESP_LOGW(TAG, "Could not find 'data=' in URL!");
            }
        }
        
        char matter_buf[768];
        int len = snprintf(matter_buf, sizeof(matter_buf),
                          "{\"type\":\"matter_status\","
                          "\"commissioned\":%s,"
                          "\"fabrics\":%d,"
                          "\"qr_url\":\"%s\","
                          "\"qr_image\":\"%s\","
                          "\"pairing_code\":\"%s\"}",
                          commissioned ? "true" : "false",
                          fabric_count,
                          qrUrl.c_str(),
                          qrImageUrl.c_str(),
                          pairingCode.c_str());
        
        ESP_LOGI(TAG, "Sending JSON (%d bytes): %s", len, matter_buf);
        
        httpd_ws_frame_t matter_frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)matter_buf,
            .len = (size_t)len
        };
        
        esp_err_t ret = httpd_ws_send_frame_async(req->handle, fd, &matter_frame);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send matter_status: %s", esp_err_to_name(ret));
        }
    }
    else if (strcmp(cmd, "info") == 0) {
        esp_chip_info_t chip;
        esp_chip_info(&chip);
        
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char chipid[18];
        snprintf(chipid, 18, "%02X:%02X:%02X:%02X:%02X:%02X", 
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        
        const esp_app_desc_t* app = esp_app_get_description();
        
        const char* reset_reason_str;
        switch (esp_reset_reason()) {
            case ESP_RST_POWERON:   reset_reason_str = "Power On"; break;
            case ESP_RST_SW:        reset_reason_str = "Software Reset"; break;
            case ESP_RST_PANIC:     reset_reason_str = "Exception/Panic"; break;
            case ESP_RST_INT_WDT:   reset_reason_str = "Interrupt WDT"; break;
            case ESP_RST_TASK_WDT:  reset_reason_str = "Task WDT"; break;
            case ESP_RST_WDT:       reset_reason_str = "Other WDT"; break;
            case ESP_RST_DEEPSLEEP: reset_reason_str = "Deep Sleep"; break;
            case ESP_RST_BROWNOUT:  reset_reason_str = "Brownout"; break;
            default:                reset_reason_str = "Unknown"; break;
        }
        
        uint32_t flash_size = 0;
        esp_flash_get_size(NULL, &flash_size);
        
        char info_buf[512];
        snprintf(info_buf, sizeof(info_buf),
                 "{\"type\":\"info\","
                 "\"chip\":\"%s\","
                 "\"uptime\":%llu,"
                 "\"heap\":%u,"
                 "\"minheap\":%u,"
                 "\"flash\":%u,"
                 "\"ver\":\"%s\","
                 "\"reset\":\"%s\"}",
                 chipid,
                 esp_timer_get_time() / 1000000,
                 esp_get_free_heap_size(),
                 esp_get_minimum_free_heap_size(),
                 flash_size,
                 app->version,
                 reset_reason_str);
        
        httpd_ws_frame_t info_frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)info_buf,
            .len = strlen(info_buf)
        };
        httpd_ws_send_frame_async(req->handle, fd, &info_frame);
    }
    
    // ============================================================================
    // BLE Commands
    // ============================================================================
    
    else if (strcmp(cmd, "ble_scan") == 0) {
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "WebSocket: BLE SCAN command received");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    
    if (self->bleManager) {
        ESP_LOGI(TAG, "✓ BLE Manager exists (pointer: %p)", (void*)self->bleManager);

        self->bleManager->startScan(10);
        
        ESP_LOGI(TAG, "Scan started, scheduling completion notification...");
        
        // ✓ Stack-Größe von 3072 auf 4096 erhöht!
        xTaskCreate([](void* param) {
            vTaskDelay(pdMS_TO_TICKS(11000));
            
            WebUIHandler* handler = (WebUIHandler*)param;
            
            // 1. Scan Complete
            const char* complete_msg = "{\"type\":\"ble_scan_complete\"}";
            handler->broadcast_to_all_clients(complete_msg);
            ESP_LOGI("WebUI", "Sent BLE scan complete notification");
            
            // 2. Discovery List (falls vorhanden)
            if (handler->bleManager) {
                vTaskDelay(pdMS_TO_TICKS(100));
                
                std::vector<ShellyBLEDevice> discovered = handler->bleManager->getDiscoveredDevices();
                
                if (discovered.size() > 0) {
                    // ✓ Statischer Buffer statt Stack-Allokation
                    static char json_buf[2048];  // ← WICHTIG: static!
                    int offset = snprintf(json_buf, sizeof(json_buf), 
                                          "{\"type\":\"ble_discovered\",\"devices\":[");
                    
                    for (size_t i = 0; i < discovered.size() && i < 10; i++) {  // Max 10 Geräte
                        offset += snprintf(json_buf + offset, sizeof(json_buf) - offset,
                                           "%s{\"name\":\"%s\",\"address\":\"%s\",\"rssi\":%d,\"encrypted\":%s}",
                                           i > 0 ? "," : "",
                                           discovered[i].name.c_str(),
                                           discovered[i].address.c_str(),
                                           discovered[i].rssi,
                                           discovered[i].isEncrypted ? "true" : "false");
                    }
                    snprintf(json_buf + offset, sizeof(json_buf) - offset, "]}");
                    
                    handler->broadcast_to_all_clients(json_buf);
                    ESP_LOGI("WebUI", "Sent %d discovered devices to clients", discovered.size());
                }
            }
            
            vTaskDelete(NULL);
        }, "ble_scan_notify", 4096, self, 1, NULL);
    }
}
    else if (strcmp(cmd, "ble_status") == 0) {
        if (self->bleManager) {
            ESP_LOGI(TAG, "BLE status requested");
            
            // Discovered Devices
            std::vector<ShellyBLEDevice> discovered = self->bleManager->getDiscoveredDevices();
            
            char json_buf[2048];
            int offset = snprintf(json_buf, sizeof(json_buf), 
                                  "{\"type\":\"ble_discovered\",\"devices\":[");
            
            for (size_t i = 0; i < discovered.size(); i++) {
                offset += snprintf(json_buf + offset, sizeof(json_buf) - offset,
                                   "%s{\"name\":\"%s\",\"address\":\"%s\",\"rssi\":%d,\"encrypted\":%s}",
                                   i > 0 ? "," : "",
                                   discovered[i].name.c_str(),
                                   discovered[i].address.c_str(),
                                   discovered[i].rssi,
                                   discovered[i].isEncrypted ? "true" : "false");
            }
            snprintf(json_buf + offset, sizeof(json_buf) - offset, "]}");
            
            httpd_ws_frame_t frame = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t*)json_buf,
                .len = strlen(json_buf)
            };
            httpd_ws_send_frame_async(req->handle, fd, &frame);
            
            // Paired Device Status
            bool paired = self->bleManager->isPaired();
            
            if (paired) {
                PairedShellyDevice device = self->bleManager->getPairedDevice();
                ShellyBLESensorData sensorData;
                bool hasData = self->bleManager->getSensorData(sensorData);
                
                offset = snprintf(json_buf, sizeof(json_buf),
                                 "{\"type\":\"ble_status\","
                                 "\"paired\":true,"
                                 "\"name\":\"%s\","
                                 "\"address\":\"%s\","
                                 "\"sensor_data\":{",
                                 device.name.c_str(),
                                 device.address.c_str());
                
                if (hasData) {
                    offset += snprintf(json_buf + offset, sizeof(json_buf) - offset,
                                      "\"valid\":true,"
                                      "\"packet_id\":%d,"
                                      "\"window_open\":%s,"
                                      "\"battery\":%d,"
                                      "\"illuminance\":%u,"
                                      "\"rotation\":%d,"
                                      "\"rssi\":%d,"
                                      "\"has_button_event\":%s,"
                                      "\"button_event\":%d,"
                                      "\"last_update\":%lu",
                                      sensorData.packetId,
                                      sensorData.windowOpen ? "true" : "false",
                                      sensorData.battery,
                                      sensorData.illuminance,
                                      sensorData.rotation,
                                      sensorData.rssi,
                                      sensorData.hasButtonEvent ? "true" : "false",
                                      (int)sensorData.buttonEvent,
                                      (unsigned long)sensorData.lastUpdate);
                } else {
                    offset += snprintf(json_buf + offset, sizeof(json_buf) - offset,
                                      "\"valid\":false");
                }
                
                snprintf(json_buf + offset, sizeof(json_buf) - offset, "}}");
            } else {
                snprintf(json_buf, sizeof(json_buf),
                        "{\"type\":\"ble_status\",\"paired\":false}");
            }
            
            frame.payload = (uint8_t*)json_buf;
            frame.len = strlen(json_buf);
            httpd_ws_send_frame_async(req->handle, fd, &frame);
        }
    }
    else if (strncmp(cmd, "{\"cmd\":\"ble_pair\"", 17) == 0) {
    if (self->bleManager) {
        String json = String(cmd);
        
        int addrStart = json.indexOf("\"address\":\"") + 11;
        int addrEnd = json.indexOf("\"", addrStart);
        String address = json.substring(addrStart, addrEnd);
        
        int bindStart = json.indexOf("\"bindkey\":\"") + 11;
        int bindEnd = json.indexOf("\"", bindStart);
        String bindkey = json.substring(bindStart, bindEnd);
        
        ESP_LOGI(TAG, "Pairing BLE device: %s", address.c_str());
        
        if (self->bleManager->pairDevice(address, bindkey)) {
            const char* success = "{\"type\":\"info\",\"message\":\"Device paired successfully\"}";
            httpd_ws_frame_t frame = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t*)success,
                .len = strlen(success)
            };
            httpd_ws_send_frame_async(req->handle, fd, &frame);
            
            ESP_LOGI(TAG, "BLE device paired successfully");
            
            ESP_LOGI(TAG, "Starting continuous scan for sensor data...");
            self->bleManager->startContinuousScan();
            
        } else {
            const char* error = "{\"type\":\"error\",\"message\":\"Failed to pair device\"}";
            httpd_ws_frame_t frame = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t*)error,
                .len = strlen(error)
            };
            httpd_ws_send_frame_async(req->handle, fd, &frame);
            
            ESP_LOGE(TAG, "Failed to pair BLE device");
        }
    }
}
else if (strncmp(cmd, "{\"cmd\":\"ble_unpair\"", 19) == 0) {
        if (self->bleManager) {
            ESP_LOGI(TAG, "Unpairing BLE device");
            
            if (self->bleManager->unpairDevice()) {
                if (self->remove_contact_sensor_callback) {
                    self->remove_contact_sensor_callback();
                } else {
                    ESP_LOGW(TAG, "No callback registered for endpoint removal");
                }
                
                const char* success = "{\"type\":\"info\",\"message\":\"Device unpaired\"}";
                httpd_ws_frame_t frame = {
                    .type = HTTPD_WS_TYPE_TEXT,
                    .payload = (uint8_t*)success,
                    .len = strlen(success)
                };
                httpd_ws_send_frame_async(req->handle, fd, &frame);
                
                ESP_LOGI(TAG, "✓ BLE device unpaired");
            }
        }
    }
    else {
        ESP_LOGW(TAG, "Unknown command: '%s'", cmd);
    }
    
    free(buf);
    return ESP_OK;
}

void WebUIHandler::remove_client(int fd) {
    unregister_client(fd);
}

void WebUIHandler::cleanup_idle_clients() {
    if (xSemaphoreTake(client_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        uint32_t now = millis();
        std::vector<int> timeout_fds;
        
        for (const auto& client : active_clients) {
            if (now - client.last_activity > WS_TIMEOUT_MS) {
                ESP_LOGW(TAG, "Client fd=%d timed out (idle for %u ms)", 
                         client.fd, now - client.last_activity);
                timeout_fds.push_back(client.fd);
            }
        }
        
        xSemaphoreGive(client_mutex);
        
        // Cleanup außerhalb des Mutex
        for (int fd : timeout_fds) {
            unregister_client(fd);
        }
        
        if (timeout_fds.size() > 0) {
            ESP_LOGI(TAG, "Cleaned up %d idle clients", timeout_fds.size());
        }
    }
  }
