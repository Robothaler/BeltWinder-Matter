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
#include <Preferences.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "web_ui_handler.h"

struct BLETaskParams {
    WebUIHandler* handler;
    int fd;
    String address;
    uint32_t passkey;
};

static const char* TAG = "WebUI";

static const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>BeltWinder Matter</title>
  <style>
    /* ============================================================================
       Base Styles & CSS Variables
       ============================================================================ */
    
    :root {
      --primary-color: #2196F3;
      --primary-light: #42A5F5;
      --success-color: #4CAF50;
      --warning-color: #FF9800;
      --error-color: #f44336;
      --bg-dark: #0a0a0a;
      --bg-card: rgba(255, 255, 255, 0.03);
      --border-color: rgba(255, 255, 255, 0.08);
      --text-primary: #e8e8e8;
      --text-secondary: #888;
      --shadow: 0 8px 32px rgba(0, 0, 0, 0.3);
      --transition: all 0.3s cubic-bezier(0.4, 0, 0.2, 1);
    }
    
    * { 
      margin: 0; 
      padding: 0; 
      box-sizing: border-box; 
    }
    
    /* ============================================================================
       Reduced Motion Support
       ============================================================================ */
    
    @media (prefers-reduced-motion: reduce) {
      *, *::before, *::after {
        animation-duration: 0.01ms !important;
        animation-iteration-count: 1 !important;
        transition-duration: 0.01ms !important;
      }
    }
    
    /* ============================================================================
       Body & Background
       ============================================================================ */
    
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, 'Inter', sans-serif;
      background: var(--bg-dark);
      color: var(--text-primary);
      line-height: 1.6;
      overflow-x: hidden;
      position: relative;
      min-height: 100vh;
    }
    
    /* Animated Background - Safari-kompatibel */
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
      will-change: transform, opacity;
      -webkit-backface-visibility: hidden; /* Safari Performance */
    }
    
    @keyframes bgShift {
      0%, 100% { opacity: 1; transform: scale(1) translateZ(0); }
      50% { opacity: 0.8; transform: scale(1.1) translateZ(0); }
    }
    
    /* ============================================================================
       Error Banner (Global)
       ============================================================================ */
    
    .error-banner {
      position: fixed;
      top: 20px;
      left: 50%;
      transform: translateX(-50%) translateY(-120%);
      background: linear-gradient(135deg, #f44336, #d32f2f);
      color: white;
      padding: 16px 24px;
      border-radius: 12px;
      box-shadow: 0 8px 32px rgba(244, 67, 54, 0.4);
      z-index: 9999;
      max-width: 90%;
      width: 500px;
      display: flex;
      align-items: center;
      gap: 12px;
      transition: transform 0.4s cubic-bezier(0.4, 0, 0.2, 1);
      will-change: transform;
    }
    
    .error-banner.show {
      transform: translateX(-50%) translateY(0);
    }
    
    .error-banner-icon {
      font-size: 1.5em;
      flex-shrink: 0;
    }
    
    .error-banner-content {
      flex: 1;
    }
    
    .error-banner-title {
      font-weight: 700;
      margin-bottom: 4px;
    }
    
    .error-banner-message {
      font-size: 0.9em;
      opacity: 0.9;
    }
    
    .error-banner-close {
      background: rgba(255, 255, 255, 0.2);
      border: none;
      color: white;
      width: 28px;
      height: 28px;
      border-radius: 50%;
      cursor: pointer;
      display: flex;
      align-items: center;
      justify-content: center;
      font-size: 1.2em;
      transition: background 0.3s;
    }
    
    .error-banner-close:hover {
      background: rgba(255, 255, 255, 0.3);
    }
    
    .error-banner.success {
      background: linear-gradient(135deg, #4CAF50, #66BB6A);
      box-shadow: 0 8px 32px rgba(76, 175, 80, 0.4);
    }
    
    .error-banner.warning {
      background: linear-gradient(135deg, #FF9800, #FFA726);
      box-shadow: 0 8px 32px rgba(255, 152, 0, 0.4);
    }
    
    /* ============================================================================
       Container & Layout
       ============================================================================ */
    
    .container { 
      max-width: 1000px; 
      margin: 0 auto; 
      padding: 20px;
      position: relative;
      z-index: 1;
    }
    
    /* ============================================================================
       Header (Glassmorphism)
       ============================================================================ */
    
    .header {
      text-align: center;
      margin-bottom: 30px;
      padding: 40px 30px;
      background: var(--bg-card);
      backdrop-filter: blur(20px);
      -webkit-backdrop-filter: blur(20px); /* Safari */
      border-radius: 24px;
      border: 1px solid var(--border-color);
      box-shadow: var(--shadow), inset 0 1px 0 rgba(255, 255, 255, 0.1);
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
      pointer-events: none;
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
      color: var(--text-secondary);
      position: relative;
      z-index: 1;
    }
    
    /* ============================================================================
       Navigation
       ============================================================================ */
    
    .nav {
      display: flex;
      justify-content: center;
      background: var(--bg-card);
      backdrop-filter: blur(20px);
      -webkit-backdrop-filter: blur(20px);
      padding: 8px;
      border-radius: 16px;
      margin-bottom: 30px;
      overflow-x: auto;
      gap: 8px;
      border: 1px solid rgba(255, 255, 255, 0.05);
      scrollbar-width: none;
      -ms-overflow-style: none;
    }
    
    .nav::-webkit-scrollbar { display: none; }
    
    .nav button {
      background: transparent;
      border: none;
      color: var(--text-secondary);
      font-size: 0.9em;
      padding: 12px 20px;
      cursor: pointer;
      transition: var(--transition);
      white-space: nowrap;
      border-radius: 12px;
      font-weight: 500;
      position: relative;
    }
    
    /* Accessibility: Focus State */
    .nav button:focus {
      outline: 2px solid var(--primary-color);
      outline-offset: 2px;
    }
    
    .nav button::before {
      content: '';
      position: absolute;
      inset: 0;
      border-radius: 12px;
      background: linear-gradient(135deg, var(--primary-color), #21CBF3);
      opacity: 0;
      transition: opacity 0.3s;
    }
    
    .nav button span {
      position: relative;
      z-index: 1;
    }
    
    .nav button:hover:not(:disabled) {
      color: var(--primary-color);
      transform: translateY(-2px);
    }
    
    .nav button.active {
      color: white;
      background: rgba(33, 150, 243, 0.2);
    }
    
    .nav button.active::before {
      opacity: 0.15;
    }
    
    .nav button:disabled {
      opacity: 0.4;
      cursor: not-allowed;
    }
    
    /* ============================================================================
       Cards
       ============================================================================ */
    
    .card {
      background: var(--bg-card);
      backdrop-filter: blur(20px);
      -webkit-backdrop-filter: blur(20px);
      border-radius: 20px;
      padding: 30px;
      margin-bottom: 25px;
      border: 1px solid var(--border-color);
      box-shadow: var(--shadow);
      transition: transform 0.3s, box-shadow 0.3s;
    }
    
    .card:hover {
      transform: translateY(-4px);
      box-shadow: 0 12px 48px rgba(0, 0, 0, 0.4);
    }
    
    .card h2 { 
      font-size: 1.4em;
      margin-bottom: 20px;
      background: linear-gradient(135deg, var(--primary-color), #21CBF3);
      -webkit-background-clip: text;
      -webkit-text-fill-color: transparent;
      background-clip: text;
      font-weight: 600;
    }
    
    /* ============================================================================
       Status Grid
       ============================================================================ */
    
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
      transition: var(--transition);
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
      transition: left 0.5s;
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
      color: var(--text-secondary);
      text-transform: uppercase;
      letter-spacing: 1px;
      font-weight: 600;
    }
    
    .status-value { 
      font-size: 1.6em;
      font-weight: 700;
      margin-top: 8px;
      transition: var(--transition);
    }
    
    .status-value.commissioned { 
      color: var(--success-color);
      text-shadow: 0 0 20px rgba(76, 175, 80, 0.5);
    }
    
    .status-value.not-commissioned { 
      color: var(--warning-color);
      text-shadow: 0 0 20px rgba(255, 152, 0, 0.5);
    }
    
    /* ============================================================================
       Buttons
       ============================================================================ */
    
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
      transition: var(--transition);
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 8px;
      font-weight: 600;
      position: relative;
      overflow: hidden;
    }
    
    /* Accessibility: Focus State */
    .btn:focus {
      outline: 2px solid var(--primary-color);
      outline-offset: 2px;
    }
    
    .btn::before {
      content: '';
      position: absolute;
      inset: 0;
      background: linear-gradient(135deg, #4CAF50, #66BB6A);
      opacity: 0;
      transition: opacity 0.3s;
    }
    
    .btn span {
      position: relative;
      z-index: 1;
    }
    
    .btn:hover:not(:disabled) {
      transform: translateY(-3px) scale(1.02);
      box-shadow: 0 8px 24px rgba(76, 175, 80, 0.4);
      border-color: #4CAF50;
    }
    
    .btn:hover:not(:disabled)::before {
      opacity: 0.3;
    }
    
    .btn:active:not(:disabled) {
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
    
    .btn.stop:hover:not(:disabled), .btn.danger:hover:not(:disabled) {
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
    
    .btn.secondary:hover:not(:disabled) {
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
    
    .btn.primary:hover:not(:disabled) {
      box-shadow: 0 8px 24px rgba(33, 150, 243, 0.4);
      border-color: #2196F3;
    }
    
    .btn.primary::before {
      background: linear-gradient(135deg, #2196F3, #42A5F5);
    }
    
    /* ============================================================================
       Direction Selector
       ============================================================================ */
    
    .direction-btn {
      padding: 20px;
      font-size: 1em;
      text-align: center;
      border-radius: 14px;
      cursor: pointer;
      transition: var(--transition);
      border: 2px solid rgba(255, 255, 255, 0.1);
      background: rgba(255, 255, 255, 0.03);
    }
    
    .direction-btn:focus {
      outline: 2px solid var(--primary-color);
      outline-offset: 2px;
    }
    
    .direction-btn.active { 
      border-color: var(--success-color);
      background: rgba(76, 175, 80, 0.15);
      box-shadow: 0 0 30px rgba(76, 175, 80, 0.3);
    }
    
    .direction-btn:hover:not(.active) { 
      background: rgba(255, 255, 255, 0.06);
      transform: translateY(-2px);
    }
    
    /* ============================================================================
       Info Table
       ============================================================================ */
    
    .info-table {
      width: 100%;
      border-collapse: collapse;
      font-size: 0.95em;
    }
    
    .info-table tr { 
      border-bottom: 1px solid rgba(255, 255, 255, 0.05);
      transition: background 0.3s;
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
    
    /* ============================================================================
       Modal Design
       ============================================================================ */
    
    .modal {
      position: fixed;
      top: 0;
      left: 0;
      width: 100%;
      height: 100%;
      background: rgba(0, 0, 0, 0.85);
      backdrop-filter: blur(10px);
      -webkit-backdrop-filter: blur(10px);
      align-items: center;
      justify-content: center;
      z-index: 1000;
      padding: 20px;
      overflow-y: auto;
      animation: modalFadeIn 0.3s;
    }
    
    @keyframes modalFadeIn {
      from { opacity: 0; }
      to { opacity: 1; }
    }
    
    .modal:not(.hidden) { display: flex; }
    
    .modal-box {
      background: rgba(20, 20, 20, 0.95);
      backdrop-filter: blur(20px);
      -webkit-backdrop-filter: blur(20px);
      padding: 35px;
      border-radius: 24px;
      max-width: 500px;
      width: 100%;
      max-height: 90vh;
      overflow-y: auto;
      border: 1px solid rgba(255, 255, 255, 0.1);
      box-shadow: 0 20px 60px rgba(0, 0, 0, 0.5);
      animation: modalSlideUp 0.3s cubic-bezier(0.4, 0, 0.2, 1);
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
      background: linear-gradient(135deg, var(--primary-color), #21CBF3);
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
      transition: var(--transition);
    }
    
    .modal-btn:focus {
      outline: 2px solid var(--primary-color);
      outline-offset: 2px;
    }
    
    .modal-btn-primary { 
      background: linear-gradient(135deg, var(--primary-color), #1976D2);
      color: white;
    }
    
    .modal-btn-primary:hover:not(:disabled) { 
      transform: translateY(-2px);
      box-shadow: 0 6px 20px rgba(33, 150, 243, 0.4);
    }
    
    .modal-btn-primary:disabled {
      opacity: 0.4;
      cursor: not-allowed;
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
      background: linear-gradient(135deg, var(--error-color), #d32f2f);
      color: white;
    }
    
    .modal-btn-danger:hover { 
      transform: translateY(-2px);
      box-shadow: 0 6px 20px rgba(244, 67, 54, 0.4);
    }
    
    /* ============================================================================
       QR Code Styling
       ============================================================================ */
    
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
      color: var(--success-color);
      margin: 20px 0;
      padding: 18px;
      background: rgba(76, 175, 80, 0.1);
      border-radius: 12px;
      letter-spacing: 3px;
      border: 1px solid rgba(76, 175, 80, 0.3);
    }
    
    /* ============================================================================
       Alert Styles
       ============================================================================ */
    
    .alert {
      padding: 18px;
      border-radius: 12px;
      margin: 20px 0;
      background: rgba(255, 255, 255, 0.05);
      border-left: 4px solid var(--primary-color);
      line-height: 1.6;
    }
    
    .alert.warning { border-left-color: var(--warning-color); }
    .alert.error { border-left-color: var(--error-color); }
    .alert.success { border-left-color: var(--success-color); }
    
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
      background: linear-gradient(135deg, var(--success-color), #66BB6A);
      color: white;
      box-shadow: 0 0 20px rgba(76, 175, 80, 0.4);
    }
    
    .badge.not-commissioned { 
      background: linear-gradient(135deg, var(--warning-color), #FFA726);
      color: white;
      box-shadow: 0 0 20px rgba(255, 152, 0, 0.4);
    }
    
    /* ============================================================================
       BLE Device List
       ============================================================================ */
    
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
      transition: var(--transition);
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
      color: var(--text-secondary);
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
    
    .signal-excellent { background: var(--success-color); color: white; }
    .signal-good { background: #8BC34A; color: white; }
    .signal-fair { background: var(--warning-color); color: white; }
    .signal-poor { background: var(--error-color); color: white; }
    
    /* ============================================================================
       Input Groups
       ============================================================================ */
    
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
      transition: var(--transition);
    }
    
    .input-group input:focus {
      outline: none;
      border-color: var(--primary-color);
      box-shadow: 0 0 0 3px rgba(33, 150, 243, 0.1);
      background: rgba(255, 255, 255, 0.08);
    }
    
    .input-group input:disabled {
      opacity: 0.5;
      cursor: not-allowed;
    }
    
    /* ============================================================================
       Spinner
       ============================================================================ */
    
    .spinner {
      display: inline-block;
      width: 20px;
      height: 20px;
      border: 3px solid rgba(255, 255, 255, 0.2);
      border-top-color: var(--primary-color);
      border-radius: 50%;
      animation: spin 1s linear infinite;
    }
    
    @keyframes spin {
      to { transform: rotate(360deg); }
    }
    
    /* ============================================================================
       Scrollbar Styling
       ============================================================================ */
    
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
    
    /* Firefox Scrollbar */
    * {
      scrollbar-width: thin;
      scrollbar-color: rgba(255, 255, 255, 0.1) rgba(255, 255, 255, 0.02);
    }
    
    /* ============================================================================
       Responsive Design
       ============================================================================ */
    
    @media (max-width: 768px) {
      .header h1 { font-size: 2em; }
      .status-grid { grid-template-columns: 1fr 1fr; }
      .btn-grid { grid-template-columns: 1fr; }
      .nav { justify-content: flex-start; }
      .modal-box { padding: 25px; }
      .device-item { flex-direction: column; align-items: flex-start; }
      .device-actions { width: 100%; justify-content: stretch; }
      .device-actions .btn { flex: 1; }
    }
    
    @media (max-width: 480px) {
      .container { padding: 15px; }
      .header { padding: 30px 20px; }
      .card { padding: 20px; }
      .status-grid { grid-template-columns: 1fr; }
    }
    
    /* ============================================================================
       Loading State
       ============================================================================ */
    
    .loading-overlay {
      position: fixed;
      top: 0;
      left: 0;
      width: 100%;
      height: 100%;
      background: rgba(0, 0, 0, 0.9);
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      z-index: 10000;
      opacity: 0;
      pointer-events: none;
      transition: opacity 0.3s;
    }
    
    .loading-overlay.show {
      opacity: 1;
      pointer-events: all;
    }
    
    .loading-spinner {
      width: 60px;
      height: 60px;
      border: 4px solid rgba(255, 255, 255, 0.1);
      border-top-color: var(--primary-color);
      border-radius: 50%;
      animation: spin 1s linear infinite;
    }
    
    .loading-text {
      margin-top: 20px;
      font-size: 1.1em;
      color: var(--text-primary);
    }
  </style>
</head>
<body>
  <!-- ============================================================================
       Error Banner (Global)
       ============================================================================ -->
  
  <div id="error-banner" class="error-banner">
    <div class="error-banner-icon">⚠️</div>
    <div class="error-banner-content">
      <div class="error-banner-title" id="error-banner-title">Error</div>
      <div class="error-banner-message" id="error-banner-message">Something went wrong</div>
    </div>
    <button class="error-banner-close" onclick="hideErrorBanner()" aria-label="Close error message">×</button>
  </div>

  <!-- ============================================================================
       Loading Overlay
       ============================================================================ -->
  
  <div id="loading-overlay" class="loading-overlay">
    <div class="loading-spinner"></div>
    <div class="loading-text" id="loading-text">Loading...</div>
  </div>

  <!-- ============================================================================
       Main Container
       ============================================================================ -->
  
  <div class="container">
    <div class="header">
      <h1>🎚️ BeltWinder Matter</h1>
      <div class="subtitle">Smart Shutter Control System</div>
    </div>

    <!-- ============================================================================
         Navigation
         ============================================================================ -->
    
    <nav class="nav" role="navigation" aria-label="Main navigation">
      <button onclick="show('overview')" class="active" id="nav-overview" aria-label="Overview tab">
        <span>📊 Overview</span>
      </button>
      <button onclick="show('matter')" id="nav-matter" aria-label="Matter settings tab">
        <span>🔗 Matter</span>
      </button>
      <button onclick="show('ble')" id="nav-ble" aria-label="BLE sensor tab">
        <span>📡 BLE Sensor</span>
      </button>
      <button onclick="show('system')" id="nav-system" aria-label="System information tab">
        <span>💻 System</span>
      </button>
      <button onclick="show('settings')" id="nav-settings" aria-label="Settings tab">
        <span>⚙️ Settings</span>
      </button>
    </nav>

    <!-- ============================================================================
         Overview Tab
         ============================================================================ -->
    
    <div id="overview" role="tabpanel" aria-labelledby="nav-overview">
      <div class="card">
        <h2>Status</h2>
        <div class="status-grid">
          <div class="status-item">
            <div class="status-label">Position</div>
            <div class="status-value" id="pos" aria-live="polite">0%</div>
          </div>
          <div class="status-item">
            <div class="status-label">Calibrated</div>
            <div class="status-value" id="calib" aria-live="polite">No</div>
          </div>
          <div class="status-item">
            <div class="status-label">Direction</div>
            <div class="status-value" id="inv" aria-live="polite">Normal</div>
          </div>
          <div class="status-item">
            <div class="status-label">Matter Status</div>
            <div class="status-value" id="matter-status" aria-live="polite">Checking...</div>
          </div>
        </div>
        <div class="btn-grid">
          <button class="btn" onclick="send('up')" aria-label="Move shutter up">
            <span>⬆ UP</span>
          </button>
          <button class="btn" onclick="send('down')" aria-label="Move shutter down">
            <span>⬇ DOWN</span>
          </button>
          <button class="btn stop" onclick="send('stop')" aria-label="Stop shutter movement">
            <span>⏹ STOP</span>
          </button>
        </div>
      </div>
    </div>

    <!-- ============================================================================
         Matter Tab
         ============================================================================ -->
    
    <div id="matter" class="hidden" role="tabpanel" aria-labelledby="nav-matter">
      <div class="card">
        <h2>Matter Pairing Information</h2>
        <div id="matter-status-detail" aria-live="polite"></div>
        <div id="matter-pairing-info"></div>
      </div>
    </div>

    <!-- ============================================================================
         BLE Sensor Tab
         ============================================================================ -->
    
    <div id="ble" class="hidden" role="tabpanel" aria-labelledby="nav-ble">
      <div class="card">
        <h2>BLE Window Sensor</h2>
        
        <!-- Current Sensor Status -->
        <div id="ble-sensor-status" class="hidden">
          <h3 style="margin-top:0;color:#888">Current Sensor</h3>
          
          <!-- State Indicator (dynamically inserted) -->
          
          <div class="status-grid" style="grid-template-columns: repeat(auto-fit, minmax(120px, 1fr)); margin-bottom: 15px;">
            <div class="status-item">
              <div class="status-label">Contact</div>
              <div class="status-value" id="ble-contact" aria-live="polite">Unknown</div>
            </div>
            <div class="status-item">
              <div class="status-label">Battery</div>
              <div class="status-value" id="ble-battery" aria-live="polite">--%</div>
            </div>
            <div class="status-item">
              <div class="status-label">Signal</div>
              <div class="status-value" id="ble-rssi" aria-live="polite">-- dBm</div>
            </div>
          </div>
          
          <div class="status-grid" style="grid-template-columns: repeat(auto-fit, minmax(120px, 1fr)); margin-bottom: 20px;">
            <div class="status-item">
              <div class="status-label">Brightness</div>
              <div class="status-value" id="ble-lux" aria-live="polite">-- lux</div>
            </div>
            <div class="status-item">
              <div class="status-label">Rotation</div>
              <div class="status-value" id="ble-rotation" aria-live="polite">--°</div>
            </div>
            <div class="status-item">
              <div class="status-label">Last Update</div>
              <div class="status-value" id="ble-last-update" aria-live="polite">Never</div>
            </div>
          </div>

          <div class="status-grid" style="grid-template-columns: repeat(auto-fit, minmax(120px, 1fr)); margin-bottom: 15px;">
            <div class="status-item">
              <div class="status-label">Packet ID</div>
              <div class="status-value" id="ble-packet-id" aria-live="polite">--</div>
            </div>
            <div class="status-item" id="ble-button-container" style="display:none">
              <div class="status-label">Button Event</div>
              <div class="status-value" id="ble-button-event" aria-live="polite">None</div>
            </div>
          </div>

          <div id="ble-continuous-scan-control" style="margin-top:25px">
          <h3 style="margin:0 0 15px 0;color:#888;font-size:1.1em;font-weight:600">
            📡 Continuous Scan Control
          </h3>
          
          <div style="background:rgba(255,255,255,0.04);padding:18px;border-radius:14px;border:1px solid rgba(255,255,255,0.08)">
            <div style="display:flex;align-items:center;justify-content:space-between;gap:15px;flex-wrap:wrap">
              <div style="flex:1;min-width:200px">
                <div style="font-weight:600;font-size:1.05em;margin-bottom:5px">
                  Scan Status: <span id="continuous-scan-status" style="color:#888">Checking...</span>
                </div>
                <div style="font-size:0.85em;color:#666">
                  Monitors device for door open/close events
                </div>
              </div>
              
              <div style="display:flex;gap:10px">
                <button class="btn primary" id="start-continuous-scan-btn" 
                        onclick="startContinuousScanManual()" 
                        style="padding:12px 20px;display:none">
                  <span>▶️ Start Scan</span>
                </button>
                
                <button class="btn danger" id="stop-continuous-scan-btn" 
                        onclick="stopContinuousScanManual()" 
                        style="padding:12px 20px;display:none">
                  <span>⏹️ Stop Scan</span>
                </button>
              </div>
            </div>
          </div>
          
          <div class="alert" style="margin-top:15px">
            <strong>ℹ️ About Continuous Scan</strong>
            <p style="margin-top:8px;line-height:1.6">
              • Auto-starts after pairing and device reboot<br>
              • Listens for door open/close events (event-driven)<br>
              • No battery drain on sensor (passive listening)<br>
              • Can be manually stopped/started anytime
            </p>
          </div>
        </div>
          
          <div style="background:rgba(255,255,255,0.04);padding:18px;border-radius:14px;margin-bottom:15px;border:1px solid rgba(255,255,255,0.08)">
            <div style="display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:15px">
              <div style="flex:1;min-width:200px">
                <div style="font-weight:bold;font-size:1.1em" id="ble-device-name">Unknown Device</div>
                <div style="color:#888;font-size:0.9em;margin-top:5px" id="ble-device-address">--:--:--:--:--:--</div>
              </div>
              <button class="btn danger" onclick="unpairBLE()" style="padding:12px 24px" aria-label="Unpair BLE device">
                <span>🔓 Unpair</span>
              </button>
            </div>
          </div>
        </div>

          <!-- Security Info Block -->
          <div id="ble-security-info" style="background:rgba(76,175,80,0.1);padding:18px;border-radius:14px;margin-top:20px;border:1px solid rgba(76,175,80,0.3);display:none">
            <h3 style="margin:0 0 15px 0;color:#4CAF50;font-size:1.1em;font-weight:600">🔐 Security Information</h3>
            
            <div style="display:grid;grid-template-columns:1fr 1fr;gap:15px">
              <div style="background:rgba(255,255,255,0.05);padding:12px;border-radius:10px">
                <div style="font-size:0.8em;color:#888;margin-bottom:5px">Passkey</div>
                <div id="ble-passkey" style="font-family:'Courier New',monospace;font-size:1.2em;font-weight:bold;color:#4CAF50">------</div>
              </div>
              
              <div style="background:rgba(255,255,255,0.05);padding:12px;border-radius:10px">
                <div style="font-size:0.8em;color:#888;margin-bottom:5px">Encryption</div>
                <div id="ble-encryption-status" style="font-size:1.1em;font-weight:bold">Checking...</div>
              </div>
            </div>
            
            <div style="background:rgba(255,255,255,0.05);padding:12px;border-radius:10px;margin-top:15px">
              <div style="font-size:0.8em;color:#888;margin-bottom:5px">Bindkey (32 hex characters)</div>
              <div id="ble-bindkey" style="font-family:'Courier New',monospace;font-size:0.9em;word-break:break-all;color:#e0e0e0">Not available</div>
            </div>
            
            <div style="margin-top:15px;padding:12px;background:rgba(255,152,0,0.1);border-radius:10px;border:1px solid rgba(255,152,0,0.3)">
              <div style="font-size:0.85em;color:#FF9800">
                <strong>⚠️ Important:</strong> Save these credentials in a secure location! You'll need them for re-pairing after factory reset.
              </div>
            </div>
          </div>

        <!-- Matter Integration Toggle -->
        <div id="ble-matter-toggle" class="hidden" style="margin-top:25px">
          <div class="card">
            <h3 style="margin-top:0;color:#888;font-size:1.1em">Matter Integration</h3>
            
            <div class="alert">
              <strong>ℹ️ Contact Sensor for Matter</strong>
              <p style="margin-top:10px;line-height:1.6">
                When enabled, the BLE sensor's contact state, battery, and illuminance will be 
                exposed as a Matter Contact Sensor endpoint. This allows control via 
                Apple Home, Google Home, Alexa, etc.
              </p>
              <p style="margin-top:10px;color:#888">
                <strong>Note:</strong> The sensor will still control the shutter logic 
                regardless of this setting.
              </p>
            </div>
            
            <div style="display:grid;grid-template-columns:1fr 1fr;gap:15px;margin-top:20px">
              <div class="direction-btn" id="matter-toggle-off" onclick="setContactSensorMatter(false)" 
                   tabindex="0" role="button" aria-pressed="false" aria-label="Disable Matter integration">
                <div style="font-size:2em;margin-bottom:8px">❌</div>
                <div style="font-weight:bold;font-size:1.1em">Matter OFF</div>
                <div style="font-size:0.85em;color:#888;margin-top:5px">Only for shutter control</div>
              </div>
              <div class="direction-btn" id="matter-toggle-on" onclick="setContactSensorMatter(true)" 
                   tabindex="0" role="button" aria-pressed="false" aria-label="Enable Matter integration">
                <div style="font-size:2em;margin-bottom:8px">✅</div>
                <div style="font-weight:bold;font-size:1.1em">Matter ON</div>
                <div style="font-size:0.85em;color:#888;margin-top:5px">Expose to Matter ecosystem</div>
              </div>
            </div>
            
            <div id="matter-toggle-status" style="margin-top:20px;padding:15px;background:rgba(255,255,255,0.04);border-radius:12px;text-align:center">
              <div style="font-size:0.9em;color:#888">Status</div>
              <div id="matter-toggle-status-text" style="font-size:1.2em;font-weight:bold;margin-top:5px" aria-live="polite">
                Loading...
              </div>
            </div>
          </div>
        </div>
        
        <!-- Scan Interface -->
        <div id="ble-scan-interface">
          <p style="color:#888;margin-bottom:20px;line-height:1.8">
            Search for nearby Shelly BLU Door/Window sensors to integrate window state detection with your smart shutter.
          </p>
          
          <button class="btn primary" id="ble-scan-btn" onclick="startBLEScan()" style="width:100%;margin-bottom:25px" aria-label="Start BLE scan">
            <span>🔍 Start Scan</span>
          </button>
          
          <h3 style="margin-top:30px;color:#888;font-size:1.1em;font-weight:600">Discovered Devices</h3>
          <ul class="device-list" id="ble-discovered-devices" role="list">
            <li style="text-align:center;color:#666;padding:40px 20px">
              <div style="font-size:3em;margin-bottom:10px;opacity:0.3">📡</div>
              <div>No scan performed yet</div>
            </li>
          </ul>
        </div>
      </div>
    </div>

    <!-- ============================================================================
         System Tab
         ============================================================================ -->
    
    <div id="system" class="hidden" role="tabpanel" aria-labelledby="nav-system">
      <div class="card">
        <h2>System Information</h2>
        <table class="info-table" id="sysinfo" role="table">
          <tbody></tbody>
        </table>
      </div>
    </div>

    <!-- ============================================================================
         Settings Tab
         ============================================================================ -->
    
    <div id="settings" class="hidden" role="tabpanel" aria-labelledby="nav-settings">
      <div class="card">
        <h2>Calibration</h2>
        <p style="color:#888;margin-bottom:20px;line-height:1.8">
          Calibrate the shutter to learn its full travel range. The system will automatically move the shutter up and down to determine minimum and maximum positions.
        </p>
        <button class="btn primary" onclick="send('calibrate')" style="width:100%" aria-label="Start calibration">
          <span>🎯 START CALIBRATION</span>
        </button>
      </div>

      <div class="card">
        <h2>Direction Control</h2>
        <p style="color:#888;margin-bottom:20px;line-height:1.8">
          Change motor direction if UP/DOWN controls are reversed. This swaps the polarity without rewiring.
        </p>
        <div style="display:grid;grid-template-columns:1fr 1fr;gap:15px">
          <div class="direction-btn" id="dir-normal" onclick="setDirection(false)" 
               tabindex="0" role="button" aria-pressed="false" aria-label="Set direction to normal">
            <div style="font-size:2em;margin-bottom:8px">✅</div>
            <div style="font-weight:bold;font-size:1.1em">Normal</div>
            <div style="font-size:0.85em;color:#888;margin-top:5px">Standard direction</div>
          </div>
          <div class="direction-btn" id="dir-inverted" onclick="setDirection(true)" 
               tabindex="0" role="button" aria-pressed="false" aria-label="Set direction to inverted">
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
        <button class="btn danger" onclick="showConfirm()" style="width:100%" aria-label="Factory reset device">
          <span>🗑️ FACTORY RESET</span>
        </button>
      </div>
    </div>
    
  </div>

  <!-- ============================================================================
       Factory Reset Confirmation Modal
       ============================================================================ -->
  
  <div id="confirm-reset" class="modal hidden" role="dialog" aria-labelledby="reset-modal-title" aria-modal="true">
    <div class="modal-box">
      <h3 id="reset-modal-title">⚠️ Confirm Factory Reset</h3>
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
        <button class="modal-btn modal-btn-secondary" onclick="hideConfirm()" aria-label="Cancel factory reset">Cancel</button>
        <button class="modal-btn modal-btn-danger" onclick="confirmReset()" aria-label="Confirm factory reset">Reset Device</button>
      </div>
    </div>
  </div>

  <!-- ============================================================================
       Matter QR Code Modal
       ============================================================================ -->
  
  <div id="matter-qr-modal" class="modal hidden" role="dialog" aria-labelledby="qr-modal-title" aria-modal="true">
    <div class="modal-box">
      <h3 id="qr-modal-title">🔗 Matter Pairing</h3>
      <div class="alert success">
        <strong>✓ Device is ready!</strong>
        <p style="margin-top:8px">Scan the QR code below with your Matter-compatible controller (Apple Home, Google Home, Alexa, etc.)</p>
      </div>
      
      <div class="qr-container">
        <img class="qr-code" id="matter-qr-img" src="" alt="Matter pairing QR code">
        <div style="margin-top:15px;color:#888;font-size:0.9em">Scan with your Matter controller app</div>
        <a id="qr-web-link" href="" target="_blank" rel="noopener noreferrer"
          style="display:inline-block;margin-top:15px;color:#2196F3;text-decoration:none;font-weight:600;transition:all .3s"
          aria-label="Open QR code in CHIP Tool QR Generator">
          🌐 Open in CHIP Tool QR Generator
        </a>
      </div>
      
      <div style="margin-top:25px">
        <div style="color:#888;margin-bottom:12px;font-weight:600;font-size:0.9em;text-transform:uppercase;letter-spacing:1px">Manual Pairing Code:</div>
        <div class="pairing-code" id="matter-pairing-code" aria-label="Manual pairing code">Loading...</div>
        <div style="color:#666;font-size:0.85em;text-align:center;margin-top:10px">
          Enter this code if your controller doesn't support QR scanning
        </div>
      </div>
      
      <div class="modal-buttons">
        <button class="modal-btn modal-btn-secondary" onclick="closeMatterQR()" aria-label="Close pairing information">Close</button>
      </div>
    </div>
  </div>

  <!-- ============================================================================
     BLE ENCRYPTED DEVICE PAIRING MODAL (Already Encrypted + Bindkey Known)
     ============================================================================ -->

  <div id="ble-encrypted-known-modal" class="modal hidden" role="dialog" aria-labelledby="enc-known-modal-title" aria-modal="true">
    <div class="modal-box">
      <h3 id="enc-known-modal-title">🔐 Pair Already-Encrypted Device</h3>
      
      <div class="alert success">
        <strong>✓ Device is Already Encrypted</strong>
        <p style="margin-top:10px">
          This device was previously encrypted (e.g., with the Shelly App).
          To connect, you need BOTH the passkey AND the bindkey from the original pairing.
        </p>
      </div>
      
      <div class="input-group">
        <label for="enc-known-device-name">Device Name</label>
        <input type="text" id="enc-known-device-name" readonly aria-readonly="true">
        </div>
            <div class="input-group">
        <label for="enc-known-device-address">MAC Address</label>
        <input type="text" id="enc-known-device-address" readonly aria-readonly="true">
      </div>
      
      <div class="input-group">
        <label for="enc-known-passkey">Passkey (6 digits, Required)</label>
        <input type="number" 
              id="enc-known-passkey" 
              placeholder="123456" 
              min="0"
              max="999999"
              oninput="validateEncryptedKnownForm()"
              aria-required="true"
              aria-describedby="enc-known-passkey-help">
        <div id="enc-known-passkey-help" style="font-size:0.85em;color:#888;margin-top:5px">
          Enter the 6-digit passkey that was set during initial pairing
        </div>
      </div>
      
      <div class="input-group">
        <label for="enc-known-bindkey">Bindkey (32 hex characters, Required)</label>
        <input type="text" 
              id="enc-known-bindkey" 
              placeholder="a1b2c3d4e5f6..." 
              maxlength="32"
              oninput="validateEncryptedKnownForm()"
              aria-required="true"
              aria-describedby="enc-known-bindkey-help"
              style="font-family:'Courier New',monospace">
        <div id="enc-known-bindkey-help" style="font-size:0.85em;color:#888;margin-top:5px">
          Enter the 32-character bindkey (0-9, a-f)
        </div>
      </div>
      
      <div class="alert warning">
        <strong>📝 Where to find these values?</strong>
        <ul style="margin:10px 0 0 20px;line-height:1.6">
          <li>Passkey: Set by you during initial pairing (default: 123456)</li>
          <li>Bindkey: Shown in Shelly App after encryption, or saved from previous ESP32 pairing</li>
        </ul>
      </div>
      
      <div class="alert">
        <strong>🔒 Secure Bonding Process</strong>
        <p style="margin-top:10px">
          The ESP32 will:
        </p>
        <ol style="margin:8px 0 0 20px;line-height:1.6">
          <li>Establish secure bonded connection</li>
          <li>Store passkey and bindkey in NVS</li>
          <li>Decrypt broadcasts automatically</li>
          <li>Start continuous scan for sensor data</li>
        </ol>
        <p style="margin-top:10px;color:#888">
          <strong>Note:</strong> NO button press needed - device is already encrypted
        </p>
      </div>
      
      <div class="modal-buttons">
        <button class="modal-btn modal-btn-secondary" onclick="closeEncryptedKnownModal()">Cancel</button>
        <button class="modal-btn modal-btn-primary" id="enc-known-confirm-btn" 
                onclick="confirmEncryptedKnownPair()" disabled>
          Pair Device
        </button>
      </div>
    </div>
  </div>

  <!-- ============================================================================
     BLE SMART CONNECT MODAL (Bonding + Optional Encryption)
     ============================================================================ -->

  <div id="ble-connect-modal" class="modal hidden" role="dialog" aria-labelledby="connect-modal-title" aria-modal="true">
    <div class="modal-box">
      <h3 id="connect-modal-title">🔗 Connect Device</h3>
      
      <div class="input-group">
        <label for="connect-device-name">Device Name</label>
        <input type="text" id="connect-device-name" readonly aria-readonly="true">
      </div>
      
      <div class="input-group">
        <label for="connect-device-address">MAC Address</label>
        <input type="text" id="connect-device-address" readonly aria-readonly="true">
      </div>

      <!-- ✅ NEU: Encryption Mode Selection -->
      <div style="margin: 25px 0;">
        <label style="display:block;margin-bottom:15px;font-weight:600;color:#888;text-transform:uppercase;font-size:0.9em;letter-spacing:1px">
          Connection Mode
        </label>
        
        <div style="display:grid;grid-template-columns:1fr 1fr;gap:15px;margin-bottom:20px">
          <div class="direction-btn active" id="connect-mode-unencrypted" 
              onclick="selectConnectMode('unencrypted')" 
              tabindex="0" role="button" aria-pressed="true">
            <div style="font-size:2em;margin-bottom:8px">🔓</div>
            <div style="font-weight:bold">Unencrypted</div>
            <div style="font-size:0.85em;color:#888;margin-top:5px">Connect only<br>(Enable encryption later)</div>
          </div>
          
          <div class="direction-btn" id="connect-mode-encrypted" 
              onclick="selectConnectMode('encrypted')" 
              tabindex="0" role="button" aria-pressed="false">
            <div style="font-size:2em;margin-bottom:8px">🔐</div>
            <div style="font-weight:bold">Encrypted</div>
            <div style="font-size:0.85em;color:#888;margin-top:5px">Set passkey now<br>(All-in-one)</div>
          </div>
        </div>
      </div>

      <!-- Passkey Eingabe (nur bei Encrypted Mode) -->
      <div id="connect-passkey-section" style="display:none">
        <div class="alert warning">
          <strong>🔐 Direct Encryption</strong>
          <p style="margin-top:10px">
            Device will be bonded AND encrypted in one step.
            Choose a 6-digit passkey to secure your device.
          </p>
        </div>
        
        <div class="input-group">
          <label for="connect-passkey">Passkey (6 digits, 0-999999)</label>
          <input type="number" 
                id="connect-passkey" 
                placeholder="123456" 
                min="0" 
                max="999999"
                oninput="validateConnectPasskey()"
                aria-required="true">
        </div>
        
        <div class="input-group">
          <label for="connect-passkey-confirm">Confirm Passkey</label>
          <input type="number" 
                id="connect-passkey-confirm" 
                placeholder="123456" 
                min="0" 
                max="999999"
                oninput="validateConnectPasskey()"
                aria-required="true">
        </div>
      </div>

      <!-- Button Press Instructions (nur bei Unencrypted) -->
      <div id="connect-button-instructions">
        <div class="alert error">
          <strong>⚠️ CRITICAL: Button Press Required!</strong>
          <p style="margin-top:10px;line-height:1.8">
            The Shelly device has <strong>very specific timing requirements</strong>:
          </p>
        </div>
        
        <div class="alert warning">
          <strong>📋 Step-by-Step Instructions</strong>
          <ol style="margin:10px 0 0 20px;line-height:2">
            <li><strong>BEFORE clicking "Connect":</strong>
              <ul style="margin-left:20px;margin-top:5px">
                <li>Press and HOLD the button on the Shelly device</li>
              </ul>
            </li>
            <li><strong>Keep holding for</strong>
              <ul style="margin-left:20px;margin-top:5px">
                <li>at least 10 seconds</li>
              </ul>
            </li>
            <li><strong>Release the device button:</strong>
              <ul style="margin-left:20px;margin-top:5px">
                <li>Click the "Connect" button below</li>
              </ul>
            </li>
            <li><strong>Wait until the bonding process completes</strong></li>
          </ol>
        </div>
      </div>

      <!-- Encrypted Mode Info -->
      <div id="connect-encrypted-info" style="display:none">
        <div class="alert success">
          <strong>⚡ Smart Connection</strong>
          <p style="margin-top:10px">
            With encrypted mode, the device will be:<br>
            ✓ Bonded (trusted connection)<br>
            ✓ Encrypted (secure with passkey)<br>
            ✓ Ready to use immediately<br>
            <br>
            <strong>Requires button press (10+ seconds)!</strong>
          </p>
        </div>
      </div>

      <div class="modal-buttons" style="margin-top:25px">
        <button class="modal-btn modal-btn-secondary" onclick="closeConnectModal()">Cancel</button>
        <button class="modal-btn modal-btn-primary" id="connect-confirm-btn" onclick="confirmSmartConnect()">
          Connect Device
        </button>
      </div>
    </div>
  </div>

  <!-- ============================================================================
       BLE Encrypted Pair Modal (Direct Pairing with Passkey)
       ============================================================================ -->
  
  <div id="ble-encrypted-pair-modal" class="modal hidden" role="dialog" aria-labelledby="enc-pair-modal-title" aria-modal="true">
    <div class="modal-box">
      <h3 id="enc-pair-modal-title">🔐 Pair Encrypted Device</h3>
      
      <div class="input-group">
        <label for="enc-pair-device-name">Device Name</label>
        <input type="text" id="enc-pair-device-name" readonly aria-readonly="true">
      </div>
      
      <div class="input-group">
        <label for="enc-pair-device-address">MAC Address</label>
        <input type="text" id="enc-pair-device-address" readonly aria-readonly="true">
      </div>
      
      <div class="alert warning">
        <strong>🔒 Encrypted Device</strong>
        <p style="margin-top:10px">
          This device is already encrypted and requires the 6-digit passkey
          that was set during initial pairing.
        </p>
      </div>
      
      <div class="input-group">
        <label for="enc-pair-passkey">Passkey (Required - 6 digits)</label>
        <input type="number" 
               id="enc-pair-passkey" 
               placeholder="Enter passkey" 
               min="0"
               max="999999"
               oninput="validateEncryptedPairForm()"
               aria-required="true"
               aria-describedby="enc-pair-help">
        <div id="enc-pair-help" style="font-size:0.85em;color:#888;margin-top:5px">
          Enter the 6-digit passkey (0-999999)
        </div>
      </div>
      
      <div class="modal-buttons">
        <button class="modal-btn modal-btn-secondary" onclick="closeEncryptedPairModal()" aria-label="Cancel pairing">Cancel</button>
        <button class="modal-btn modal-btn-primary" id="enc-pair-confirm-btn" 
                onclick="confirmEncryptedPair()" disabled aria-label="Confirm pairing">
          Pair Device
        </button>
      </div>
    </div>
  </div>

  <!-- ============================================================================
       Enable Encryption Modal (Phase 2)
       ============================================================================ -->
  
  <div id="enable-encryption-modal" class="modal hidden" role="dialog" aria-labelledby="enc-modal-title" aria-modal="true">
    <div class="modal-box">
      <h3 id="enc-modal-title">🔐 Enable Encryption</h3>
      
      <div class="alert warning">
        <strong>⚠️ Important</strong>
        <p style="margin-top:10px">
          This will permanently enable encryption on the device. 
          You will need to remember this passkey for all future connections!
        </p>
      </div>
      
      <div class="input-group">
        <label for="enc-device-name">Device</label>
        <input type="text" id="enc-device-name" readonly aria-readonly="true">
      </div>
      
      <div class="input-group">
        <label for="enc-device-address">MAC Address</label>
        <input type="text" id="enc-device-address" readonly aria-readonly="true">
      </div>
      
      <div class="input-group">
        <label for="enc-passkey">Choose Passkey (6 digits, 0-999999)</label>
        <input type="number" 
               id="enc-passkey" 
               placeholder="123456" 
               min="0" 
               max="999999"
               oninput="validateEnableEncryption()"
               aria-required="true"
               aria-describedby="enc-passkey-help">
        <div id="enc-passkey-help" style="font-size:0.85em;color:#888;margin-top:5px">
          Choose a 6-digit passkey to secure your device
        </div>
      </div>
      
      <div class="input-group">
        <label for="enc-passkey-confirm">Confirm Passkey</label>
        <input type="number" 
               id="enc-passkey-confirm" 
               placeholder="123456" 
               min="0" 
               max="999999"
               oninput="validateEnableEncryption()"
               aria-required="true"
               aria-describedby="enc-confirm-help">
        <div id="enc-confirm-help" style="font-size:0.85em;color:#888;margin-top:5px">
          Re-enter the same passkey to confirm
        </div>
      </div>
      
      <div class="alert">
        <strong>💾 Save this passkey!</strong>
        <p style="margin-top:8px">
          Write down your passkey in a safe place. You'll need it for future connections and after device resets.
        </p>
      </div>
      
      <div class="modal-buttons">
        <button class="modal-btn modal-btn-secondary" onclick="closeEnableEncryptionModal()" aria-label="Cancel encryption">Cancel</button>
        <button class="modal-btn modal-btn-primary" id="enc-confirm-btn" onclick="confirmEnableEncryption()" disabled aria-label="Enable encryption">
          Enable Encryption
        </button>
      </div>
    </div>
  </div>

  <!-- ============================================================================
       JavaScript
       ============================================================================ -->
  
  <script>
    'use strict';
    
    // ============================================================================
    // State Management
    // ============================================================================
    
    const AppState = {
      ws: null,
      reconnectInterval: null,
      statusInterval: null,
      matterCommissioned: false,
      discoveredDevices: [],
      currentBLEPairDevice: null,
      currentConnectDevice: null,
      currentEncryptedDevice: null,
      currentUnencryptedDevice: null,
      currentEncryptedKnownDevice: null,
      contactSensorMatterEnabled: false,
      contactSensorEndpointActive: false
    };

    // ============================================================================
    // Error Banner Functions
    // ============================================================================
    
    function showErrorBanner(title, message, type = 'error') {
      const banner = document.getElementById('error-banner');
      const titleEl = document.getElementById('error-banner-title');
      const messageEl = document.getElementById('error-banner-message');
      
      titleEl.textContent = title;
      messageEl.textContent = message;
      
      // Reset classes
      banner.className = 'error-banner';
      
      // Add type class
      if (type === 'success') {
        banner.classList.add('success');
      } else if (type === 'warning') {
        banner.classList.add('warning');
      }
      
      // Show banner
      setTimeout(() => {
        banner.classList.add('show');
      }, 100);
      
      // Auto-hide after 5 seconds
      setTimeout(() => {
        hideErrorBanner();
      }, 5000);
    }
    
    function hideErrorBanner() {
      const banner = document.getElementById('error-banner');
      banner.classList.remove('show');
    }
    
    // ============================================================================
    // Loading Overlay Functions
    // ============================================================================
    
    function showLoading(text = 'Loading...') {
      const overlay = document.getElementById('loading-overlay');
      const textEl = document.getElementById('loading-text');
      textEl.textContent = text;
      overlay.classList.add('show');
    }
    
    function hideLoading() {
      const overlay = document.getElementById('loading-overlay');
      overlay.classList.remove('show');
    }
    
    // ============================================================================
    // WebSocket Connection
    // ============================================================================
    
    const MAX_RECONNECT_DELAY = 30000;  // 30 Sekunden

    function connectWebSocket() {
        // ✅ Verhindere mehrfache gleichzeitige Connects
        if (AppState.ws && AppState.ws.readyState === WebSocket.CONNECTING) {
            console.log('⏳ WebSocket connection already in progress...');
            return;
        }
        
        // ✅ Schließe alte Connection falls noch offen
        if (AppState.ws && AppState.ws.readyState !== WebSocket.CLOSED) {
            console.log('🔌 Closing old WebSocket connection...');
            AppState.ws.close();
        }
        
        console.log('🔌 Connecting WebSocket (Attempt ' + (reconnectAttempts + 1) + ')...');
        
        try {
            AppState.ws = new WebSocket('ws://' + location.host + '/ws');
        } catch (error) {
            console.error('✗ Failed to create WebSocket:', error);
            scheduleReconnect();
            return;
        }

        AppState.ws.onopen = () => {
            console.log('✓ WebSocket connected');
            reconnectAttempts = 0;  // ✅ Reset counter
            clearInterval(AppState.reconnectInterval);
            hideErrorBanner();
            
            setTimeout(() => {
                if (AppState.ws.readyState === WebSocket.OPEN) {
                    AppState.ws.send('status');
                    AppState.ws.send('matter_status');
                }
            }, 100);
            
            AppState.statusInterval = setInterval(() => {
                if (AppState.ws.readyState === WebSocket.OPEN) {
                    AppState.ws.send('status');
                }
            }, 2000);
        };

        AppState.ws.onclose = (event) => {
            console.log('✗ WebSocket disconnected:', event.code, event.reason);
            clearInterval(AppState.statusInterval);
            
            showErrorBanner('Connection Lost', 'Attempting to reconnect...', 'warning');
            
            scheduleReconnect();
        };

        AppState.ws.onerror = (e) => {
            console.error('✗ WebSocket error:', e);
            // onerror wird immer von onclose gefolgt, daher hier nichts tun
        };

        AppState.ws.onmessage = handleWebSocketMessage;
      }

    // ✅ Exponential Backoff Reconnect
    function scheduleReconnect() {
        if (reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
            console.error('❌ Max reconnection attempts reached');
            showErrorBanner(
                'Connection Failed', 
                'Unable to connect. Please refresh the page.', 
                'error'
            );
      return;
      }
      // ✅ Exponential Backoff: 1s, 2s, 4s, 8s, 16s, 30s (max)
      const delay = Math.min(
          BASE_RECONNECT_DELAY * Math.pow(2, reconnectAttempts),
          MAX_RECONNECT_DELAY
      );

      reconnectAttempts++;

      console.log(`🔄 Reconnecting in ${delay}ms (attempt ${reconnectAttempts}/${MAX_RECONNECT_ATTEMPTS})`);

      clearTimeout(AppState.reconnectInterval);
      AppState.reconnectInterval = setTimeout(() => {
          connectWebSocket();
      }, delay);
    }
    
    // ============================================================================
    // WebSocket Message Handler
    // ============================================================================
    
    function handleWebSocketMessage(e) {
      console.log('📨 WebSocket message received:', e.data);
      
      let data;
      try {
        data = JSON.parse(e.data);
      } catch (err) {
        console.error('✗ Failed to parse JSON:', err);
        return;
      }

      if (!data || !data.type) {
        console.error('✗ Invalid message format - missing type:', data);
        return;
      }
      
      // Handle error messages
      if (data.type === 'error') {
        showErrorBanner('Error', data.message, 'error');
        hideLoading();
        return;
      }
      
      // Handle info messages
      if (data.type === 'info') {
        if (data.message && data.message.includes('<strong>')) {
          // HTML message - show as banner
          const tempDiv = document.createElement('div');
          tempDiv.innerHTML = data.message;
          const title = tempDiv.querySelector('strong')?.textContent || 'Info';
          const text = tempDiv.textContent.replace(title, '').trim();
          showErrorBanner(title, text, 'success');
        }
      }
      
      // Handle success messages
      if (data.type === 'success') {
        if (data.message) {
          const tempDiv = document.createElement('div');
          tempDiv.innerHTML = data.message;
          const title = tempDiv.querySelector('strong')?.textContent || 'Success';
          const text = tempDiv.textContent.replace(title, '').trim();
          showErrorBanner(title, text, 'success');
        }
        hideLoading();
      }
      
      // Route to specific handlers
      switch (data.type) {
        case 'status':
          handleStatusUpdate(data);
          break;
        case 'matter_status':
          handleMatterStatus(data);
          break;
        case 'info':
          handleSystemInfo(data);
          break;
        case 'ble_discovered':
          handleBLEDiscovered(data);
          break;
        case 'ble_status':
          handleBLEStatus(data);
          break;
        case 'ble_scan_complete':
          handleBLEScanComplete();
          break;
        case 'ble_state_changed':
          handleBLEStateChanged(data);
          break;
        case 'ble_sensor_update':
          handleBLESensorUpdate(data);
          break;
        case 'contact_sensor_status':
          handleContactSensorStatus(data);
          break;
        case 'modal_close':
            handleModalClose(data);
            break;
        case 'sensor_data_result':
            handleSensorDataResult(data);
            break;
        default:
          console.warn('⚠ Unknown message type:', data.type);
      }
    }
    
    // ============================================================================
    // Message Handlers
    // ============================================================================
    
    function handleStatusUpdate(data) {
      document.getElementById('pos').innerText = data.pos + '%';
      document.getElementById('calib').innerText = data.cal ? 'Yes' : 'No';
      document.getElementById('inv').innerText = data.inv ? 'Inverted' : 'Normal';
      updateDirectionButtons(data.inv);
    }
    
    function handleMatterStatus(data) {
      AppState.matterCommissioned = data.commissioned;
      
      const statusEl = document.getElementById('matter-status');
      if (data.commissioned) {
        statusEl.innerHTML = 'Paired <span class="badge commissioned">✓</span>';
        statusEl.className = 'status-value commissioned';
      } else {
        statusEl.innerHTML = 'Not Paired <span class="badge not-commissioned">!</span>';
        statusEl.className = 'status-value not-commissioned';
      }
      
      updateMatterInfo(data);
    }

    function handleModalClose(data) {
        console.log('📋 Modal close requested:', data.modal_id);
        
        const modal = document.getElementById(data.modal_id);
        if (modal) {
            modal.classList.add('hidden');
            modal.setAttribute('aria-hidden', 'true');
            console.log('✓ Modal closed:', data.modal_id);
        } else {
            console.warn('⚠ Modal not found:', data.modal_id);
        }
    }
    
    function handleSystemInfo(data) {
      if (data.chip) {
        renderSystemInfo(data);
      }
    }
    
    function handleBLEDiscovered(data) {
      AppState.discoveredDevices = data.devices || [];
      renderBLEDiscoveredDevices(data.devices || []);
    }
    
    function handleBLEStatus(data) {
      renderBLESensorStatus(data);
    }
    
    function handleBLEScanComplete() {
      const btn = document.getElementById('ble-scan-btn');
      btn.disabled = false;
      btn.innerHTML = '<span>🔍 Start Scan</span>';
      hideLoading();
    }
    
    function handleBLEStateChanged(data) {
      console.log('📡 BLE State Changed:', data.state);
      
      // Update UI based on state
      const statusDiv = document.querySelector('.state-indicator');
      if (statusDiv) {
        statusDiv.remove();
      }
      
      // Show appropriate message based on state
      if (data.state === 'connected_unencrypted') {
        showErrorBanner('Device Connected', 
          'Device is bonded but not encrypted. Enable encryption to secure the connection.', 
          'warning');
      } else if (data.state === 'connected_encrypted') {
        showErrorBanner('Encryption Enabled', 
          'Device is now securely encrypted!', 
          'success');
      }
    }

    // ════════════════════════════════════════════════════════════════════════
    // Handle GATT Sensor Data Result
    // ════════════════════════════════════════════════════════════════════════

    function handleSensorDataResult(data) {
      console.log('📊 GATT Sensor Data Result:', data);
      
      hideLoading();
      
      if (!data.success) {
        showErrorBanner('Read Failed', data.error || 'Could not read sensor data', 'error');
        return;
      }
      
      // Success - Update UI
      console.log('✓ Sensor data received:');
      console.log('  Packet ID:', data.packet_id);
      console.log('  Battery:', data.battery + '%');
      console.log('  Window:', data.window_open ? 'OPEN' : 'CLOSED');
      console.log('  Illuminance:', data.illuminance, 'lux');
      console.log('  Rotation:', data.rotation + '°');
      
      // Update Battery
      if (document.getElementById('ble-battery')) {
        document.getElementById('ble-battery').innerText = data.battery + '%';
      }
      
      // Update Contact State
      if (document.getElementById('ble-contact')) {
        const contactEl = document.getElementById('ble-contact');
        contactEl.innerText = data.window_open ? '🔓 OPEN' : '🔒 CLOSED';
        contactEl.className = 'status-value ' + (data.window_open ? 'not-commissioned' : 'commissioned');
      }
      
      // Update RSSI
      if (document.getElementById('ble-rssi')) {
        document.getElementById('ble-rssi').innerText = data.rssi + ' dBm';
      }
      
      // Update Illuminance
      if (document.getElementById('ble-lux')) {
        document.getElementById('ble-lux').innerText = data.illuminance + ' lux';
      }
      
      // Update Rotation
      if (document.getElementById('ble-rotation')) {
        document.getElementById('ble-rotation').innerText = data.rotation + '°';
      }
      
      // Update Packet ID
      if (document.getElementById('ble-packet-id')) {
        document.getElementById('ble-packet-id').innerText = data.packet_id;
      }
      
      // Update Last Update Time
      if (document.getElementById('ble-last-update')) {
        document.getElementById('ble-last-update').innerText = 'Just now';
      }
      
      // Show success message
      showErrorBanner(
        'Data Retrieved', 
        `Battery: ${data.battery}% | Window: ${data.window_open ? 'OPEN' : 'CLOSED'} | Light: ${data.illuminance} lux`, 
        'success'
      );
    }

    // ════════════════════════════════════════════════════════════════════════
    // Continuous Scan Manual Control
    // ════════════════════════════════════════════════════════════════════════

    function startContinuousScanManual() {
      if (!AppState.ws || AppState.ws.readyState !== WebSocket.OPEN) {
        showErrorBanner('Connection Error', 'WebSocket not connected', 'error');
        return;
      }
      
      console.log('📡 Starting Continuous Scan manually...');
      
      AppState.ws.send('ble_start_continuous_scan');
      
      showLoading('Starting Continuous Scan...');
      
      setTimeout(() => {
        hideLoading();
        updateContinuousScanUI(true);
      }, 2000);
    }

    function stopContinuousScanManual() {
      if (!AppState.ws || AppState.ws.readyState !== WebSocket.OPEN) {
        showErrorBanner('Connection Error', 'WebSocket not connected', 'error');
        return;
      }
      
      console.log('⏹️ Stopping Continuous Scan manually...');
      
      AppState.ws.send('ble_stop_scan');
      
      showLoading('Stopping Continuous Scan...');
      
      setTimeout(() => {
        hideLoading();
        updateContinuousScanUI(false);
      }, 2000);
    }

    function updateContinuousScanUI(isActive) {
      const statusEl = document.getElementById('continuous-scan-status');
      const startBtn = document.getElementById('start-continuous-scan-btn');
      const stopBtn = document.getElementById('stop-continuous-scan-btn');
      
      if (!statusEl || !startBtn || !stopBtn) {
        console.warn('⚠ Continuous Scan UI elements not found');
        return;
      }
      
      if (isActive) {
        statusEl.textContent = 'Active 🟢';
        statusEl.style.color = '#4CAF50';
        startBtn.style.display = 'none';
        stopBtn.style.display = 'block';
      } else {
        statusEl.textContent = 'Inactive 🔴';
        statusEl.style.color = '#f44336';
        startBtn.style.display = 'block';
        stopBtn.style.display = 'none';
      }
    }
    
    function handleBLESensorUpdate(data) {
      console.log('📊 BLE Sensor Update:', data);
      
      // Update all sensor values
      if (document.getElementById('ble-contact')) {
        document.getElementById('ble-contact').innerText = data.window_open ? '🔓 OPEN' : '🔒 CLOSED';
        document.getElementById('ble-contact').className = 'status-value ' + 
          (data.window_open ? 'not-commissioned' : 'commissioned');
      }
      
      if (document.getElementById('ble-battery')) {
        document.getElementById('ble-battery').innerText = data.battery + '%';
      }
      
      if (document.getElementById('ble-rssi')) {
        document.getElementById('ble-rssi').innerText = data.rssi + ' dBm';
      }
      
      if (document.getElementById('ble-lux')) {
        document.getElementById('ble-lux').innerText = data.illuminance + ' lux';
      }
      
      if (document.getElementById('ble-rotation')) {
        document.getElementById('ble-rotation').innerText = data.rotation + '°';
      }
      
      if (document.getElementById('ble-packet-id')) {
        document.getElementById('ble-packet-id').innerText = data.packet_id;
      }
      
      // Last update time
      if (document.getElementById('ble-last-update')) {
        const now = Date.now();
        const secondsAgo = Math.floor((now - data.last_update) / 1000);
        let timeStr;
        if (secondsAgo < 60) timeStr = secondsAgo + 's ago';
        else if (secondsAgo < 3600) timeStr = Math.floor(secondsAgo / 60) + 'm ago';
        else timeStr = Math.floor(secondsAgo / 3600) + 'h ago';
        
        document.getElementById('ble-last-update').innerText = timeStr;
      }
      
      // Button events
      if (data.has_button_event) {
        const eventMap = {
          1: '👆 Single Press',
          128: '⏸️ Hold',
          254: '⏸️ Hold'
        };
        
        const eventName = eventMap[data.button_event] || '❓ Unknown';
        document.getElementById('ble-button-event').innerText = eventName;
        document.getElementById('ble-button-container').style.display = 'block';
        
        // Highlight effect
        const btnContainer = document.getElementById('ble-button-container');
        btnContainer.style.background = 'rgba(33, 150, 243, 0.2)';
        btnContainer.style.borderColor = '#2196F3';
        
        setTimeout(() => {
          btnContainer.style.background = '';
          btnContainer.style.borderColor = '';
        }, 3000);
      }
    }
    
    function handleContactSensorStatus(data) {
      AppState.contactSensorMatterEnabled = data.enabled;
      AppState.contactSensorEndpointActive = data.active;
      updateContactSensorToggle(data.enabled, data.active);
    }
    
    // ============================================================================
    // Tab Navigation
    // ============================================================================
    
    function show(id) {
      // Hide all tabs
      document.querySelectorAll('#overview, #matter, #ble, #system, #settings').forEach(e => {
        e.classList.add('hidden');
        e.setAttribute('aria-hidden', 'true');
      });
      
      // Show selected tab
      const selectedTab = document.getElementById(id);
      selectedTab.classList.remove('hidden');
      selectedTab.setAttribute('aria-hidden', 'false');
      
      // Update nav buttons
      document.querySelectorAll('.nav button').forEach(b => {
        b.classList.remove('active');
        b.setAttribute('aria-selected', 'false');
      });
      
      const navBtn = document.getElementById('nav-' + id);
      navBtn.classList.add('active');
      navBtn.setAttribute('aria-selected', 'true');
      
      // Load tab-specific data
      if (AppState.ws && AppState.ws.readyState === WebSocket.OPEN) {
        switch(id) {
          case 'system':
            AppState.ws.send('info');
            break;
          case 'matter':
            AppState.ws.send('matter_status');
            break;
          case 'ble':
            AppState.ws.send('ble_status');
            AppState.ws.send('contact_sensor_status');
            break;
        }
      }
    }
    
    // ============================================================================
    // Keyboard Navigation for Direction Buttons
    // ============================================================================
    
    document.addEventListener('DOMContentLoaded', () => {
      // Direction buttons keyboard support
      document.querySelectorAll('.direction-btn').forEach(btn => {
        btn.addEventListener('keydown', (e) => {
          if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            btn.click();
          }
        });
      });
    });
    
    // ============================================================================
    // Shutter Control Functions
    // ============================================================================
    
    function send(cmd) {
      if (AppState.ws && AppState.ws.readyState === WebSocket.OPEN) {
        AppState.ws.send(cmd);
        
        // Visual feedback
        if (cmd === 'calibrate') {
          showLoading('Starting calibration...');
        }
      } else {
        showErrorBanner('Connection Error', 'WebSocket not connected', 'error');
      }
    }
    
    function setDirection(inverted) {
      if (AppState.ws && AppState.ws.readyState === WebSocket.OPEN) {
        AppState.ws.send(inverted ? 'invert_on' : 'invert_off');
        updateDirectionButtons(inverted);
      }
    }
    
    function updateDirectionButtons(inverted) {
      const normalBtn = document.getElementById('dir-normal');
      const invertedBtn = document.getElementById('dir-inverted');
      
      if (inverted) {
        normalBtn.classList.remove('active');
        normalBtn.setAttribute('aria-pressed', 'false');
        invertedBtn.classList.add('active');
        invertedBtn.setAttribute('aria-pressed', 'true');
      } else {
        invertedBtn.classList.remove('active');
        invertedBtn.setAttribute('aria-pressed', 'false');
        normalBtn.classList.add('active');
        normalBtn.setAttribute('aria-pressed', 'true');
      }
    }
    
    // ============================================================================
    // Factory Reset
    // ============================================================================
    
    function showConfirm() {
      document.getElementById('confirm-reset').classList.remove('hidden');
      document.getElementById('confirm-reset').setAttribute('aria-hidden', 'false');
      // Focus first button for accessibility
      setTimeout(() => {
        document.querySelector('#confirm-reset .modal-btn-secondary').focus();
      }, 100);
    }
    
    function hideConfirm() {
      document.getElementById('confirm-reset').classList.add('hidden');
      document.getElementById('confirm-reset').setAttribute('aria-hidden', 'true');
    }
    
    function confirmReset() {
      if (AppState.ws && AppState.ws.readyState === WebSocket.OPEN) {
        showLoading('Resetting device...');
        AppState.ws.send('reset');
        hideConfirm();
        
        // Device will restart, show message
        setTimeout(() => {
          hideLoading();
          showErrorBanner('Device Reset', 'Device is restarting. Please wait...', 'warning');
        }, 2000);
      }
    }
    
    // ============================================================================
    // Matter Functions
    // ============================================================================
    
    function showMatterQR(qrUrl, qrImageUrl, pairingCode) {
      document.getElementById('matter-qr-img').src = qrImageUrl;
      document.getElementById('matter-pairing-code').innerText = pairingCode;
      
      const linkEl = document.getElementById('qr-web-link');
      if (linkEl && qrUrl) {
        linkEl.href = qrUrl;
        linkEl.style.display = 'inline-block';
      } else if (linkEl) {
        linkEl.style.display = 'none';
      }
      
      document.getElementById('matter-qr-modal').classList.remove('hidden');
      document.getElementById('matter-qr-modal').setAttribute('aria-hidden', 'false');
    }
    
    function closeMatterQR() {
      document.getElementById('matter-qr-modal').classList.add('hidden');
      document.getElementById('matter-qr-modal').setAttribute('aria-hidden', 'true');
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
                        (data.qr_url || '').replace(/'/g, "\\'") + '\',\'' + 
                        data.qr_image.replace(/'/g, "\\'") + '\',\'' + 
                        (data.pairing_code || '').replace(/'/g, "\\'") + 
                        '\')" style="width:100%" aria-label="Show Matter pairing QR code"><span>📱 Show QR Code & Pairing Info</span></button>';
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
    
    // ============================================================================
    // System Info
    // ============================================================================
    
    function renderSystemInfo(data) {
      let html = '';
      html += '<tr><td>Chip ID:</td><td>' + (data.chip || 'N/A') + '</td></tr>';
      html += '<tr><td>Uptime:</td><td>' + (data.uptime || 0) + 's</td></tr>';
      html += '<tr><td>Free Heap:</td><td>' + ((data.heap || 0) / 1024).toFixed(1) + ' KB</td></tr>';
      html += '<tr><td>Min Free Heap:</td><td>' + ((data.minheap || 0) / 1024).toFixed(1) + ' KB</td></tr>';
      html += '<tr><td>Flash Size:</td><td>' + ((data.flash || 0) / 1024 / 1024).toFixed(1) + ' MB</td></tr>';
      html += '<tr><td>Firmware Version:</td><td>' + (data.ver || 'N/A') + '</td></tr>';
      html += '<tr><td>Reset Reason:</td><td>' + (data.reset || 'Unknown') + '</td></tr>';
      document.getElementById('sysinfo').innerHTML = html;
    }
    
    // ============================================================================
    // BLE Scan Functions
    // ============================================================================
    
    function startBLEScan() {
      if (AppState.ws && AppState.ws.readyState === WebSocket.OPEN) {
        AppState.ws.send('ble_scan');
        
        const btn = document.getElementById('ble-scan-btn');
        btn.disabled = true;
        btn.innerHTML = '<span class="spinner"></span> <span>Scanning...</span>';
        
        showLoading('Scanning for BLE devices...');
        
        // Safety timeout
        setTimeout(() => {
          btn.disabled = false;
          btn.innerHTML = '<span>🔍 Start Scan</span>';
          hideLoading();
        }, 15000);
      }
    }
    
    function renderBLEDiscoveredDevices(devices) {
      let html = '';
      
      if (!devices || devices.length === 0) {
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
          
          const devJson = JSON.stringify(dev).replace(/"/g, '&quot;');
          
          html += `<li class="device-item">
            <div class="device-info">
              <div class="device-name">${escapeHtml(dev.name)}`;
          
          // ✅ Encryption Badge
          if (dev.encrypted) {
            html += `<span class="badge" style="background:rgba(76,175,80,0.2);color:#4CAF50;border:1px solid rgba(76,175,80,0.3)">🔒 Encrypted</span>`;
          } else {
            html += `<span class="badge" style="background:rgba(255,152,0,0.2);color:#FF9800;border:1px solid rgba(255,152,0,0.3)">🔓 Unencrypted</span>`;
          }
          
          html += `</div>
              <div class="device-details">
                MAC: ${escapeHtml(dev.address)} | 
                <span class="${signalClass}">${dev.rssi} dBm</span>
              </div>
            </div>
            <div class="device-actions">`;
          
          // ✅ 3 VERSCHIEDENE BUTTONS je nach Status
          
          if (dev.encrypted) {
            // ──────────────────────────────────────────────────────────────────
            // ENCRYPTED DEVICE → Button für "Pair with Credentials"
            // ──────────────────────────────────────────────────────────────────
            html += `<button class="btn primary" onclick='openEncryptedKnownModal(${devJson})' 
                      style="padding:10px 20px" 
                      aria-label="Pair encrypted device ${escapeHtml(dev.name)}">
                      <span>🔐 Pair (Have Keys)</span>
                    </button>`;
          } else {
            // ──────────────────────────────────────────────────────────────────
            // UNENCRYPTED DEVICE → Button für Smart Connect
            // ──────────────────────────────────────────────────────────────────
            html += `<button class="btn secondary" onclick='openConnectModal(${devJson})' 
                      style="padding:10px 20px" 
                      aria-label="Connect to device ${escapeHtml(dev.name)}">
                      <span>🔗 Connect</span>
                    </button>`;
          }
          
          html += `</div>
          </li>`;
        });
      }
      
      document.getElementById('ble-discovered-devices').innerHTML = html;
    }

    
    // ============================================================================
    // Smart Connect Functions
    // ============================================================================

    let connectMode = 'unencrypted';  // Default mode

    function selectConnectMode(mode) {
      connectMode = mode;
      
      const unencBtn = document.getElementById('connect-mode-unencrypted');
      const encBtn = document.getElementById('connect-mode-encrypted');
      const passkeySection = document.getElementById('connect-passkey-section');
      const buttonInstructions = document.getElementById('connect-button-instructions');
      const encryptedInfo = document.getElementById('connect-encrypted-info');
      
      if (mode === 'unencrypted') {
        // Unencrypted Mode UI
        unencBtn.classList.add('active');
        unencBtn.setAttribute('aria-pressed', 'true');
        encBtn.classList.remove('active');
        encBtn.setAttribute('aria-pressed', 'false');
        
        passkeySection.style.display = 'none';
        buttonInstructions.style.display = 'block';
        encryptedInfo.style.display = 'none';
        
        // Reset passkey inputs
        document.getElementById('connect-passkey').value = '';
        document.getElementById('connect-passkey-confirm').value = '';
        
        // Enable button (no passkey validation needed)
        document.getElementById('connect-confirm-btn').disabled = false;
        
      } else {
        // Encrypted Mode UI
        encBtn.classList.add('active');
        encBtn.setAttribute('aria-pressed', 'true');
        unencBtn.classList.remove('active');
        unencBtn.setAttribute('aria-pressed', 'false');
        
        passkeySection.style.display = 'block';
        buttonInstructions.style.display = 'block';  // Noch immer Button-Press nötig!
        encryptedInfo.style.display = 'block';
        
        // Disable button until passkey is valid
        document.getElementById('connect-confirm-btn').disabled = true;
        
        // Focus first passkey input
        setTimeout(() => {
          document.getElementById('connect-passkey').focus();
        }, 100);
      }
    }

    function validateConnectPasskey() {
      const passkey = document.getElementById('connect-passkey').value;
      const confirm = document.getElementById('connect-passkey-confirm').value;
      const btn = document.getElementById('connect-confirm-btn');
      
      // Enable button only if both fields are 6 digits and match
      const isValid = (passkey.length === 6 && confirm.length === 6 && passkey === confirm);
      btn.disabled = !isValid;
      
      // Visual feedback
      const confirmInput = document.getElementById('connect-passkey-confirm');
      if (confirm.length === 6) {
        if (passkey === confirm) {
          confirmInput.style.borderColor = 'var(--success-color)';
        } else {
          confirmInput.style.borderColor = 'var(--error-color)';
        }
      } else {
        confirmInput.style.borderColor = '';
      }
    }

    function openConnectModal(deviceJson) {
      const device = typeof deviceJson === 'string' ? JSON.parse(deviceJson) : deviceJson;
      AppState.currentConnectDevice = device;
      
      document.getElementById('connect-device-name').value = device.name || 'Unknown';
      document.getElementById('connect-device-address').value = device.address || 'Unknown';
      
      // Reset to unencrypted mode
      selectConnectMode('unencrypted');
      
      document.getElementById('ble-connect-modal').classList.remove('hidden');
      document.getElementById('ble-connect-modal').setAttribute('aria-hidden', 'false');
    }

    function closeConnectModal() {
      document.getElementById('ble-connect-modal').classList.add('hidden');
      document.getElementById('ble-connect-modal').setAttribute('aria-hidden', 'true');
      AppState.currentConnectDevice = null;
      
      // Reset form
      connectMode = 'unencrypted';
      document.getElementById('connect-passkey').value = '';
      document.getElementById('connect-passkey-confirm').value = '';
      
      const btn = document.getElementById('connect-confirm-btn');
      btn.disabled = false;
      btn.innerHTML = 'Connect Device';
    }

    function confirmSmartConnect() {
      if (!AppState.currentConnectDevice) return;
      
      if (!AppState.ws || AppState.ws.readyState !== WebSocket.OPEN) {
        showErrorBanner('Connection Error', 'WebSocket not connected', 'error');
        return;
      }
      
      const address = AppState.currentConnectDevice.address;
      let passkey = 0;
      
      // ========================================================================
      // Passkey Validation (nur bei Encrypted Mode)
      // ========================================================================
      
      if (connectMode === 'encrypted') {
        const passkeyInput = document.getElementById('connect-passkey').value;
        const confirmInput = document.getElementById('connect-passkey-confirm').value;
        
        if (passkeyInput.length !== 6 || passkeyInput !== confirmInput) {
          showErrorBanner('Invalid Passkey', 'Passkeys must be 6 digits and match!', 'error');
          return;
        }
        
        passkey = parseInt(passkeyInput);
      }
      
      console.log('🔗 Smart Connect:', connectMode);
      console.log('  Address:', address);
      console.log('  Passkey:', passkey > 0 ? passkey : 'NONE');
      
      // ========================================================================
      // Send Command to ESP32
      // ========================================================================
      
      AppState.ws.send(JSON.stringify({
        cmd: 'ble_smart_connect',
        address: address,
        passkey: passkey
      }));
      
      // ========================================================================
      // UI Feedback
      // ========================================================================
      
      const btn = document.getElementById('connect-confirm-btn');
      btn.disabled = true;
      
      if (connectMode === 'unencrypted') {
        btn.innerHTML = '<span class="spinner"></span> <span>Bonding...</span>';
        showLoading('Bonding device (unencrypted)...');
      } else {
        btn.innerHTML = '<span class="spinner"></span> <span>Bonding + Encrypting...</span>';
        showLoading('Bonding + Enabling encryption...');
      }
      
      // Auto-close modal after timeout
      setTimeout(() => {
        closeConnectModal();
        hideLoading();
      }, 30000);
    }

    // ============================================================================
    // BLE Connect Modal (Phase 1: Bonding)
    // ============================================================================
    
    function openConnectModal(deviceJson) {
      const device = typeof deviceJson === 'string' ? JSON.parse(deviceJson) : deviceJson;
      AppState.currentConnectDevice = device;
      
      document.getElementById('connect-device-name').value = device.name || 'Unknown';
      document.getElementById('connect-device-address').value = device.address || 'Unknown';
      
      document.getElementById('ble-connect-modal').classList.remove('hidden');
      document.getElementById('ble-connect-modal').setAttribute('aria-hidden', 'false');
    }
    
    function closeConnectModal() {
      document.getElementById('ble-connect-modal').classList.add('hidden');
      document.getElementById('ble-connect-modal').setAttribute('aria-hidden', 'true');
      AppState.currentConnectDevice = null;
      
      // Reset button state
      const btn = document.getElementById('connect-confirm-btn');
      btn.disabled = false;
      btn.innerHTML = 'Connect Device';
    }
    
    function confirmConnect() {
      if (!AppState.currentConnectDevice) return;
      
      if (!AppState.ws || AppState.ws.readyState !== WebSocket.OPEN) {
        showErrorBanner('Connection Error', 'WebSocket not connected', 'error');
        return;
      }
      
      console.log('🔗 Connecting to device:', AppState.currentConnectDevice.address);
      
      AppState.ws.send(JSON.stringify({
        cmd: 'ble_connect',
        address: AppState.currentConnectDevice.address
      }));
      
      // Button state
      const btn = document.getElementById('connect-confirm-btn');
      btn.disabled = true;
      btn.innerHTML = '<span class="spinner"></span> <span>Connecting...</span>';
      
      showLoading('Bonding with device...');
      
      // Auto-close after timeout
      setTimeout(() => {
        closeConnectModal();
        hideLoading();
      }, 20000);
    }

    // ============================================================================
    // Already-Encrypted Device Pairing Functions
    // ============================================================================

    function openEncryptedKnownModal(deviceJson) {
      const device = typeof deviceJson === 'string' ? JSON.parse(deviceJson) : deviceJson;
      AppState.currentEncryptedKnownDevice = device;
      
      document.getElementById('enc-known-device-name').value = device.name || 'Unknown';
      document.getElementById('enc-known-device-address').value = device.address || 'Unknown';
      document.getElementById('enc-known-passkey').value = '';
      document.getElementById('enc-known-bindkey').value = '';
      
      document.getElementById('ble-encrypted-known-modal').classList.remove('hidden');
      document.getElementById('ble-encrypted-known-modal').setAttribute('aria-hidden', 'false');
      
      // Focus passkey input
      setTimeout(() => {
        document.getElementById('enc-known-passkey').focus();
      }, 100);
    }

    function closeEncryptedKnownModal() {
      document.getElementById('ble-encrypted-known-modal').classList.add('hidden');
      document.getElementById('ble-encrypted-known-modal').setAttribute('aria-hidden', 'true');
      AppState.currentEncryptedKnownDevice = null;
      
      // Reset form
      document.getElementById('enc-known-passkey').value = '';
      document.getElementById('enc-known-bindkey').value = '';
      const btn = document.getElementById('enc-known-confirm-btn');
      btn.disabled = true;
      btn.innerHTML = 'Pair Device';
    }

    function validateEncryptedKnownForm() {
      const passkey = document.getElementById('enc-known-passkey').value.trim();
      const bindkey = document.getElementById('enc-known-bindkey').value.trim().toLowerCase();
      const btn = document.getElementById('enc-known-confirm-btn');
      
      // Validate passkey (6 digits)
      const passkeyValid = (passkey.length === 6 && !isNaN(passkey));
      
      // Validate bindkey (32 hex characters)
      const bindkeyValid = (bindkey.length === 32 && /^[0-9a-f]{32}$/.test(bindkey));
      
      // Visual feedback for bindkey
      const bindkeyInput = document.getElementById('enc-known-bindkey');
      if (bindkey.length > 0) {
        if (bindkeyValid) {
          bindkeyInput.style.borderColor = 'var(--success-color)';
        } else {
          bindkeyInput.style.borderColor = 'var(--error-color)';
        }
      } else {
        bindkeyInput.style.borderColor = '';
      }
      
      // Enable button only if both valid
      btn.disabled = !(passkeyValid && bindkeyValid);
    }

    function confirmEncryptedKnownPair() {
      if (!AppState.currentEncryptedKnownDevice) return;
      
      if (!AppState.ws || AppState.ws.readyState !== WebSocket.OPEN) {
        showErrorBanner('Connection Error', 'WebSocket not connected', 'error');
        return;
      }
      
      const passkeyInput = document.getElementById('enc-known-passkey').value.trim();
      const bindkeyInput = document.getElementById('enc-known-bindkey').value.trim().toLowerCase();
      
      // Validate again
      if (passkeyInput.length !== 6 || bindkeyInput.length !== 32 || !/^[0-9a-f]{32}$/.test(bindkeyInput)) {
        showErrorBanner('Invalid Input', 'Please check passkey and bindkey format', 'error');
        return;
      }
      
      const passkey = parseInt(passkeyInput);
      
      console.log('🔐 Pairing already-encrypted device');
      console.log('  Address:', AppState.currentEncryptedKnownDevice.address);
      console.log('  Passkey:', passkey);
      console.log('  Bindkey:', bindkeyInput);
      
      // Send command
      AppState.ws.send(JSON.stringify({
        cmd: 'ble_pair_encrypted_known',
        address: AppState.currentEncryptedKnownDevice.address,
        passkey: passkey,
        bindkey: bindkeyInput
      }));
      
      // Button state
      const btn = document.getElementById('enc-known-confirm-btn');
      btn.disabled = true;
      btn.innerHTML = '<span class="spinner"></span> <span>Pairing...</span>';
      
      showLoading('Pairing with encrypted device...');
      
      // Auto-close after timeout
      setTimeout(() => {
        closeEncryptedKnownModal();
        hideLoading();
      }, 30000);
    }
    
    // ============================================================================
    // BLE Encrypted Pair Modal (Direct Pairing)
    // ============================================================================
    
    function openEncryptedPairModal(deviceJson) {
      const device = typeof deviceJson === 'string' ? JSON.parse(deviceJson) : deviceJson;
      AppState.currentEncryptedDevice = device;
      
      document.getElementById('enc-pair-device-name').value = device.name || 'Unknown';
      document.getElementById('enc-pair-device-address').value = device.address || 'Unknown';
      document.getElementById('enc-pair-passkey').value = '';
      
      document.getElementById('ble-encrypted-pair-modal').classList.remove('hidden');
      document.getElementById('ble-encrypted-pair-modal').setAttribute('aria-hidden', 'false');
      
      // Focus passkey input
      setTimeout(() => {
        document.getElementById('enc-pair-passkey').focus();
      }, 100);
    }
    
    function closeEncryptedPairModal() {
      document.getElementById('ble-encrypted-pair-modal').classList.add('hidden');
      document.getElementById('ble-encrypted-pair-modal').setAttribute('aria-hidden', 'true');
      AppState.currentEncryptedDevice = null;
      
      // Reset form
      document.getElementById('enc-pair-passkey').value = '';
      const btn = document.getElementById('enc-pair-confirm-btn');
      btn.disabled = true;
      btn.innerHTML = 'Pair Device';
    }
    
    function validateEncryptedPairForm() {
      const passkey = document.getElementById('enc-pair-passkey').value.trim();
      const btn = document.getElementById('enc-pair-confirm-btn');
      
      btn.disabled = (passkey.length !== 6);
    }
    
    function confirmEncryptedPair() {
      if (!AppState.currentEncryptedDevice) return;
      
      if (!AppState.ws || AppState.ws.readyState !== WebSocket.OPEN) {
        showErrorBanner('Connection Error', 'WebSocket not connected', 'error');
        return;
      }
      
      const passkeyInput = document.getElementById('enc-pair-passkey');
      const passkey = passkeyInput.value.trim();
      
      if (passkey.length !== 6) {
        showErrorBanner('Invalid Passkey', 'Passkey must be exactly 6 digits', 'error');
        return;
      }
      
      console.log('🔐 Pairing encrypted device with passkey');
      
      AppState.ws.send(JSON.stringify({
        cmd: 'ble_pair_encrypted',
        address: AppState.currentEncryptedDevice.address,
        passkey: parseInt(passkey)
      }));
      
      // Button state
      const btn = document.getElementById('enc-pair-confirm-btn');
      btn.disabled = true;
      btn.innerHTML = '<span class="spinner"></span> <span>Pairing...</span>';
      
      showLoading('Pairing with encrypted device...');
      
      // Auto-close after timeout
      setTimeout(() => {
        closeEncryptedPairModal();
        hideLoading();
      }, 30000);
    }
    
    // ============================================================================
    // Enable Encryption Modal (Phase 2)
    // ============================================================================
    
    function showEnableEncryptionModal() {
      // Find unencrypted device from discovered list
      const unencrypted = AppState.discoveredDevices.find(d => !d.encrypted);
      if (!unencrypted) {
        showErrorBanner('No Device', 'No unencrypted device found', 'error');
        return;
      }
      
      AppState.currentUnencryptedDevice = unencrypted;
      
      document.getElementById('enc-device-name').value = unencrypted.name;
      document.getElementById('enc-device-address').value = unencrypted.address;
      document.getElementById('enc-passkey').value = '';
      document.getElementById('enc-passkey-confirm').value = '';
      
      document.getElementById('enable-encryption-modal').classList.remove('hidden');
      document.getElementById('enable-encryption-modal').setAttribute('aria-hidden', 'false');
      
      // Focus first input
      setTimeout(() => {
        document.getElementById('enc-passkey').focus();
      }, 100);
    }
    
    function closeEnableEncryptionModal() {
      document.getElementById('enable-encryption-modal').classList.add('hidden');
      document.getElementById('enable-encryption-modal').setAttribute('aria-hidden', 'true');
      AppState.currentUnencryptedDevice = null;
      
      // Reset form
      document.getElementById('enc-passkey').value = '';
      document.getElementById('enc-passkey-confirm').value = '';
      const btn = document.getElementById('enc-confirm-btn');
      btn.disabled = true;
      btn.innerHTML = 'Enable Encryption';
    }
    
    function validateEnableEncryption() {
      const passkey = document.getElementById('enc-passkey').value;
      const confirm = document.getElementById('enc-passkey-confirm').value;
      const btn = document.getElementById('enc-confirm-btn');
      
      // Enable button only if:
      // 1. Both fields have exactly 6 digits
      // 2. Both values match
      const isValid = (passkey.length === 6 && confirm.length === 6 && passkey === confirm);
      btn.disabled = !isValid;
      
      // Visual feedback for matching
      const confirmInput = document.getElementById('enc-passkey-confirm');
      if (confirm.length === 6) {
        if (passkey === confirm) {
          confirmInput.style.borderColor = 'var(--success-color)';
        } else {
          confirmInput.style.borderColor = 'var(--error-color)';
        }
      } else {
        confirmInput.style.borderColor = '';
      }
    }
    
    function confirmEnableEncryption() {
      if (!AppState.currentUnencryptedDevice) return;
      
      const passkey = document.getElementById('enc-passkey').value;
      const confirm = document.getElementById('enc-passkey-confirm').value;
      
      if (passkey.length !== 6 || passkey !== confirm) {
        showErrorBanner('Invalid Input', 'Passkeys must be 6 digits and match!', 'error');
        return;
      }
      
      if (!AppState.ws || AppState.ws.readyState !== WebSocket.OPEN) {
        showErrorBanner('Connection Error', 'WebSocket not connected', 'error');
        return;
      }
      
      console.log('🔐 Enabling encryption with passkey...');
      
      AppState.ws.send(JSON.stringify({
        cmd: 'ble_enable_encryption',
        address: AppState.currentUnencryptedDevice.address,
        passkey: parseInt(passkey)
      }));
      
      closeEnableEncryptionModal();
      showLoading('Enabling encryption...');
      
      // Refresh after 3 seconds
      setTimeout(() => {
        if (AppState.ws && AppState.ws.readyState === WebSocket.OPEN) {
          AppState.ws.send('ble_status');
        }
        hideLoading();
      }, 3000);
    }
    
    // ============================================================================
    // BLE Sensor Status Rendering
    // ============================================================================
    
    function renderBLESensorStatus(data) {
      if (data.paired) {
        document.getElementById('ble-sensor-status').classList.remove('hidden');
        document.getElementById('ble-scan-interface').classList.add('hidden');
        document.getElementById('ble-matter-toggle').classList.remove('hidden');
        
        document.getElementById('ble-device-name').innerText = data.name || 'Unknown';
        document.getElementById('ble-device-address').innerText = data.address || 'Unknown';
        
        // Security Info anzeigen
        const securityBlock = document.getElementById('ble-security-info');
        if (securityBlock) {
            securityBlock.style.display = 'block';
            
            // Passkey
            const passkeyEl = document.getElementById('ble-passkey');
            if (data.passkey && data.passkey !== 'Not set') {
                passkeyEl.innerText = data.passkey;
                passkeyEl.style.color = '#4CAF50';
            } else {
                passkeyEl.innerText = 'Not set';
                passkeyEl.style.color = '#888';
            }
            
            // Encryption Status
            const encStatusEl = document.getElementById('ble-encryption-status');
            if (data.state === 'connected_encrypted') {
                encStatusEl.innerHTML = '🔒 Enabled';
                encStatusEl.style.color = '#4CAF50';
            } else if (data.state === 'connected_unencrypted') {
                encStatusEl.innerHTML = '🔓 Disabled';
                encStatusEl.style.color = '#FF9800';
            } else {
                encStatusEl.innerHTML = 'Unknown';
                encStatusEl.style.color = '#888';
            }
            
            // Bindkey
            const bindkeyEl = document.getElementById('ble-bindkey');
            if (data.bindkey && data.bindkey.length === 32) {
                bindkeyEl.innerText = data.bindkey;
                bindkeyEl.style.color = '#4CAF50';
            } else {
                bindkeyEl.innerText = 'Not available (device not encrypted)';
                bindkeyEl.style.color = '#888';
            }
        }
        
        // State indicator
        let stateIndicator = '';
        
        if (data.state === 'connected_unencrypted') {
          stateIndicator = '<div class="alert warning state-indicator" style="margin-bottom:15px">' +
                          '<strong>🔓 Connected (Unencrypted)</strong>' +
                          '<p style="margin-top:8px">This device is connected but not encrypted. ' +
                          'Click "Enable Encryption" below to secure the connection.</p>' +
                          '<button class="btn primary" onclick="showEnableEncryptionModal()" ' +
                          'style="margin-top:12px;width:100%" aria-label="Enable encryption on connected device">' +
                          '<span>🔐 Enable Encryption</span>' +
                          '</button>' +
                          '</div>';
        } else if (data.state === 'connected_encrypted') {
          stateIndicator = '<div class="alert success state-indicator" style="margin-bottom:15px">' +
                          '<strong>✓ Connected & Encrypted</strong>' +
                          '<p style="margin-top:8px">This device is securely connected with encryption enabled.</p>' +
                          '</div>';
        }
        
        // Remove old state indicator
        const statusContainer = document.getElementById('ble-sensor-status');
        const existingIndicator = statusContainer.querySelector('.state-indicator');
        if (existingIndicator) {
          existingIndicator.remove();
        }
        
        // Insert new state indicator at the beginning
        if (stateIndicator) {
          const h3 = statusContainer.querySelector('h3');
          if (h3) {
            h3.insertAdjacentHTML('afterend', stateIndicator);
          }
        }
        
        const contactEl = document.getElementById('ble-contact');
        
        // Check if we have valid sensor data
        if (data.sensor_data && data.sensor_data.valid) {
          // Contact State
          contactEl.innerText = data.sensor_data.window_open ? '🔓 OPEN' : '🔒 CLOSED';
          contactEl.className = 'status-value ' + (data.sensor_data.window_open ? 'not-commissioned' : 'commissioned');
          
          // Battery
          document.getElementById('ble-battery').innerText = data.sensor_data.battery + '%';
          
          // RSSI
          document.getElementById('ble-rssi').innerText = data.sensor_data.rssi + ' dBm';
          
          // Illuminance
          document.getElementById('ble-lux').innerText = data.sensor_data.illuminance + ' lux';
          
          // Rotation
          document.getElementById('ble-rotation').innerText = data.sensor_data.rotation + '°';
          
          // Last Update
          const now = Date.now();
          const secondsAgo = Math.floor((now - data.sensor_data.last_update) / 1000);
          let timeStr;
          if (secondsAgo < 60) timeStr = secondsAgo + 's ago';
          else if (secondsAgo < 3600) timeStr = Math.floor(secondsAgo / 60) + 'm ago';
          else timeStr = Math.floor(secondsAgo / 3600) + 'h ago';
          
          document.getElementById('ble-last-update').innerText = timeStr;

          // Update Continuous Scan Control UI
          const scanControlDiv = document.getElementById('ble-continuous-scan-control');
          if (scanControlDiv) {
            scanControlDiv.style.display = 'block';
              
            // Status vom Backend (falls verfügbar)
            const isScanning = data.continuous_scan_active || false;
            updateContinuousScanUI(isScanning);
          }
          
          // Packet ID
          if (typeof data.sensor_data.packet_id !== 'undefined') {
            document.getElementById('ble-packet-id').innerText = data.sensor_data.packet_id;
          } else {
            document.getElementById('ble-packet-id').innerText = '--';
          }
          
          // Button Events
          if (data.sensor_data.has_button_event) {
            const eventMap = {
              0: { name: 'None', emoji: '' },
              1: { name: 'Single Press', emoji: '👆' },
              128: { name: 'Hold', emoji: '⏸️' },
              254: { name: 'Hold', emoji: '⏸️' }
            };
            
            const eventInfo = eventMap[data.sensor_data.button_event] || { name: 'Unknown', emoji: '❓' };
            
            document.getElementById('ble-button-event').innerText = eventInfo.emoji + ' ' + eventInfo.name;
            document.getElementById('ble-button-container').style.display = 'block';
            
            // Highlight effect
            const btnContainer = document.getElementById('ble-button-container');
            btnContainer.style.background = 'rgba(33, 150, 243, 0.2)';
            btnContainer.style.borderColor = '#2196F3';
            
            setTimeout(() => {
              btnContainer.style.background = '';
              btnContainer.style.borderColor = '';
            }, 3000);
            
          } else {
            document.getElementById('ble-button-container').style.display = 'none';
          }
          
        } else {
          // No valid sensor data yet
          contactEl.innerText = 'No Data';
          contactEl.className = 'status-value';
          document.getElementById('ble-battery').innerText = '--%';
          document.getElementById('ble-rssi').innerText = '-- dBm';
          document.getElementById('ble-lux').innerText = '-- lux';
          document.getElementById('ble-rotation').innerText = '--°';
          document.getElementById('ble-last-update').innerText = 'Waiting...';
          document.getElementById('ble-packet-id').innerText = '--';
          document.getElementById('ble-button-container').style.display = 'none';
        }
      } else {
        // Not paired
        document.getElementById('ble-sensor-status').classList.add('hidden');
        document.getElementById('ble-scan-interface').classList.remove('hidden');
        document.getElementById('ble-matter-toggle').classList.add('hidden');
        const securityBlock = document.getElementById('ble-security-info');
        if (securityBlock) {
            securityBlock.style.display = 'none';
        }
        const scanControlDiv = document.getElementById('ble-continuous-scan-control');
        if (scanControlDiv) {
          scanControlDiv.style.display = 'none';
        }
      }
      
      // Refresh contact sensor status
      if (AppState.ws && AppState.ws.readyState === WebSocket.OPEN) {
        AppState.ws.send('contact_sensor_status');
      }
    }
    
    // ============================================================================
    // BLE Unpair
    // ============================================================================
    
    function unpairBLE() {
      if (!confirm('Remove this sensor? The shutter will no longer react to window open/close events.')) {
        return;
      }
      
      if (AppState.ws && AppState.ws.readyState === WebSocket.OPEN) {
        showLoading('Unpairing device...');
        
        AppState.ws.send(JSON.stringify({cmd: 'ble_unpair'}));
        
        setTimeout(() => {
          AppState.ws.send('ble_status');
          hideLoading();
          showErrorBanner('Device Unpaired', 'BLE sensor has been removed', 'success');
        }, 1000);
      }
    }
    
    // ============================================================================
    // Contact Sensor Matter Toggle
    // ============================================================================
    
    function updateContactSensorToggle(enabled, active) {
      AppState.contactSensorMatterEnabled = enabled;
      AppState.contactSensorEndpointActive = active;
      
      const offBtn = document.getElementById('matter-toggle-off');
      const onBtn = document.getElementById('matter-toggle-on');
      
      if (enabled) {
        offBtn.classList.remove('active');
        offBtn.setAttribute('aria-pressed', 'false');
        onBtn.classList.add('active');
        onBtn.setAttribute('aria-pressed', 'true');
      } else {
        onBtn.classList.remove('active');
        onBtn.setAttribute('aria-pressed', 'false');
        offBtn.classList.add('active');
        offBtn.setAttribute('aria-pressed', 'true');
      }
      
      // Status Text
      let statusText = '';
      let statusColor = '';
      
      if (enabled && active) {
        statusText = '✓ Active in Matter';
        statusColor = 'var(--success-color)';
      } else if (enabled && !active) {
        statusText = '⏳ Waiting for sensor data...';
        statusColor = 'var(--warning-color)';
      } else {
        statusText = '❌ Disabled';
        statusColor = 'var(--text-secondary)';
      }
      
      const statusEl = document.getElementById('matter-toggle-status-text');
      if (statusEl) {
        statusEl.innerText = statusText;
        statusEl.style.color = statusColor;
      }
    }
    
    function setContactSensorMatter(enable) {
      if (AppState.ws && AppState.ws.readyState === WebSocket.OPEN) {
        const cmd = enable ? 'contact_sensor_enable' : 'contact_sensor_disable';
        AppState.ws.send(cmd);
        
        // Optimistic UI update
        updateContactSensorToggle(enable, AppState.contactSensorEndpointActive);
        
        // Status refresh after 1 second
        setTimeout(() => {
          if (AppState.ws && AppState.ws.readyState === WebSocket.OPEN) {
            AppState.ws.send('contact_sensor_status');
          }
        }, 1000);
        
        const message = enable 
          ? 'Contact sensor enabled for Matter' 
          : 'Contact sensor disabled';
        showErrorBanner('Matter Integration', message, 'success');
      }
    }
    
    // ============================================================================
    // Utility Functions
    // ============================================================================
    
    function escapeHtml(unsafe) {
      if (!unsafe) return '';
      return unsafe
        .toString()
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;")
        .replace(/"/g, "&quot;")
        .replace(/'/g, "&#039;");
    }
    
    // ============================================================================
    // Keyboard Accessibility
    // ============================================================================
    
    // ESC key to close modals
    document.addEventListener('keydown', (e) => {
      if (e.key === 'Escape') {
        // Close any open modal
        const modals = document.querySelectorAll('.modal:not(.hidden)');
        modals.forEach(modal => {
          modal.classList.add('hidden');
          modal.setAttribute('aria-hidden', 'true');
        });
        
        // Close error banner
        hideErrorBanner();
      }
    });
    
    // Trap focus in modals
    document.querySelectorAll('.modal').forEach(modal => {
      modal.addEventListener('keydown', (e) => {
        if (e.key === 'Tab') {
          const focusableElements = modal.querySelectorAll(
            'button:not(:disabled), input:not(:disabled), [tabindex]:not([tabindex="-1"])'
          );
          
          const firstElement = focusableElements[0];
          const lastElement = focusableElements[focusableElements.length - 1];
          
          if (e.shiftKey && document.activeElement === firstElement) {
            e.preventDefault();
            lastElement.focus();
          } else if (!e.shiftKey && document.activeElement === lastElement) {
            e.preventDefault();
            firstElement.focus();
          }
        }
      });
    });
    
    // ============================================================================
    // Initialize
    // ============================================================================
    
    // Start WebSocket connection when page loads
    document.addEventListener('DOMContentLoaded', () => {
      console.log('🚀 BeltWinder Matter - Initializing...');
      connectWebSocket();
      
      // Show overview tab by default
      show('overview');
      
      console.log('✓ Application initialized');
    });
    
    // ============================================================================
    // Safari-specific fixes
    // ============================================================================
    
    // Detect Safari
    const isSafari = /^((?!chrome|android).)*safari/i.test(navigator.userAgent);
    
    if (isSafari) {
      console.log('🍎 Safari detected - applying compatibility fixes');
      
      // Safari has issues with backdrop-filter on some elements
      // Force hardware acceleration
      document.querySelectorAll('.card, .modal-box, .header').forEach(el => {
        el.style.transform = 'translateZ(0)';
      });
    }
    
    // ============================================================================
    // Network Status Monitoring
    // ============================================================================
    
    window.addEventListener('online', () => {
      console.log('📶 Network online');
      showErrorBanner('Network Restored', 'Connection is back online', 'success');
      
      // Reconnect WebSocket if needed
      if (!AppState.ws || AppState.ws.readyState !== WebSocket.OPEN) {
        connectWebSocket();
      }
    });
    
    window.addEventListener('offline', () => {
      console.log('📵 Network offline');
      showErrorBanner('Network Lost', 'No internet connection', 'error');
    });
    
    // ============================================================================
    // Performance Monitoring
    // ============================================================================
    
    if ('performance' in window && 'memory' in performance) {
      setInterval(() => {
        const memory = performance.memory;
        const usedMB = (memory.usedJSHeapSize / 1048576).toFixed(1);
        const totalMB = (memory.jsHeapSizeLimit / 1048576).toFixed(1);
        
        console.log(`💾 Memory: ${usedMB}MB / ${totalMB}MB`);
        
        // Warn if memory usage is high
        if (memory.usedJSHeapSize / memory.jsHeapSizeLimit > 0.9) {
          console.warn('⚠️ High memory usage detected');
        }
      }, 60000); // Check every minute
    }
    
    // ============================================================================
    // Visibility Change Handling
    // ============================================================================
    
    document.addEventListener('visibilitychange', () => {
      if (document.hidden) {
        console.log('👁️ Page hidden - reducing updates');
        // Optionally reduce update frequency when page is not visible
      } else {
        console.log('👁️ Page visible - resuming normal updates');
        // Refresh status immediately when page becomes visible
        if (AppState.ws && AppState.ws.readyState === WebSocket.OPEN) {
          AppState.ws.send('status');
          AppState.ws.send('matter_status');
          
          const currentTab = document.querySelector('.nav button.active').id.replace('nav-', '');
          if (currentTab === 'ble') {
            AppState.ws.send('ble_status');
          }
        }
      }
    });
    
    // ============================================================================
    // Touch Event Optimization for Mobile
    // ============================================================================
    
    // Add touch-friendly feedback
    if ('ontouchstart' in window) {
      console.log('📱 Touch device detected');
      
      document.querySelectorAll('.btn, .direction-btn, .nav button').forEach(el => {
        el.addEventListener('touchstart', function() {
          this.style.transform = 'scale(0.95)';
        }, { passive: true });
        
        el.addEventListener('touchend', function() {
          setTimeout(() => {
            this.style.transform = '';
          }, 100);
        }, { passive: true });
      });
    }
    
    // ============================================================================
    // Auto-reconnect Logic Enhancement
    // ============================================================================
    
    let reconnectAttempts = 0;
    const MAX_RECONNECT_ATTEMPTS = 10;
    const BASE_RECONNECT_DELAY = 1000;
    
    function enhancedReconnect() {
      if (reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
        console.error('❌ Max reconnection attempts reached');
        showErrorBanner(
          'Connection Failed', 
          'Unable to connect to device. Please check your network and refresh the page.', 
          'error'
        );
        clearInterval(AppState.reconnectInterval);
        return;
      }
      
      reconnectAttempts++;
      const delay = Math.min(BASE_RECONNECT_DELAY * Math.pow(2, reconnectAttempts - 1), 30000);
      
      console.log(`🔄 Reconnection attempt ${reconnectAttempts}/${MAX_RECONNECT_ATTEMPTS} in ${delay}ms`);
      
      setTimeout(() => {
        connectWebSocket();
      }, delay);
    }
    
    // Reset reconnect counter on successful connection
    const originalOnOpen = AppState.ws ? AppState.ws.onopen : null;
    if (AppState.ws) {
      AppState.ws.addEventListener('open', () => {
        reconnectAttempts = 0;
        console.log('✓ Reconnection counter reset');
      });
    }
    
    // ============================================================================
    // Console Commands for Debugging
    // ============================================================================
    
    window.BeltWinder = {
      // Get current state
      getState: () => {
        console.log('Current AppState:', AppState);
        return AppState;
      },
      
      // Send raw command
      send: (cmd) => {
        if (AppState.ws && AppState.ws.readyState === WebSocket.OPEN) {
          AppState.ws.send(cmd);
          console.log('✓ Sent:', cmd);
        } else {
          console.error('✗ WebSocket not connected');
        }
      },
      
      // Force reconnect
      reconnect: () => {
        console.log('🔄 Forcing reconnection...');
        if (AppState.ws) {
          AppState.ws.close();
        }
        reconnectAttempts = 0;
        connectWebSocket();
      },
      
      // Show test notification
      testNotification: (type = 'success') => {
        showErrorBanner('Test Notification', 'This is a test message', type);
      },
      
      // Get discovered BLE devices
      getDiscoveredDevices: () => {
        console.log('Discovered BLE devices:', AppState.discoveredDevices);
        return AppState.discoveredDevices;
      },
      
      // Debug mode toggle
      debugMode: false,
      toggleDebug: () => {
        window.BeltWinder.debugMode = !window.BeltWinder.debugMode;
        console.log('Debug mode:', window.BeltWinder.debugMode ? 'ON' : 'OFF');
        
        if (window.BeltWinder.debugMode) {
          // Enable verbose WebSocket logging
          const originalOnMessage = AppState.ws.onmessage;
          AppState.ws.onmessage = (e) => {
            console.log('📨 [DEBUG] Raw message:', e.data);
            originalOnMessage(e);
          };
        }
      }
    };
    
    console.log('💡 Debug commands available via window.BeltWinder');
    console.log('   - BeltWinder.getState()');
    console.log('   - BeltWinder.send(cmd)');
    console.log('   - BeltWinder.reconnect()');
    console.log('   - BeltWinder.testNotification(type)');
    console.log('   - BeltWinder.getDiscoveredDevices()');
    console.log('   - BeltWinder.toggleDebug()');
    
    // ============================================================================
    // Service Worker Registration (Optional - for PWA support)
    // ============================================================================
    
    if ('serviceWorker' in navigator) {
      // Uncomment to enable PWA features
      /*
      navigator.serviceWorker.register('/sw.js')
        .then(registration => {
          console.log('✓ Service Worker registered:', registration);
        })
        .catch(error => {
          console.log('✗ Service Worker registration failed:', error);
        });
      */
    }
    
    // ============================================================================
    // Analytics/Error Tracking Stub
    // ============================================================================
    
    window.addEventListener('error', (event) => {
      console.error('💥 Global error:', event.error);
      
      // You can send this to an analytics service
      // Example: sendToAnalytics('error', event.error);
    });
    
    window.addEventListener('unhandledrejection', (event) => {
      console.error('💥 Unhandled promise rejection:', event.reason);
      
      // You can send this to an analytics service
      // Example: sendToAnalytics('unhandledRejection', event.reason);
    });
    
    // ============================================================================
    // Cleanup on Page Unload
    // ============================================================================
    
    window.addEventListener('beforeunload', () => {
      console.log('👋 Page unloading - cleaning up...');
      
      // Close WebSocket gracefully
      if (AppState.ws && AppState.ws.readyState === WebSocket.OPEN) {
        AppState.ws.close();
      }
      
      // Clear intervals
      clearInterval(AppState.statusInterval);
      clearInterval(AppState.reconnectInterval);
    });
    
    // ============================================================================
    // Animation Performance Optimization
    // ============================================================================
    
    // Pause animations when not visible
    let animationFrameId = null;
    
    function optimizeAnimations() {
      if (document.hidden) {
        // Pause CSS animations
        document.body.style.animationPlayState = 'paused';
      } else {
        // Resume CSS animations
        document.body.style.animationPlayState = 'running';
      }
    }
    
    document.addEventListener('visibilitychange', optimizeAnimations);
    
    // ============================================================================
    // Battery Status API (if available)
    // ============================================================================
    
    if ('getBattery' in navigator) {
      navigator.getBattery().then(battery => {
        console.log('🔋 Device battery:', (battery.level * 100) + '%');
        
        // Reduce update frequency if battery is low
        if (battery.level < 0.15) {
          console.log('⚡ Low battery detected - reducing update frequency');
          // Optionally adjust status interval
        }
        
        battery.addEventListener('levelchange', () => {
          console.log('🔋 Battery level changed:', (battery.level * 100) + '%');
        });
      });
    }
    
    // ============================================================================
    // Responsive Image Loading
    // ============================================================================
    
    // Lazy load QR code image only when modal is opened
    const originalShowMatterQR = showMatterQR;
    showMatterQR = function(qrUrl, qrImageUrl, pairingCode) {
      const img = document.getElementById('matter-qr-img');
      
      // Only load image if not already loaded
      if (!img.src || img.src !== qrImageUrl) {
        img.src = qrImageUrl;
      }
      
      originalShowMatterQR(qrUrl, qrImageUrl, pairingCode);
    };
    
    // ============================================================================
    // Internationalization Stub
    // ============================================================================
    
    // Detect browser language
    const userLanguage = navigator.language || navigator.userLanguage;
    console.log('🌍 Browser language:', userLanguage);
    
    // You can implement multi-language support here
    // Example: loadTranslations(userLanguage);
    
    // ============================================================================
    // Theme Support (Optional)
    // ============================================================================
    
    // Detect system theme preference
    if (window.matchMedia) {
      const darkModeQuery = window.matchMedia('(prefers-color-scheme: dark)');
      
      console.log('🎨 System theme:', darkModeQuery.matches ? 'dark' : 'light');
      
      // Listen for theme changes
      darkModeQuery.addEventListener('change', (e) => {
        console.log('🎨 Theme changed to:', e.matches ? 'dark' : 'light');
        // You can adjust UI based on theme preference
      });
    }
    
    // ============================================================================
    // End of Script
    // ============================================================================
    
    console.log('✅ BeltWinder Matter UI - Fully Loaded');
    console.log('📍 Version: 1.3.0');
    console.log('🔧 Debug mode: Type BeltWinder.toggleDebug() in console');
    
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
    
    // Socket-Handling optimieren
    cfg.max_open_sockets = 5;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 4;
    cfg.stack_size = 8192;
    cfg.ctrl_port = 32768;
    cfg.close_fn = nullptr;
    cfg.uri_match_fn = nullptr;
    cfg.keep_alive_enable = false;
    cfg.keep_alive_idle = 0;
    cfg.keep_alive_interval = 0;
    cfg.keep_alive_count = 0;

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
            .handle_ws_control_frames = true
        };
        httpd_register_uri_handler(server, &ws);
        
        ESP_LOGI(TAG, "HTTP server started");
        ESP_LOGI(TAG, "  Max open sockets: %d", cfg.max_open_sockets);
        ESP_LOGI(TAG, "  LRU purge: %s", cfg.lru_purge_enable ? "enabled" : "disabled");
        
        // ✅ BLE Callbacks registrieren
        if (bleManager) {
            ESP_LOGI(TAG, "═══════════════════════════════════");
            ESP_LOGI(TAG, "REGISTERING BLE CALLBACKS");
            
            // State Change ist OK hier, das betrifft nur UI
            bleManager->setStateChangeCallback([this](auto oldState, auto newState) {
                broadcastBLEStateChange(oldState, newState);
            });
             
            ESP_LOGI(TAG, "✓ BLE State Callback registered");
            ESP_LOGI(TAG, "ℹ Sensor Data forwarded via Main Loop");
            ESP_LOGI(TAG, "═══════════════════════════════════");
        }
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
        size_t msg_len = strlen(message);
        
        httpd_ws_frame_t ws_pkt;
        memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
        ws_pkt.type = HTTPD_WS_TYPE_TEXT;
        ws_pkt.payload = (uint8_t*)message;
        ws_pkt.len = msg_len;
        
        // Kopie der Client-Liste, um Deadlocks zu vermeiden, falls unregister aufgerufen wird
        std::vector<int> target_fds;
        for (const auto& client : active_clients) {
            target_fds.push_back(client.fd);
        }
        xSemaphoreGive(client_mutex);
        
        for (int fd : target_fds) {
            // Sende direkt - ESP IDF kopiert den Buffer in den TCP Stack
            esp_err_t ret = httpd_ws_send_frame_async(server, fd, &ws_pkt);
            
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to send to fd=%d: %s", fd, esp_err_to_name(ret));
                // Optional: Hier könnte man Clients zum Löschen markieren
            }
        }
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
        
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║  WEBSOCKET CONNECTION INCOMING    ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "Socket FD: %d", fd);
        ESP_LOGI(TAG, "User-Agent: %s", req->user_ctx ? "present" : "NULL");
        
        // IP-Adresse extrahieren
        struct sockaddr_in6 addr;
        socklen_t addr_len = sizeof(addr);
        if (getpeername(fd, (struct sockaddr*)&addr, &addr_len) == 0) {
            char ip_str[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &addr.sin6_addr, ip_str, sizeof(ip_str));
            ESP_LOGI(TAG, "Client IP: %s", ip_str);
        }
        
        ESP_LOGI(TAG, "═══════════════════════════════════");
        
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

    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "COMMAND DEBUG");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    ESP_LOGI(TAG, "Command: %s", cmd);
    ESP_LOGI(TAG, "Length: %d", strlen(cmd));

    // Prüfe ob es JSON ist
    if (cmd[0] == '{') {
        ESP_LOGI(TAG, "→ JSON Command detected");
        
        // Parse cmd type
        String json = String(cmd);
        int cmdStart = json.indexOf("\"cmd\":\"") + 7;
        int cmdEnd = json.indexOf("\"", cmdStart);
        String cmdType = json.substring(cmdStart, cmdEnd);
        
        ESP_LOGI(TAG, "→ Command Type: %s", cmdType.c_str());
    }

    ESP_LOGI(TAG, "═══════════════════════════════════");
    
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
    ESP_LOGI(TAG, "WebSocket: BLE DISCOVERY SCAN");
    ESP_LOGI(TAG, "═══════════════════════════════════");
    
    if (self->bleManager) {
        // ════════════════════════════════════════════════════════════════
        // ✅ Ensure BLE is started BEFORE scanning
        // ════════════════════════════════════════════════════════════════
        
        if (!self->bleManager->isBLEStarted()) {
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "→ BLE not started yet");
            ESP_LOGI(TAG, "  Starting BLE for discovery scan...");
            
            if (!self->bleManager->ensureBLEStarted()) {
                ESP_LOGE(TAG, "✗ Failed to start BLE");
                
                const char* error = "{\"type\":\"error\",\"message\":\"Failed to start BLE\"}";
                httpd_ws_frame_t frame = {
                    .type = HTTPD_WS_TYPE_TEXT,
                    .payload = (uint8_t*)error,
                    .len = strlen(error)
                };
                httpd_ws_send_frame_async(req->handle, fd, &frame);
                
                free(buf);
                return ESP_OK;
            }
            
            ESP_LOGI(TAG, "✓ BLE started successfully");
            ESP_LOGI(TAG, "");
            
            // Kurze Pause für BLE Stack
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        
        ESP_LOGI(TAG, "Starting 10-second discovery scan...");
        ESP_LOGI(TAG, "Will stop on first Shelly BLU Door/Window found!");
        
        // Start Scan
        self->bleManager->startScan(10, true);

            xTaskCreate([](void *param) {
                WebUIHandler *handler = (WebUIHandler *)param;
                
                ESP_LOGI("WebUI", "📡 Scan task started");

                // Wait for scan completion
                uint32_t elapsed = 0;
                const uint32_t max_duration = 12000;
                
                while (elapsed < max_duration) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                    elapsed += 100;
                    
                    if (handler->bleManager && !handler->bleManager->isScanActive()) {
                        ESP_LOGI("WebUI", "✓ Scan ended at %u ms", elapsed);
                        break;
                    }
                }

                // Send completion
                const char *complete_msg = "{\"type\":\"ble_scan_complete\"}";
                handler->broadcast_to_all_clients(complete_msg);
                ESP_LOGI("WebUI", "✓ Scan complete sent");

                vTaskDelay(pdMS_TO_TICKS(200));

                // Send devices
                if (handler->bleManager) {
                    std::vector<ShellyBLEDevice> discovered = handler->bleManager->getDiscoveredDevices();

                    if (discovered.size() > 0) {
                        // ✅ FIXED: Lokaler Buffer (kein static mehr)
                        char json_buf[2048];
                        
                        int offset = snprintf(json_buf, sizeof(json_buf),
                                              "{\"type\":\"ble_discovered\",\"devices\":[");

                        for (size_t i = 0; i < discovered.size() && i < 10; i++) {
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
                        ESP_LOGI("WebUI", "✓ Sent %d devices", discovered.size());
                    } else {
                        const char *empty = "{\"type\":\"ble_discovered\",\"devices\":[]}";
                        handler->broadcast_to_all_clients(empty);
                        ESP_LOGI("WebUI", "ℹ No devices found");
                    }
                }

                ESP_LOGI("WebUI", "✓ Task complete");
                
                // ✅ CRITICAL: Delete task!
                vTaskDelete(NULL);
                
            }, "ble_scan_mon", 4096, self, 1, NULL);
        }
    }

    else if (strcmp(cmd, "ble_status") == 0) {
    if (self->bleManager) {
        ESP_LOGI(TAG, "BLE status requested");
        
        // Discovery List
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
        
        ShellyBLEManager::DeviceState state = self->bleManager->getDeviceState();
        const char* stateStr = "not_paired";
        
        if (state == ShellyBLEManager::STATE_CONNECTED_UNENCRYPTED) {
            stateStr = "connected_unencrypted";
        } else if (state == ShellyBLEManager::STATE_CONNECTED_ENCRYPTED) {
            stateStr = "connected_encrypted";
        }
        
        if (paired) {
            PairedShellyDevice device = self->bleManager->getPairedDevice();
            ShellyBLESensorData sensorData;
            bool hasData = self->bleManager->getSensorData(sensorData);

            // Passkey und Bindkey aus NVS laden
            String passkey = "Not set";
            String bindkey = device.bindkey;

            Preferences prefs;
            if (prefs.begin("ShellyBLE", true)) {
                // Passkey aus Preferences (falls gespeichert)
                uint32_t stored_passkey = prefs.getUInt("passkey", 0);
                if (stored_passkey > 0) {
                    char pk_buf[8];
                    snprintf(pk_buf, sizeof(pk_buf), "%06u", stored_passkey);
                    passkey = String(pk_buf);
                }
                prefs.end();
            }
            
            // Wenn kein Bindkey vorhanden, versuche aus NVS zu laden
            if (bindkey.length() == 0) {
                if (prefs.begin("ShellyBLE", true)) {
                    bindkey = prefs.getString("bindkey", "");
                    prefs.end();
                }
            }

            bool continuousScanActive = self->bleManager->isScanActive();
            
            offset = snprintf(json_buf, sizeof(json_buf),
                             "{\"type\":\"ble_status\","
                             "\"paired\":true,"
                             "\"state\":\"%s\","
                             "\"name\":\"%s\","
                             "\"address\":\"%s\","
                             "\"passkey\":\"%s\","
                             "\"bindkey\":\"%s\","
                             "\"continuous_scan_active\":%s,"
                             "\"sensor_data\":{",
                             stateStr,
                             device.name.c_str(),
                             device.address.c_str(),
                             passkey.c_str(),
                             bindkey.c_str(),
                             continuousScanActive ? "true" : "false");
            
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
            bool isScanActive = self->bleManager ? self->bleManager->isScanActive() : false;
        snprintf(json_buf, sizeof(json_buf),
                "{\"type\":\"ble_status\","
                "\"paired\":false,"
                "\"state\":\"%s\","
                "\"continuous_scan_active\":%s}",
                stateStr,
                isScanActive ? "true" : "false");
        }
        
        frame.payload = (uint8_t*)json_buf;
        frame.len = strlen(json_buf);
        httpd_ws_send_frame_async(req->handle, fd, &frame);
    }
}

// ============================================================================
// ✅ SMART CONNECT COMMAND (3-in-1 Workflow)
// ============================================================================

else if (strncmp(cmd, "{\"cmd\":\"ble_smart_connect\"", 26) == 0) {
    if (self->bleManager) {
        String json = String(cmd);
        
        // Parse address
        int addrStart = json.indexOf("\"address\":\"") + 11;
        int addrEnd = json.indexOf("\"", addrStart);
        String address = json.substring(addrStart, addrEnd);
        
        // Parse passkey
        int passkeyStart = json.indexOf("\"passkey\":") + 10;
        int passkeyEnd = json.indexOf(",", passkeyStart);
        if (passkeyEnd == -1) passkeyEnd = json.indexOf("}", passkeyStart);
        String passkeyStr = json.substring(passkeyStart, passkeyEnd);
        uint32_t passkey = passkeyStr.toInt();
        
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "SMART CONNECT");
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "Address: %s", address.c_str());
        ESP_LOGI(TAG, "Mode: %s", passkey > 0 ? "ENCRYPTED" : "UNENCRYPTED");
        if (passkey > 0) {
            ESP_LOGI(TAG, "Passkey: %06u", passkey);
        }
        
        // ✅ WICHTIG: Button-Press Anleitung VORHER senden
        const char* instructions = 
            "{\"type\":\"info\",\"message\":\"<strong>📋 GET READY!</strong><br><br>"
            "<strong>RIGHT NOW:</strong><br>"
            "1. Press and HOLD the button on the device<br>"
            "2. Keep holding... (count to 15)<br>"
            "3. LED should flash rapidly<br><br>"
            "Starting connection in 5 seconds...<br>"
            "Keep holding the button!\"}";
        
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)instructions,
            .len = strlen(instructions)
        };
        httpd_ws_send_frame_async(req->handle, fd, &frame);
        
        // Gib User 5 Sekunden
        vTaskDelay(pdMS_TO_TICKS(5000));
        
        // ✅ Task-basierte Ausführung (non-blocking)
        struct SmartConnectParams {
            ShellyBLEManager* bleManager;
            WebUIHandler* handler;
            int fd;
            String address;
            uint32_t passkey;
        };
        
        SmartConnectParams* params = new SmartConnectParams{
            self->bleManager,
            self,
            fd,
            address,
            passkey
        };
        
        xTaskCreate([](void* pvParameters) {
            SmartConnectParams* p = (SmartConnectParams*)pvParameters;
            
            ESP_LOGI(TAG, "🚀 Smart Connect Task started");
            ESP_LOGI(TAG, "   Address: %s", p->address.c_str());
            ESP_LOGI(TAG, "   Passkey: %s", p->passkey > 0 ? "SET" : "NONE");
            
            // Watchdog entfernen (kann lange dauern)
            esp_task_wdt_delete(NULL);
            
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "Pre-connection Scanner Status:");
            if (p->bleManager->isScanActive()) {
                ESP_LOGW(TAG, "  ⚠ Scanner is ACTIVE - will be stopped by connectDevice()");
            } else {
                ESP_LOGI(TAG, "  ✓ Scanner is IDLE - ready for GATT connection");
            }
            ESP_LOGI(TAG, "");
            
            // ✅ Smart Connect aufrufen (stoppt Scanner automatisch)
            bool success = p->bleManager->smartConnectDevice(p->address, p->passkey);
            
            if (success) {
                PairedShellyDevice device = p->bleManager->getPairedDevice();
                
                char success_msg[1024];
                
                if (p->passkey > 0) {
                    // Encrypted Mode Success
                    snprintf(success_msg, sizeof(success_msg),
                            "{\"type\":\"success\",\"message\":\"<strong>✅ Encrypted Connection Complete!</strong><br><br>"
                            "Your device is now:<br>"
                            "✓ Bonded (trusted connection)<br>"
                            "✓ Encrypted (passkey: %06u)<br>"
                            "✓ Bindkey received: %s<br><br>"
                            "<strong>⚠️ SAVE YOUR CREDENTIALS!</strong><br>"
                            "You will need them for future connections.<br><br>"
                            "Continuous scan is now active.\"}",
                            p->passkey,
                            device.bindkey.c_str());
                } else {
                    // Unencrypted Mode Success
                    snprintf(success_msg, sizeof(success_msg),
                            "{\"type\":\"success\",\"message\":\"<strong>✓ Device Connected!</strong><br><br>"
                            "The device is bonded but NOT encrypted yet.<br><br>"
                            "You can enable encryption later via the UI.<br><br>"
                            "Continuous scan is now active.\"}");
                }
                
                // Sende Success Message
                if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    httpd_ws_frame_t frame = {
                        .type = HTTPD_WS_TYPE_TEXT,
                        .payload = (uint8_t*)success_msg,
                        .len = strlen(success_msg)
                    };
                    httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                    xSemaphoreGive(p->handler->client_mutex);
                }
                
                // Modal schließen
                vTaskDelay(pdMS_TO_TICKS(2000));
                p->handler->sendModalClose(p->fd, "ble-connect-modal");
                
                ESP_LOGI(TAG, "✓ Smart Connect successful");
                
                // Status-Update senden
                vTaskDelay(pdMS_TO_TICKS(1000));
                
                const char* stateStr = (p->passkey > 0) ? "connected_encrypted" : "connected_unencrypted";
                
                char status_buf[512];
                snprintf(status_buf, sizeof(status_buf),
                        "{\"type\":\"ble_status\","
                        "\"paired\":true,"
                        "\"state\":\"%s\","
                        "\"name\":\"%s\","
                        "\"address\":\"%s\","
                        "\"passkey\":\"%06u\","
                        "\"bindkey\":\"%s\","
                        "\"sensor_data\":{\"valid\":false}}",
                        stateStr,
                        device.name.c_str(),
                        device.address.c_str(),
                        p->passkey,
                        device.bindkey.c_str());
                
                p->handler->broadcast_to_all_clients(status_buf);
                
            } else {
                // Fehler
                const char* error = 
                    "{\"type\":\"error\",\"message\":\"<strong>✗ Connection Failed</strong><br><br>"
                    "<strong>Most likely causes:</strong><br><br>"
                    "1️⃣ <strong>Button not held long enough</strong><br>"
                    "   → Must hold for FULL 15 seconds<br>"
                    "   → LED must flash RAPIDLY<br><br>"
                    "2️⃣ <strong>Device too far away</strong><br>"
                    "   → Move within 2 meters<br><br>"
                    "3️⃣ <strong>Wrong passkey</strong> (if encrypted)<br>"
                    "   → Try factory reset first<br><br>"
                    "<strong>Try again!</strong>\"}";
                
                if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    httpd_ws_frame_t frame = {
                        .type = HTTPD_WS_TYPE_TEXT,
                        .payload = (uint8_t*)error,
                        .len = strlen(error)
                    };
                    httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                    xSemaphoreGive(p->handler->client_mutex);
                }
                
                ESP_LOGE(TAG, "✗ Smart Connect failed");
            }
            
            ESP_LOGI(TAG, "✓ Smart Connect Task completed");
            
            delete p;
            vTaskDelete(NULL);

            UBaseType_t highWater = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "Task Stack (ble_smart) usage: %u bytes free", highWater * sizeof(StackType_t));  
            
        }, "ble_smart", 4096, params, 5, NULL);
        
        ESP_LOGI(TAG, "✓ Smart Connect task created");
    }
}

// ============================================================================
// BLE Connect Command (Phase 1) - Mit Smart Detection
// ============================================================================
else if (strncmp(cmd, "{\"cmd\":\"ble_connect\"", 20) == 0) {
    if (self->bleManager) {
        String json = String(cmd);
        
        int addrStart = json.indexOf("\"address\":\"") + 11;
        int addrEnd = json.indexOf("\"", addrStart);
        String address = json.substring(addrStart, addrEnd);
        
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "BLE CONNECT (Phase 1: Bonding)");
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "Address: %s", address.c_str());
        
        // ✅ Sende Anweisungen VOR dem Connect-Versuch
        const char* instructions = 
            "{\"type\":\"info\",\"message\":\"<strong>📋 GET READY!</strong><br><br>"
            "<strong>RIGHT NOW:</strong><br>"
            "1. Press and HOLD the button on the device<br>"
            "2. Keep holding... (count to 15)<br>"
            "3. LED should flash rapidly<br><br>"
            "Starting connection in 5 seconds...<br>"
            "Keep holding the button!\"}";
        
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)instructions,
            .len = strlen(instructions)
        };
        httpd_ws_send_frame_async(req->handle, fd, &frame);
        
        // ✅ Gib User 5 Sekunden zum Lesen + Button drücken
        vTaskDelay(pdMS_TO_TICKS(5000));
        
        // ✅ Jetzt verbinden
        if (self->bleManager->connectDevice(address)) {
            const char* success = 
                "{\"type\":\"success\",\"message\":\"<strong>✓ Bonding Complete!</strong><br><br>"
                "The device is now bonded and ready.<br><br>"
                "<strong>Connection is ACTIVE</strong><br><br>"
                "Next steps:<br>"
                "• Click 'Enable Encryption' to set passkey<br>"
                "• NO button press needed for encryption!<br><br>"
                "Note: Device is bonded but NOT encrypted yet.\"}";
            
            httpd_ws_frame_t frame = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t*)success,
                .len = strlen(success)
            };
            httpd_ws_send_frame_async(req->handle, fd, &frame);

            vTaskDelay(pdMS_TO_TICKS(2000));
            self->sendModalClose(fd, "ble-connect-modal");
            
            ESP_LOGI(TAG, "✓ Bonding successful");
            
            // Status-Update
            vTaskDelay(pdMS_TO_TICKS(1000));
            
            PairedShellyDevice device = self->bleManager->getPairedDevice();
            
            char status_buf[512];
            snprintf(status_buf, sizeof(status_buf),
                    "{\"type\":\"ble_status\","
                    "\"paired\":true,"
                    "\"state\":\"connected_unencrypted\","
                    "\"name\":\"%s\","
                    "\"address\":\"%s\","
                    "\"sensor_data\":{\"valid\":false}}",
                    device.name.c_str(),
                    device.address.c_str());
            
            self->broadcast_to_all_clients(status_buf);
            
        } else {
            // ✅ BESSERE ERROR MESSAGE mit Troubleshooting
            const char* error = 
                "{\"type\":\"error\",\"message\":\"<strong>✗ Bonding Failed</strong><br><br>"
                "<strong>Most likely causes:</strong><br><br>"
                "1️⃣ <strong>Button not held long enough</strong><br>"
                "   → Must hold for FULL 15 seconds<br>"
                "   → LED must flash RAPIDLY (not slowly)<br><br>"
                "2️⃣ <strong>Device too far away</strong><br>"
                "   → Move device within 2 meters of ESP32<br><br>"
                "3️⃣ <strong>Device already bonded elsewhere</strong><br>"
                "   → Reset device first (hold button 30+ seconds)<br><br>"
                "4️⃣ <strong>Wrong address type</strong><br>"
                "   → Try scanning again<br><br>"
                "<strong>Try again and follow timing exactly!</strong>\"}";
            
            httpd_ws_frame_t frame = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t*)error,
                .len = strlen(error)
            };
            httpd_ws_send_frame_async(req->handle, fd, &frame);
            
            ESP_LOGE(TAG, "✗ Bonding failed");
        }
    }
}

