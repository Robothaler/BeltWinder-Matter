#include <Arduino.h>
#include <WiFi.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_chip_info.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_flash.h>
#include <esp_mac.h>
#include <string.h>
#include <app/server/Server.h>
#include <Matter.h>
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
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
      background: #121212;
      color: #e0e0e0;
      line-height: 1.6;
      overflow-x: hidden;
    }
    .container { max-width: 800px; margin: 0 auto; padding: 20px; }
    .header {
      text-align: center;
      margin-bottom: 30px;
      padding: 20px;
      background: linear-gradient(135deg, #2196F3 0%, #1976D2 100%);
      border-radius: 15px;
    }
    .header h1 { color: white; font-size: 2em; }
    .nav {
      display: flex;
      justify-content: space-around;
      background: #1e1e1e;
      padding: 15px;
      border-radius: 10px;
      margin-bottom: 20px;
      overflow-x: auto;
      flex-wrap: wrap;
      gap: 5px;
    }
    .nav button {
      background: none;
      border: none;
      color: #aaa;
      font-size: 0.9em;
      padding: 8px 12px;
      cursor: pointer;
      transition: all .2s;
      white-space: nowrap;
      border-radius: 5px;
    }
    .nav button.active { color: #2196F3; background: #2a2a2a; }
    .card {
      background: #1e1e1e;
      border-radius: 15px;
      padding: 25px;
      margin-bottom: 20px;
    }
    .card h2 { font-size: 1.3em; margin-bottom: 15px; color: #2196F3; }
    .status-grid {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 15px;
      margin-bottom: 20px;
    }
    .status-item {
      background: #2a2a2a;
      padding: 12px;
      border-radius: 10px;
      text-align: center;
    }
    .status-label { font-size: 0.8em; color: #888; text-transform: uppercase; }
    .status-value { font-size: 1.4em; font-weight: bold; margin-top: 5px; }
    .status-value.commissioned { color: #4CAF50; }
    .status-value.not-commissioned { color: #FF9800; }
    .btn-grid {
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      gap: 15px;
      margin-top: 20px;
    }
    .btn {
      background: linear-gradient(135deg, #4CAF50 0%, #388E3C 100%);
      color: white;
      border: none;
      padding: 15px 10px;
      font-size: 1.1em;
      border-radius: 10px;
      cursor: pointer;
      transition: all .2s;
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 8px;
    }
    .btn:hover { transform: translateY(-2px); box-shadow: 0 4px 8px rgba(0,0,0,0.3); }
    .btn:disabled { background: #555; cursor: not-allowed; opacity: 0.5; transform: none; }
    .btn.stop, .btn.danger { background: linear-gradient(135deg, #f44336 0%, #d32f2f 100%); }
    .btn.secondary { background: linear-gradient(135deg, #607D8B 0%, #455A64 100%); }
    .btn.primary { background: linear-gradient(135deg, #2196F3 0%, #1976D2 100%); }
    .direction-btn {
      padding: 15px;
      font-size: 1em;
      text-align: center;
      border-radius: 10px;
      cursor: pointer;
      transition: all .3s;
      border: 2px solid transparent;
      background: #2a2a2a;
    }
    .direction-btn.active { border-color: #4CAF50; }
    .direction-btn:hover { background: #333; }
    .info-table {
      width: 100%;
      border-collapse: collapse;
      font-size: 0.95em;
    }
    .info-table tr { border-bottom: 1px solid #333; }
    .info-table td { padding: 12px 8px; }
    .info-table td:first-child { font-weight: bold; color: #aaa; width: 45%; }
    .info-table td:last-child { text-align: right; color: #e0e0e0; }
    .hidden { display: none !important; }
    .modal {
      position: fixed;
      top: 0;
      left: 0;
      width: 100%;
      height: 100%;
      background: rgba(0,0,0,0.9);
      align-items: center;
      justify-content: center;
      z-index: 1000;
      padding: 20px;
      overflow-y: auto;
    }
    .modal:not(.hidden) { display: flex; }
    .modal-box {
      background: #1e1e1e;
      padding: 30px;
      border-radius: 15px;
      max-width: 500px;
      width: 100%;
      max-height: 90vh;
      overflow-y: auto;
    }
    .modal-box h3 { margin-bottom: 20px; color: #2196F3; font-size: 1.5em; }
    .modal-buttons {
      display: flex;
      gap: 15px;
      justify-content: center;
      margin-top: 20px;
      flex-wrap: wrap;
    }
    .modal-btn {
      padding: 12px 24px;
      border: none;
      border-radius: 8px;
      cursor: pointer;
      font-size: 1em;
      transition: all .2s;
    }
    .modal-btn-primary { background: #2196F3; color: white; }
    .modal-btn-primary:hover { background: #1976D2; }
    .modal-btn-secondary { background: #666; color: white; }
    .modal-btn-secondary:hover { background: #555; }
    .modal-btn-danger { background: #f44336; color: white; }
    .modal-btn-danger:hover { background: #d32f2f; }
    .qr-container {
      text-align: center;
      margin: 20px 0;
      padding: 20px;
      background: #2a2a2a;
      border-radius: 10px;
    }
    .qr-code {
      max-width: 300px;
      width: 100%;
      height: auto;
      margin: 0 auto;
      display: block;
      background: white;
      padding: 10px;
      border-radius: 10px;
    }
    .pairing-code {
      font-family: monospace;
      font-size: 1.5em;
      color: #4CAF50;
      margin: 15px 0;
      padding: 15px;
      background: #2a2a2a;
      border-radius: 8px;
      letter-spacing: 2px;
    }
    .alert {
      padding: 15px;
      border-radius: 8px;
      margin: 15px 0;
      background: #2a2a2a;
      border-left: 4px solid #2196F3;
    }
    .alert.warning { border-left-color: #FF9800; }
    .alert.error { border-left-color: #f44336; }
    .alert.success { border-left-color: #4CAF50; }
    .badge {
      display: inline-block;
      padding: 4px 10px;
      border-radius: 5px;
      font-size: 0.75em;
      font-weight: bold;
      margin-left: 8px;
    }
    .badge.commissioned { background: #4CAF50; color: white; }
    .badge.not-commissioned { background: #FF9800; color: white; }

    #qrcode { 
      margin: 0 auto; 
      padding: 20px; 
      background: white; 
      border-radius: 10px; 
      display: inline-block; 
    }
    #qrcode img, 
    #qrcode canvas { 
      display: block; 
      margin: 0 auto; 
    }      
  </style>
  <script src="https://davidshimjs.github.io/qrcodejs/qrcode.min.js"></script>
</head>
<body>
  <div class="container">
    <div class="header"><h1>🎚️ BeltWinder Matter</h1></div>

    <div class="nav">
      <button onclick="show('overview')" class="active" id="nav-overview">📊 Overview</button>
      <button onclick="show('matter')" id="nav-matter">🔗 Matter</button>
      <button onclick="show('system')" id="nav-system">💻 System</button>
      <button onclick="show('settings')" id="nav-settings">⚙️ Settings</button>
    </div>

    <!-- Overview Tab -->
    <div id="overview">
      <div class="card">
        <h2>Status</h2>
        <div class="status-grid">
          <div class="status-item"><div class="status-label">Position</div><div class="status-value" id="pos">0%</div></div>
          <div class="status-item"><div class="status-label">Calibrated</div><div class="status-value" id="calib">No</div></div>
          <div class="status-item"><div class="status-label">Direction</div><div class="status-value" id="inv">Normal</div></div>
          <div class="status-item"><div class="status-label">Matter Status</div><div class="status-value" id="matter-status">Checking...</div></div>
        </div>
        <div class="btn-grid">
          <button class="btn" onclick="send('up')">⬆ UP</button>
          <button class="btn" onclick="send('down')">⬇ DOWN</button>
          <button class="btn stop" onclick="send('stop')">⏹ STOP</button>
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
        <p style="color:#888;margin-bottom:15px">Calibrate the shutter to learn its full travel range</p>
        <button class="btn primary" onclick="send('calibrate')" style="width:100%">🎯 START CALIBRATION</button>
      </div>

      <div class="card">
        <h2>Direction Control</h2>
        <p style="color:#888;margin-bottom:15px">Change motor direction if UP/DOWN is reversed</p>
        <div style="display:grid;grid-template-columns:1fr 1fr;gap:15px">
          <div class="direction-btn" id="dir-normal" onclick="setDirection(false)">
            <div style="font-size:1.2em;margin-bottom:5px">✅</div>
            <div style="font-weight:bold">Normal</div>
            <div style="font-size:0.8em;color:#888">Standard direction</div>
          </div>
          <div class="direction-btn" id="dir-inverted" onclick="setDirection(true)">
            <div style="font-size:1.2em;margin-bottom:5px">🔄</div>
            <div style="font-weight:bold">Inverted</div>
            <div style="font-size:0.8em;color:#888">Reversed direction</div>
          </div>
        </div>
      </div>

      <div class="card">
        <h2>Factory Reset</h2>
        <p style="color:#888;margin-bottom:15px">⚠️ This will remove the device from Matter and erase all settings</p>
        <button class="btn danger" onclick="showConfirm()" style="width:100%">🗑️ FACTORY RESET</button>
      </div>
    </div>
  </div>

  <!-- Factory Reset Confirmation Modal -->
  <div id="confirm-reset" class="modal hidden">
    <div class="modal-box">
      <h3>⚠️ Confirm Factory Reset</h3>
      <p style="margin:20px 0;font-size:1.1em">This will:</p>
      <ul style="margin:15px 0 15px 25px;color:#888">
        <li>Remove device from Matter</li>
        <li>Erase all calibration data</li>
        <li>Reset all settings to defaults</li>
      </ul>
      <p style="margin:20px 0;color:#f44336;font-weight:bold">This action cannot be undone!</p>
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
      <div class="alert success">Device is ready to be paired with Matter!</div>
      
      <div class="qr-container">
        <img class="qr-code" id="matter-qr-img" src="" alt="QR Code">
        <div style="margin-top:15px;color:#888;font-size:0.9em">Scan with your Matter controller</div>
        <a id="qr-web-link" href="" target="_blank" 
          style="display:inline-block;margin-top:10px;color:#2196F3;text-decoration:none">
          🌐 Open in CHIP QR Generator
        </a>
      </div>
      <div style="margin-top:20px">
        <div style="color:#888;margin-bottom:10px;font-weight:bold">Manual Pairing Code:</div>
        <div class="pairing-code" id="matter-pairing-code">Loading...</div>
      </div>
      
      <div class="modal-buttons">
        <button class="modal-btn modal-btn-secondary" onclick="closeMatterQR()">Close</button>
      </div>
    </div>
  </div>

  <script>
    let ws;
    let reconnectInterval;
    let matterCommissioned = false;

    function connectWebSocket() {
      ws = new WebSocket('ws://' + location.host + '/ws');

      ws.onopen = () => {
        console.log('WebSocket connected');
        clearInterval(reconnectInterval);
        ws.send('status');
        ws.send('matter_status');
        
        setInterval(() => {
          if (ws.readyState === WebSocket.OPEN) {
            ws.send('status');
          }
        }, 2000);
      };

      ws.onclose = () => {
        console.log('WebSocket disconnected. Reconnecting...');
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
          console.error('Failed to parse JSON:', err, e.data);
          return;
        }
        
        if (d.type === 'status') {
          document.getElementById('pos').innerText = d.pos + '%';
          document.getElementById('calib').innerText = d.cal ? 'Yes' : 'No';
          document.getElementById('inv').innerText = d.inv ? 'Inverted' : 'Normal';
          updateDirectionButtons(d.inv);
        } 
        else if (d.type === 'matter_status') {
          console.log('Matter status received:', d);
          
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
      };
    }

    connectWebSocket();

    function show(id) {
      document.querySelectorAll('#overview, #matter, #system, #settings').forEach(e => e.classList.add('hidden'));
      document.getElementById(id).classList.remove('hidden');
      
      document.querySelectorAll('.nav button').forEach(b => b.classList.remove('active'));
      document.getElementById('nav-' + id).classList.add('active');
      
      if (id === 'system' && ws.readyState === WebSocket.OPEN) {
        ws.send('info');
      }
      if (id === 'matter' && ws.readyState === WebSocket.OPEN) {
        console.log('Matter tab opened, requesting status...');
        ws.send('matter_status');
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
      console.log('showMatterQR called:', {qrUrl, qrImageUrl, pairingCode});
      
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
      console.log('updateMatterInfo called with:', data);
      
      if (!data) {
        console.error('updateMatterInfo: data is undefined!');
        return;
      }
      
      let statusHtml = '<div class="alert ' + (data.commissioned ? 'success' : 'warning') + '">';
      
      if (data.commissioned) {
        statusHtml += '<strong>✓ Device is paired with Matter</strong>';
        statusHtml += '<p style="margin-top:10px;color:#888">Your shutter is connected and can be controlled by your Matter ecosystem.</p>';
        statusHtml += '<p style="margin-top:5px;color:#888">Active Fabrics: ' + (data.fabrics || 0) + '</p>';
      } else {
        statusHtml += '<strong>! Device not yet paired</strong>';
        statusHtml += '<p style="margin-top:10px;color:#888">Scan the QR code below or use the manual pairing code to add this device to your Matter controller.</p>';
      }
      statusHtml += '</div>';
      
      let detailEl = document.getElementById('matter-status-detail');
      if (detailEl) {
        detailEl.innerHTML = statusHtml;
      } else {
        console.error('Element #matter-status-detail not found!');
      }
      
      let pairingHtml = '';
      
      console.log('Checking QR availability:', {
        commissioned: data.commissioned,
        qr_image: data.qr_image,
        qr_image_length: data.qr_image ? data.qr_image.length : 0
      });
      
      if (!data.commissioned) {
        if (data.qr_image && data.qr_image.length > 0) {
          console.log('QR Code available, creating button');
          pairingHtml = '<div style="margin-top:20px">';
          pairingHtml += '<button class="btn primary" onclick="showMatterQR(\'' + 
                        (data.qr_url || '') + '\',\'' + 
                        data.qr_image + '\',\'' + 
                        (data.pairing_code || '') + 
                        '\')" style="width:100%">📱 Show QR Code & Pairing Info</button>';
          pairingHtml += '</div>';
        } else {
          console.warn('QR Code NOT available!');
          pairingHtml = '<div class="alert warning" style="margin-top:20px">';
          pairingHtml += '<strong>⚠️ QR Code not available</strong><br>';
          pairingHtml += '<p style="margin-top:10px;color:#888">Pairing Code: ' + (data.pairing_code || 'N/A') + '</p>';
          pairingHtml += '<p style="margin-top:5px;color:#888">Please check the serial console for pairing information.</p>';
          pairingHtml += '</div>';
        }
      } else {
        pairingHtml = '<div class="alert success" style="margin-top:20px">';
        pairingHtml += '<strong>✓ Device is successfully paired</strong><br>';
        pairingHtml += '<p style="margin-top:10px;color:#888">To remove this device, use Factory Reset in Settings.</p>';
        pairingHtml += '</div>';
      }
      
      let pairingEl = document.getElementById('matter-pairing-info');
      if (pairingEl) {
        pairingEl.innerHTML = pairingHtml;
      } else {
        console.error('Element #matter-pairing-info not found!');
      }
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
  </script>
</body>
</html>
)rawliteral";

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
    cfg.max_uri_handlers = 4;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;

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
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }
}

void WebUIHandler::register_client(int fd) {
    if (xSemaphoreTake(client_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        active_clients.push_back(fd);
        ESP_LOGI(TAG, "Client connected: fd=%d (total: %d)", fd, active_clients.size());
        xSemaphoreGive(client_mutex);
    }
}

void WebUIHandler::unregister_client(int fd) {
    if (xSemaphoreTake(client_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        active_clients.erase(
            std::remove(active_clients.begin(), active_clients.end(), fd),
            active_clients.end()
        );
        ESP_LOGI(TAG, "Client disconnected: fd=%d (remaining: %d)", fd, active_clients.size());
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
        
        std::vector<int> clients_copy = active_clients;
        xSemaphoreGive(client_mutex);
        
        for (int fd : clients_copy) {
            esp_err_t ret = httpd_ws_send_frame_async(server, fd, &frame);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to send to fd=%d", fd);
                unregister_client(fd);
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
        self->register_client(fd);
        return ESP_OK;
    }

    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.type = HTTPD_WS_TYPE_TEXT;
    
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) return ret;
    
    if (frame.len == 0 || frame.len > 512) return ESP_ERR_INVALID_SIZE;
    
    uint8_t* buf = (uint8_t*)malloc(frame.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    
    frame.payload = buf;
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret != ESP_OK) {
        free(buf);
        return ret;
    }
    
    buf[frame.len] = '\0';
    char* cmd = (char*)buf;
    
    WebUIHandler* self = (WebUIHandler*)req->user_ctx;
    int fd = httpd_req_to_sockfd(req);
    
    ESP_LOGI(TAG, "Received command from fd=%d: '%s'", fd, cmd);
    
    // ========================================================================
    // Command Handling
    // ========================================================================
    
    // Shutter Commands
    if (strcmp(cmd, "up") == 0) {
        shutter_driver_go_to_lift_percent(self->handle, 0);
    } 
    else if (strcmp(cmd, "down") == 0) {
        shutter_driver_go_to_lift_percent(self->handle, 100);
    } 
    else if (strcmp(cmd, "stop") == 0) {
        shutter_driver_stop_motion(self->handle);
    } 
    else if (strcmp(cmd, "calibrate") == 0) {
        shutter_driver_start_calibration(self->handle);
    } 
    else if (strcmp(cmd, "invert_on") == 0) {
        shutter_driver_set_direction(self->handle, true);
    }
        else if (strcmp(cmd, "invert_off") == 0) {
        shutter_driver_set_direction(self->handle, false);
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
    else if (strcmp(cmd, "ble_scan") == 0) {
        if (self->bleManager) {
            self->bleManager->startScan(10);
        }
    }
    else if (strcmp(cmd, "ble_status") == 0) {
        if (self->bleManager) {
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
            
            // Paired Devices
            std::vector<PairedShellyDevice> paired = self->bleManager->getPairedDevices();
            
            String json = "{\"type\":\"ble_paired\",\"devices\":[";
            for (size_t i = 0; i < paired.size(); i++) {
                if (i > 0) json += ",";
                json += "{\"name\":\"" + paired[i].name + "\",";
                json += "\"address\":\"" + paired[i].address + "\",";
                json += "\"state\":\"" + String(paired[i].windowOpen ? "open" : "closed") + "\"}";
            }
            json += "]}";
            
            frame.payload = (uint8_t*)json.c_str();
            frame.len = json.length();
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
            
            ESP_LOGI(TAG, "Pairing: %s", address.c_str());
            
            if (self->bleManager->pairDevice(address, bindkey)) {
                const char* success = "{\"type\":\"info\",\"message\":\"Device paired\"}";
                httpd_ws_frame_t frame = {
                    .type = HTTPD_WS_TYPE_TEXT,
                    .payload = (uint8_t*)success,
                    .len = strlen(success)
                };
                httpd_ws_send_frame_async(req->handle, fd, &frame);
            }
        }
    }
    else if (strncmp(cmd, "{\"cmd\":\"ble_unpair\"", 19) == 0) {
        if (self->bleManager) {
            String json = String(cmd);
            
            int addrStart = json.indexOf("\"address\":\"") + 11;
            int addrEnd = json.indexOf("\"", addrStart);
            String address = json.substring(addrStart, addrEnd);
            
            ESP_LOGI(TAG, "Unpairing: %s", address.c_str());
            self->bleManager->unpairDevice(address);
        }
    }
    else {
        ESP_LOGW(TAG, "Unknown command: '%s'", cmd);
    }
    
    free(buf);
    return ESP_OK;
}