else if (strncmp(cmd, "{\"cmd\":\"ble_encrypt\"", 20) == 0) {
    if (self->bleManager) {
        String json = String(cmd);
        
        // 1. Parameter manuell extrahieren (wie bei ble_connect)
        int addrStart = json.indexOf("\"address\":\"") + 11;
        int addrEnd = json.indexOf("\"", addrStart);
        String address = json.substring(addrStart, addrEnd);

        int passStart = json.indexOf("\"passkey\":") + 10;
        // Suche das Ende der Zahl (entweder Komma oder schließende Klammer)
        int passEnd = json.indexOf(",", passStart);
        if (passEnd == -1) passEnd = json.indexOf("}", passStart);
        uint32_t passkey = json.substring(passStart, passEnd).toInt();

        // 2. Task-Parameter vorbereiten
        BLETaskParams* params = new BLETaskParams{self, fd, address, passkey};

        xTaskCreate([](void* pvParameters) {
            BLETaskParams* p = (BLETaskParams*)pvParameters;
            
            ESP_LOGI(TAG, "Starting Encryption Task for %s with Passkey %u", p->address.c_str(), p->passkey);

            // Info an UI senden
            const char* info = "{\"type\":\"info\",\"message\":\"Enabling encryption... Device will reboot.\"}";
            if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000))) {
                httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t*)info, .len = strlen(info) };
                httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                xSemaphoreGive(p->handler->client_mutex);
            }

            // 3. Die eigentliche Verschlüsselung im Manager aufrufen
            // Hier nutzen wir direkt den bleManager aus deinem 'self' (p->handler)
            if (p->handler->bleManager->enableEncryption(p->address, p->passkey)) {
                
                const char* success = "{\"type\":\"info\",\"message\":\"Encryption successful!\"}";
                if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000))) {
                    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t*)success, .len = strlen(success) };
                    httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                    xSemaphoreGive(p->handler->client_mutex);
                }

                // UI Status-Update senden
                PairedShellyDevice dev = p->handler->bleManager->getPairedDevice();
                char json_buf[1024];
                snprintf(json_buf, sizeof(json_buf),
                        "{\"type\":\"ble_status\","
                        "\"paired\":true,"
                        "\"state\":\"connected_encrypted\"," 
                        "\"name\":\"%s\","
                        "\"address\":\"%s\","
                        "\"sensor_data\":{\"valid\":false}}",
                        dev.name.c_str(), dev.address.c_str());
                        
                p->handler->broadcast_to_all_clients(json_buf);
            } else {
                const char* error = "{\"type\":\"error\",\"message\":\"Encryption failed. Check passkey!\"}";
                if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000))) {
                    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t*)error, .len = strlen(error) };
                    httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                    xSemaphoreGive(p->handler->client_mutex);
                }
            }

            delete p;
            vTaskDelete(NULL);

            UBaseType_t highWater = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "Task Stack (ble_enc_task) usage: %u bytes free", highWater * sizeof(StackType_t));

        }, "ble_enc_task", 6144, params, 1, NULL);
    }
}

// ============================================================================
// BLE Enable Encryption Command (Phase 2) - NON-BLOCKING
// ============================================================================
else if (strncmp(cmd, "{\"cmd\":\"ble_enable_encryption\"", 30) == 0) {
    if (self->bleManager) {
        String json = String(cmd);
        
        int addrStart = json.indexOf("\"address\":\"") + 11;
        int addrEnd = json.indexOf("\"", addrStart);
        String address = json.substring(addrStart, addrEnd);
        
        int passkeyStart = json.indexOf("\"passkey\":") + 10;
        int passkeyEnd = json.indexOf(",", passkeyStart);
        if (passkeyEnd == -1) passkeyEnd = json.indexOf("}", passkeyStart);
        String passkeyStr = json.substring(passkeyStart, passkeyEnd);
        uint32_t passkey = passkeyStr.toInt();
        
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "BLE ENABLE ENCRYPTION (Phase 2)");
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "Address: %s", address.c_str());
        ESP_LOGI(TAG, "Passkey: %06u", passkey);
        
        // Info-Message an Client senden
        const char* info = 
            "{\"type\":\"info\",\"message\":\"<strong>🔐 Phase 2: Enabling Encryption</strong><br><br>"
            "Using ACTIVE connection from Phase 1.<br>"
            "<strong>NO button press needed!</strong><br><br>"
            "Writing passkey and reading bindkey...\"}";
        
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)info,
            .len = strlen(info)
        };
        httpd_ws_send_frame_async(req->handle, fd, &frame);
        
        // ✅ Task-Parameter für non-blocking Execution
        struct EncryptionTaskParams {
            ShellyBLEManager* bleManager;
            WebUIHandler* handler;
            int fd;
            String address;
            uint32_t passkey;
        };
        
        EncryptionTaskParams* params = new EncryptionTaskParams{
            self->bleManager,
            self,
            fd,
            address,
            passkey
        };
        
        // ✅ Starte separaten Task für nicht-blockierende Ausführung!
        xTaskCreate([](void* pvParameters) {
            EncryptionTaskParams* p = (EncryptionTaskParams*)pvParameters;
            
            ESP_LOGI(TAG, "🔐 Encryption Task started for %s", p->address.c_str());
            
            // ✅ WICHTIG: Watchdog für diesen Task entfernen!
            // Dieser Task ist nicht zeitkritisch und kann lange dauern (bis zu 60s)
            esp_task_wdt_delete(NULL);
            
            // ✅ Enable Encryption (mit internen Watchdog-Resets)
            bool success = p->bleManager->enableEncryption(p->address, p->passkey);
            
            if (success) {
                // Hole Device-Info für Success-Message
                PairedShellyDevice device = p->bleManager->getPairedDevice();
                
                char success_msg[768];
                snprintf(success_msg, sizeof(success_msg),
                        "{\"type\":\"success\",\"message\":\"<strong>✅ Encryption Enabled!</strong><br><br>"
                        "Your device is now securely encrypted.<br><br>"
                        "<strong>🔑 Bindkey:</strong> %s<br><br>"
                        "⚠️ <strong>SAVE THIS BINDKEY!</strong><br>"
                        "You will need it for:<br>"
                        "• Re-pairing after factory reset<br>"
                        "• Integration with other systems<br>"
                        "• Backup and restore<br><br>"
                        "Continuous scan will now pick up sensor data...\"}",
                        device.bindkey.c_str());
                
                // Sende Erfolgs-Nachricht
                if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    httpd_ws_frame_t frame = {
                        .type = HTTPD_WS_TYPE_TEXT,
                        .payload = (uint8_t*)success_msg,
                        .len = strlen(success_msg)
                    };
                    httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                    xSemaphoreGive(p->handler->client_mutex);
                }
                
                ESP_LOGI(TAG, "✓ Encryption enabled successfully");

                vTaskDelay(pdMS_TO_TICKS(3000));
    
                  char close_msg[128];
                  snprintf(close_msg, sizeof(close_msg),
                          "{\"type\":\"modal_close\",\"modal_id\":\"enable-encryption-modal\"}");
                  
                  if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                      httpd_ws_frame_t frame = {
                          .type = HTTPD_WS_TYPE_TEXT,
                          .payload = (uint8_t*)close_msg,
                          .len = strlen(close_msg)
                      };
                      httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                      xSemaphoreGive(p->handler->client_mutex);
                  }
                
                // ✅ Status-Update nach kurzer Verzögerung
                vTaskDelay(pdMS_TO_TICKS(1000));
                
                char status_buf[512];
                snprintf(status_buf, sizeof(status_buf),
                        "{\"type\":\"ble_status\","
                        "\"paired\":true,"
                        "\"state\":\"connected_encrypted\","
                        "\"name\":\"%s\","
                        "\"address\":\"%s\","
                        "\"sensor_data\":{\"valid\":false}}",
                        device.name.c_str(),
                        device.address.c_str());
                
                p->handler->broadcast_to_all_clients(status_buf);
                
                // ✅ Continuous Scan starten
                ESP_LOGI(TAG, "→ Starting continuous scan for sensor data...");
                p->bleManager->startContinuousScan();
                
            } else {
                const char* error = 
                    "{\"type\":\"error\",\"message\":\"<strong>✗ Encryption Failed</strong><br><br>"
                    "Could not enable encryption.<br><br>"
                    "Possible reasons:<br>"
                    "• Wrong passkey<br>"
                    "• Device rejected passkey<br>"
                    "• Connection timeout<br>"
                    "• Bindkey not found in NVS<br><br>"
                    "Please try again or re-pair the device.\"}";
                
                if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    httpd_ws_frame_t frame = {
                        .type = HTTPD_WS_TYPE_TEXT,
                        .payload = (uint8_t*)error,
                        .len = strlen(error)
                    };
                    httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                    xSemaphoreGive(p->handler->client_mutex);
                }
                
                ESP_LOGE(TAG, "✗ Encryption failed");
            }
            
            ESP_LOGI(TAG, "✓ Encryption task completed");
            
            delete p;
            vTaskDelete(NULL);

            UBaseType_t highWater = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "Task Stack (ble_enc) usage: %u bytes free", highWater * sizeof(StackType_t));
            
        }, "ble_enc", 4096, params, 5, NULL);  // ✅ Stack: 8KB, Priorität: 5
        
        ESP_LOGI(TAG, "✓ Encryption task created");
    }
}

// ============================================================================
// ✅ PAIR ALREADY-ENCRYPTED DEVICE (Passkey + Bindkey Known)
// ============================================================================

else if (strncmp(cmd, "{\"cmd\":\"ble_pair_encrypted_known\"", 33) == 0) {
    if (self->bleManager) {
        String json = String(cmd);
        
        // Parse address
        int addrStart = json.indexOf("\"address\":\"") + 11;
        int addrEnd = json.indexOf("\"", addrStart);
        String address = json.substring(addrStart, addrEnd);
        
        // Parse passkey
        int passkeyStart = json.indexOf("\"passkey\":") + 10;
        int passkeyEnd = json.indexOf(",", passkeyStart);
        String passkeyStr = json.substring(passkeyStart, passkeyEnd);
        uint32_t passkey = passkeyStr.toInt();
        
        // Parse bindkey
        int bindkeyStart = json.indexOf("\"bindkey\":\"") + 11;
        int bindkeyEnd = json.indexOf("\"", bindkeyStart);
        String bindkey = json.substring(bindkeyStart, bindkeyEnd);
        
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "PAIR ALREADY-ENCRYPTED DEVICE");
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "Address: %s", address.c_str());
        ESP_LOGI(TAG, "Passkey: %06u", passkey);
        ESP_LOGI(TAG, "Bindkey: %s", bindkey.c_str());
        ESP_LOGI(TAG, "");
        
        // ✅ Validate inputs
        if (bindkey.length() != 32) {
            ESP_LOGE(TAG, "✗ Invalid bindkey length: %d (expected 32)", bindkey.length());
            
            const char* error = "{\"type\":\"error\",\"message\":\"Invalid bindkey length\"}";
            httpd_ws_frame_t frame = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t*)error,
                .len = strlen(error)
            };
            httpd_ws_send_frame_async(req->handle, fd, &frame);
            
            free(buf);
            return ESP_OK;
        }
        
        // ✅ Validate hex characters
        for (int i = 0; i < 32; i++) {
            char c = bindkey[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                ESP_LOGE(TAG, "✗ Invalid bindkey character at position %d: '%c'", i, c);
                
                const char* error = "{\"type\":\"error\",\"message\":\"Bindkey must contain only hex characters (0-9, a-f)\"}";
                httpd_ws_frame_t frame = {
                    .type = HTTPD_WS_TYPE_TEXT,
                    .payload = (uint8_t*)error,
                    .len = strlen(error)
                };
                httpd_ws_send_frame_async(req->handle, fd, &frame);
                
                free(buf);
                return ESP_OK;
            }
        }
        
        ESP_LOGI(TAG, "✓ Input validation passed");
        ESP_LOGI(TAG, "");
        
        // ✅ Info-Message an Client
        const char* info = 
            "{\"type\":\"info\",\"message\":\"<strong>🔐 Pairing with Encrypted Device</strong><br><br>"
            "Establishing secure connection...<br>"
            "This will:<br>"
            "• Bond with the device (no button press needed)<br>"
            "• Store passkey and bindkey<br>"
            "• Start decrypting broadcasts<br>"
            "• Begin continuous scanning\"}";
        
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)info,
            .len = strlen(info)
        };
        httpd_ws_send_frame_async(req->handle, fd, &frame);
        
        // ✅ Task-Parameter vorbereiten
        struct EncryptedKnownParams {
            ShellyBLEManager* bleManager;
            WebUIHandler* handler;
            int fd;
            String address;
            uint32_t passkey;
            String bindkey;
        };
        
        EncryptedKnownParams* params = new EncryptedKnownParams{
            self->bleManager,
            self,
            fd,
            address,
            passkey,
            bindkey
        };
        
        // ✅ Task erstellen (non-blocking)
        xTaskCreate([](void* pvParameters) {
            EncryptedKnownParams* p = (EncryptedKnownParams*)pvParameters;
            
            ESP_LOGI(TAG, "🔐 Already-Encrypted Pairing Task started");
            ESP_LOGI(TAG, "   Address: %s", p->address.c_str());
            
            // Watchdog entfernen
            esp_task_wdt_delete(NULL);
            
            // ════════════════════════════════════════════════════════════════
            // SCHRITT 1: Secure Bonding (OHNE Button-Press!)
            // ════════════════════════════════════════════════════════════════
            
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
            ESP_LOGI(TAG, "║   STEP 1: SECURE BONDING          ║");
            ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "→ Establishing bonded connection...");
            ESP_LOGI(TAG, "  (No button press needed - already encrypted)");
            ESP_LOGI(TAG, "");
            
            // Get device info from discovered list
            uint8_t addressType = BLE_ADDR_RANDOM;  // Default für Shelly
            
            std::vector<ShellyBLEDevice> discovered = p->bleManager->getDiscoveredDevices();
            for (const auto& dev : discovered) {
                if (dev.address.equalsIgnoreCase(p->address)) {
                    addressType = dev.addressType;
                    break;
                }
            }
            
            // NimBLE Security Setup
            NimBLEDevice::setSecurityAuth(true, false, true);  // Bonding, No MITM, SC
            NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);  // Just Works
            
            // Create client
            NimBLEClient* pClient = NimBLEDevice::createClient();
            if (!pClient) {
                ESP_LOGE(TAG, "✗ Failed to create client");
                
                const char* error = "{\"type\":\"error\",\"message\":\"Failed to create BLE client\"}";
                if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    httpd_ws_frame_t frame = {
                        .type = HTTPD_WS_TYPE_TEXT,
                        .payload = (uint8_t*)error,
                        .len = strlen(error)
                    };
                    httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                    xSemaphoreGive(p->handler->client_mutex);
                }
                
                delete p;
                vTaskDelete(NULL);
                return;
            }
            
            pClient->setConnectTimeout(15000);
            
            // Connect
            NimBLEAddress bleAddr(p->address.c_str(), addressType);
            bool connected = pClient->connect(bleAddr, false);
            
            if (!connected) {
                // Try alternative address type
                uint8_t altType = (addressType == BLE_ADDR_PUBLIC) ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
                ESP_LOGI(TAG, "→ Trying alternative address type...");
                
                bleAddr = NimBLEAddress(p->address.c_str(), altType);
                connected = pClient->connect(bleAddr, false);
            }
            
            if (!connected) {
                ESP_LOGE(TAG, "✗ Connection failed");
                
                const char* error = "{\"type\":\"error\",\"message\":\"Connection failed. Device not reachable.\"}";
                if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    httpd_ws_frame_t frame = {
                        .type = HTTPD_WS_TYPE_TEXT,
                        .payload = (uint8_t*)error,
                        .len = strlen(error)
                    };
                    httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                    xSemaphoreGive(p->handler->client_mutex);
                }
                
                NimBLEDevice::deleteClient(pClient);
                delete p;
                vTaskDelete(NULL);
                return;
            }
            
            ESP_LOGI(TAG, "✓ Connected");
            ESP_LOGI(TAG, "");
            
            // Request secure connection (bonding)
            ESP_LOGI(TAG, "→ Requesting secure connection...");
            bool secureResult = pClient->secureConnection();
            
            if (!secureResult) {
                ESP_LOGE(TAG, "✗ Secure connection failed");
                
                pClient->disconnect();
                NimBLEDevice::deleteClient(pClient);
                
                const char* error = "{\"type\":\"error\",\"message\":\"Bonding failed\"}";
                if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    httpd_ws_frame_t frame = {
                        .type = HTTPD_WS_TYPE_TEXT,
                        .payload = (uint8_t*)error,
                        .len = strlen(error)
                    };
                    httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                    xSemaphoreGive(p->handler->client_mutex);
                }
                
                delete p;
                vTaskDelete(NULL);
                return;
            }
            
            ESP_LOGI(TAG, "✓ Bonding complete");
            ESP_LOGI(TAG, "");
            
            // Disconnect (nicht mehr benötigt)
            pClient->disconnect();
            
            uint8_t retries = 0;
            while (pClient->isConnected() && retries < 20) {
                vTaskDelay(pdMS_TO_TICKS(100));
                retries++;
            }
            
            NimBLEDevice::deleteClient(pClient);
            
            // ════════════════════════════════════════════════════════════════
            // SCHRITT 2: Credentials speichern
            // ════════════════════════════════════════════════════════════════
            
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
            ESP_LOGI(TAG, "║   STEP 2: STORE CREDENTIALS       ║");
            ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
            ESP_LOGI(TAG, "");
            
            // Find device name from discovered list
            String deviceName = "Unknown";
            for (const auto& dev : discovered) {
                if (dev.address.equalsIgnoreCase(p->address)) {
                    deviceName = dev.name;
                    break;
                }
            }
            
            // Store in BLE Manager
            Preferences prefs;
            prefs.begin("ShellyBLE", false);
            prefs.putString("address", p->address);
            prefs.putString("name", deviceName);
            prefs.putString("bindkey", p->bindkey);
            prefs.putUInt("passkey", p->passkey);
            prefs.end();
            
            ESP_LOGI(TAG, "✓ Stored in NVS:");
            ESP_LOGI(TAG, "  Address: %s", p->address.c_str());
            ESP_LOGI(TAG, "  Name: %s", deviceName.c_str());
            ESP_LOGI(TAG, "  Passkey: %06u", p->passkey);
            ESP_LOGI(TAG, "  Bindkey: %s", p->bindkey.c_str());
            ESP_LOGI(TAG, "");
            
            // Update internal state (reload from NVS)
            p->bleManager->loadPairedDevice();
            p->bleManager->updateDeviceState(ShellyBLEManager::STATE_CONNECTED_ENCRYPTED);
            
            // ════════════════════════════════════════════════════════════════
            // SCHRITT 3: Continuous Scan starten
            // ════════════════════════════════════════════════════════════════
            
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
            ESP_LOGI(TAG, "║   STEP 3: START CONTINUOUS SCAN   ║");
            ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
            ESP_LOGI(TAG, "");
            
            vTaskDelay(pdMS_TO_TICKS(1000));
            
            p->bleManager->startContinuousScan();
            
            ESP_LOGI(TAG, "✓ Continuous scan started");
            ESP_LOGI(TAG, "");
            
            // ════════════════════════════════════════════════════════════════
            // SUCCESS MESSAGE
            // ════════════════════════════════════════════════════════════════
            
            char success_msg[1024];
            snprintf(success_msg, sizeof(success_msg),
                    "{\"type\":\"success\",\"message\":\"<strong>✅ Encrypted Device Paired!</strong><br><br>"
                    "Your device is now connected:<br>"
                    "✓ Secure bonded connection<br>"
                    "✓ Passkey: %06u<br>"
                    "✓ Bindkey: %s<br><br>"
                    "Broadcasts will be decrypted automatically.<br>"
                    "Continuous scan is now active.\"}",
                    p->passkey,
                    p->bindkey.c_str());
            
            if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                httpd_ws_frame_t frame = {
                    .type = HTTPD_WS_TYPE_TEXT,
                    .payload = (uint8_t*)success_msg,
                    .len = strlen(success_msg)
                };
                httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                xSemaphoreGive(p->handler->client_mutex);
            }
            
            ESP_LOGI(TAG, "✓ Pairing successful");
            
            // Modal schließen
            vTaskDelay(pdMS_TO_TICKS(2000));
            p->handler->sendModalClose(p->fd, "ble-encrypted-known-modal");
            
            // Status-Update senden
            vTaskDelay(pdMS_TO_TICKS(1000));
            
            char status_buf[768];
            snprintf(status_buf, sizeof(status_buf),
                    "{\"type\":\"ble_status\","
                    "\"paired\":true,"
                    "\"state\":\"connected_encrypted\","
                    "\"name\":\"%s\","
                    "\"address\":\"%s\","
                    "\"passkey\":\"%06u\","
                    "\"bindkey\":\"%s\","
                    "\"sensor_data\":{\"valid\":false}}",
                    deviceName.c_str(),
                    p->address.c_str(),
                    p->passkey,
                    p->bindkey.c_str());
            
            p->handler->broadcast_to_all_clients(status_buf);
            
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
            ESP_LOGI(TAG, "║  ✅ TASK COMPLETE                 ║");
            ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
            ESP_LOGI(TAG, "");
            
            delete p;
            vTaskDelete(NULL);

            UBaseType_t highWater = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "Task Stack (ble_enc_known)usage: %u bytes free", highWater * sizeof(StackType_t));
            
        }, "ble_enc_known", 4096, params, 5, NULL);
        
        ESP_LOGI(TAG, "✓ Already-Encrypted pairing task created");
    }
}




else if (strncmp(cmd, "{\"cmd\":\"ble_unpair\"", 19) == 0) {
    if (self->bleManager) {
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "BLE UNPAIRING");
        ESP_LOGI(TAG, "═══════════════════════════════════");
        
        if (self->bleManager->unpairDevice()) {
            // ✓ Callback für Contact Sensor Endpoint Removal
            if (self->remove_contact_sensor_callback) {
                ESP_LOGI(TAG, "→ Removing Contact Sensor endpoint...");
                self->remove_contact_sensor_callback();
            }
            
            const char* success = "{\"type\":\"info\",\"message\":\"Device unpaired\"}";
            httpd_ws_frame_t frame = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t*)success,
                .len = strlen(success)
            };
            httpd_ws_send_frame_async(req->handle, fd, &frame);
            
            ESP_LOGI(TAG, "✓ Device unpaired");
            ESP_LOGI(TAG, "✓ Continuous scan stopped");
        }
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
        
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "BLE PAIRING (Unencrypted)");
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "Address: %s", address.c_str());
        ESP_LOGI(TAG, "Bindkey: %s", bindkey.length() > 0 ? "[provided]" : "[empty]");
        
        if (self->bleManager->pairDevice(address, bindkey)) {
            const char* success = "{\"type\":\"info\",\"message\":\"Device paired successfully!\"}";
            httpd_ws_frame_t frame = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t*)success,
                .len = strlen(success)
            };
            httpd_ws_send_frame_async(req->handle, fd, &frame);
            
            ESP_LOGI(TAG, "✓ Pairing successful");
            
            // ✅ Continuous Scan starten
            ESP_LOGI(TAG, "→ Starting continuous scan for sensor data...");
            self->bleManager->startContinuousScan();
            
        } else {
            const char* error = "{\"type\":\"error\",\"message\":\"Failed to pair device\"}";
            httpd_ws_frame_t frame = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t*)error,
                .len = strlen(error)
            };
            httpd_ws_send_frame_async(req->handle, fd, &frame);
            
            ESP_LOGE(TAG, "✗ Pairing failed");
        }
    }
}

else if (strcmp(cmd, "ble_start_continuous_scan") == 0) {
    if (self->bleManager) {
        if (self->bleManager->isPaired()) {
            ESP_LOGI(TAG, "Starting continuous BLE scan (paired device exists)");
            self->bleManager->startContinuousScan();
            
            const char* success = "{\"type\":\"info\",\"message\":\"Continuous scanning started\"}";
            httpd_ws_frame_t frame = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t*)success,
                .len = strlen(success)
            };
            httpd_ws_send_frame_async(req->handle, fd, &frame);
        } else {
            ESP_LOGW(TAG, "Cannot start continuous scan - no device paired");
            
            const char* error = "{\"type\":\"error\",\"message\":\"No device paired\"}";
            httpd_ws_frame_t frame = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t*)error,
                .len = strlen(error)
            };
            httpd_ws_send_frame_async(req->handle, fd, &frame);
        }
    }
}

else if (strcmp(cmd, "ble_stop_scan") == 0) {
    if (self->bleManager) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
        ESP_LOGI(TAG, "║  USER: STOP CONTINUOUS SCAN       ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
        ESP_LOGI(TAG, "");
        
        // ✅ KRITISCH: stopScan(true) = manueller Stop!
        // Dies verhindert Auto-Restart und setzt NVS auf false
        self->bleManager->stopScan(true);
        
        const char* success = 
            "{\"type\":\"info\",\"message\":\"Continuous scanning stopped by user\"}";
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)success,
            .len = strlen(success)
        };
        httpd_ws_send_frame_async(req->handle, fd, &frame);
        
        ESP_LOGI(TAG, "✓ Continuous scan stopped (manual)");
        ESP_LOGI(TAG, "  NVS updated: continuous_scan = false");
        ESP_LOGI(TAG, "  Will NOT auto-restart");
        ESP_LOGI(TAG, "");
        
        // ✅ Status-Update nach kurzer Verzögerung
        vTaskDelay(pdMS_TO_TICKS(500));
        
        // Sende aktuellen BLE Status
        if (self->bleManager->isPaired()) {
            PairedShellyDevice device = self->bleManager->getPairedDevice();
            
            char status_buf[512];
            snprintf(status_buf, sizeof(status_buf),
                    "{\"type\":\"ble_status\","
                    "\"paired\":true,"
                    "\"state\":\"connected_encrypted\","
                    "\"name\":\"%s\","
                    "\"address\":\"%s\","
                    "\"continuous_scan_active\":false,"
                    "\"sensor_data\":{\"valid\":false}}",
                    device.name.c_str(),
                    device.address.c_str());
            
            self->broadcast_to_all_clients(status_buf);
        }
    } else {
        const char* error = "{\"type\":\"error\",\"message\":\"BLE Manager not available\"}";
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)error,
            .len = strlen(error)
        };
        httpd_ws_send_frame_async(req->handle, fd, &frame);
    }
}

    // ============================================================================
    // Contact Sensor Matter Toggle Commands
    // ============================================================================

    else if (strcmp(cmd, "contact_sensor_enable") == 0) {
        ESP_LOGI(TAG, "WebSocket: Enable Contact Sensor for Matter");
        
        extern void enableContactSensorMatter();
        enableContactSensorMatter();
        
        const char* success = "{\"type\":\"info\",\"message\":\"Contact Sensor enabled for Matter\"}";
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)success,
            .len = strlen(success)
        };
        httpd_ws_send_frame_async(req->handle, fd, &frame);
    }
    else if (strcmp(cmd, "contact_sensor_disable") == 0) {
        ESP_LOGI(TAG, "WebSocket: Disable Contact Sensor for Matter");
        
        extern void disableContactSensorMatter();
        disableContactSensorMatter();
        
        const char* success = "{\"type\":\"info\",\"message\":\"Contact Sensor disabled for Matter\"}";
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)success,
            .len = strlen(success)
        };
        httpd_ws_send_frame_async(req->handle, fd, &frame);
    }
    // ============================================================================
    // BLE Read Sensor Data (GATT)
    // ============================================================================

    else if (strcmp(cmd, "read_sensor_data") == 0) {
        if (self->bleManager) {
            if (!self->bleManager->isPaired()) {
                const char* error = "{\"type\":\"error\",\"message\":\"No device paired\"}";
                httpd_ws_frame_t frame = {
                    .type = HTTPD_WS_TYPE_TEXT,
                    .payload = (uint8_t*)error,
                    .len = strlen(error)
                };
                httpd_ws_send_frame_async(req->handle, fd, &frame);
                return ESP_OK;
            }
            
            ESP_LOGI(TAG, "═══════════════════════════════════");
            ESP_LOGI(TAG, "WebSocket: READ SENSOR DATA (GATT)");
            ESP_LOGI(TAG, "═══════════════════════════════════");
            
            PairedShellyDevice device = self->bleManager->getPairedDevice();
            ShellyBLESensorData data;
            
            // ✅ GATT Read in separatem Task (non-blocking)
            struct ReadTaskParams {
                ShellyBLEManager* bleManager;
                WebUIHandler* handler;
                int fd;
                String address;
            };
            
            ReadTaskParams* params = new ReadTaskParams{
                self->bleManager,
                self,
                fd,
                device.address
            };
            
            xTaskCreate([](void* pvParameters) {
                ReadTaskParams* p = (ReadTaskParams*)pvParameters;
                
                ESP_LOGI(TAG, "📖 Read Task started for %s", p->address.c_str());
                
                // Watchdog für diesen Task entfernen (kann lange dauern)
                esp_task_wdt_delete(NULL);
                
                ShellyBLESensorData data;
                bool success = p->bleManager->readSampleBTHomeData(p->address, data);
                
                if (success) {
                    // Erfolg - Sende Daten an WebUI
                    char json_buf[512];
                    snprintf(json_buf, sizeof(json_buf),
                            "{\"type\":\"sensor_data_result\","
                            "\"success\":true,"
                            "\"packet_id\":%d,"
                            "\"battery\":%d,"
                            "\"window_open\":%s,"
                            "\"illuminance\":%u,"
                            "\"rotation\":%d,"
                            "\"rssi\":%d,"
                            "\"valid\":true}",
                            data.packetId,
                            data.battery,
                            data.windowOpen ? "true" : "false",
                            data.illuminance,
                            data.rotation,
                            data.rssi);
                    
                    if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                        httpd_ws_frame_t frame = {
                            .type = HTTPD_WS_TYPE_TEXT,
                            .payload = (uint8_t*)json_buf,
                            .len = strlen(json_buf)
                        };
                        httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                        xSemaphoreGive(p->handler->client_mutex);
                    }
                    
                    ESP_LOGI(TAG, "✓ Sensor data sent to WebUI");
                    
                } else {
                    // Fehler
                    const char* error = "{\"type\":\"sensor_data_result\","
                                      "\"success\":false,"
                                      "\"error\":\"Failed to read sensor data\"}";
                    
                    if (xSemaphoreTake(p->handler->client_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                        httpd_ws_frame_t frame = {
                            .type = HTTPD_WS_TYPE_TEXT,
                            .payload = (uint8_t*)error,
                            .len = strlen(error)
                        };
                        httpd_ws_send_frame_async(p->handler->server, p->fd, &frame);
                        xSemaphoreGive(p->handler->client_mutex);
                    }
                    
                    ESP_LOGE(TAG, "✗ Failed to read sensor data");
                }
                
                delete p;
                vTaskDelete(NULL);

                UBaseType_t highWater = uxTaskGetStackHighWaterMark(NULL);
                ESP_LOGI(TAG, "Task Stack (ble_read) usage: %u bytes free", highWater * sizeof(StackType_t));
                
            }, "ble_read", 4096, params, 5, NULL);
            
            // Sofort Info an User senden
            const char* info = "{\"type\":\"info\",\"message\":\"Reading sensor data via GATT...\"}";
            httpd_ws_frame_t frame = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t*)info,
                .len = strlen(info)
            };
            httpd_ws_send_frame_async(req->handle, fd, &frame);
        }
    }
    else if (strcmp(cmd, "contact_sensor_status") == 0) {
        extern bool contact_sensor_matter_enabled;
        extern bool contact_sensor_endpoint_active;
        
        char status_buf[128];
        snprintf(status_buf, sizeof(status_buf),
                "{\"type\":\"contact_sensor_status\","
                "\"enabled\":%s,"
                "\"active\":%s}",
                contact_sensor_matter_enabled ? "true" : "false",
                contact_sensor_endpoint_active ? "true" : "false");
        
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)status_buf,
            .len = strlen(status_buf)
        };
        httpd_ws_send_frame_async(req->handle, fd, &frame);
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

  // ============================================================================
  // WebSocket Broadcast: BLE State Change
  // ============================================================================

  void WebUIHandler::broadcastBLEStateChange(ShellyBLEManager::DeviceState oldState, 
                                            ShellyBLEManager::DeviceState newState) {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
    ESP_LOGI(TAG, "║   BROADCASTING STATE CHANGE       ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
    
    const char* stateStr = "not_paired";
    const char* stateLabel = "Not Paired";
    
    if (newState == ShellyBLEManager::STATE_CONNECTED_UNENCRYPTED) {
        stateStr = "connected_unencrypted";
        stateLabel = "Connected (Unencrypted)";
    } else if (newState == ShellyBLEManager::STATE_CONNECTED_ENCRYPTED) {
        stateStr = "connected_encrypted";
        stateLabel = "Connected & Encrypted";
    }
    
    ESP_LOGI(TAG, "Old State: %d → New State: %d", (int)oldState, (int)newState);
    ESP_LOGI(TAG, "State String: %s", stateStr);
    ESP_LOGI(TAG, "Active clients: %d", active_clients.size());
    
    char msg[256];
    snprintf(msg, sizeof(msg),
            "{\"type\":\"ble_state_changed\","
            "\"state\":\"%s\","
            "\"label\":\"%s\"}",
            stateStr,
            stateLabel);
    
    ESP_LOGI(TAG, "Broadcasting message: %s", msg);
    broadcast_to_all_clients(msg);
    ESP_LOGI(TAG, "✓ Broadcast complete");
    ESP_LOGI(TAG, "");
  }

  // ============================================================================
  // WebSocket Broadcast: Sensor Data Update
  // ============================================================================

  void WebUIHandler::broadcastSensorDataUpdate(const String& address, const ShellyBLESensorData& data) {
      ESP_LOGI(TAG, "");
      ESP_LOGI(TAG, "╔═══════════════════════════════════╗");
      ESP_LOGI(TAG, "║   BROADCASTING SENSOR DATA        ║");
      ESP_LOGI(TAG, "╚═══════════════════════════════════╝");
      ESP_LOGI(TAG, "Address: %s", address.c_str());
      ESP_LOGI(TAG, "Battery: %d%% | Window: %s", 
              data.battery, 
              data.windowOpen ? "OPEN" : "CLOSED");
      ESP_LOGI(TAG, "Packet ID: %d", data.packetId);
      ESP_LOGI(TAG, "Active clients: %d", active_clients.size());
      
      char json_buf[512];
      snprintf(json_buf, sizeof(json_buf),
              "{\"type\":\"ble_sensor_update\","
              "\"address\":\"%s\","
              "\"window_open\":%s,"
              "\"battery\":%d,"
              "\"illuminance\":%u,"
              "\"rotation\":%d,"
              "\"rssi\":%d,"
              "\"packet_id\":%d,"
              "\"has_button_event\":%s,"
              "\"button_event\":%d,"
              "\"last_update\":%lu}",
              address.c_str(),
              data.windowOpen ? "true" : "false",
              data.battery,
              data.illuminance,
              data.rotation,
              data.rssi,
              data.packetId,
              data.hasButtonEvent ? "true" : "false",
              (int)data.buttonEvent,
              (unsigned long)data.lastUpdate);
      
      ESP_LOGI(TAG, "Broadcasting message: %s", json_buf);
      broadcast_to_all_clients(json_buf);
      ESP_LOGI(TAG, "✓ Broadcast complete");
      ESP_LOGI(TAG, "");
  }

  // ============================================================================
  // Helper: Send Modal Close Command
  // ============================================================================

  void WebUIHandler::sendModalClose(int fd, const char* modal_id) {
      char msg[128];
      snprintf(msg, sizeof(msg),
              "{\"type\":\"modal_close\",\"modal_id\":\"%s\"}",
              modal_id);
      
      httpd_ws_frame_t frame = {
          .type = HTTPD_WS_TYPE_TEXT,
          .payload = (uint8_t*)msg,
          .len = strlen(msg)
      };
      
      httpd_ws_send_frame_async(server, fd, &frame);
      ESP_LOGI(TAG, "→ Sent modal close command: %s", modal_id);
  }
