#include "OV7670.h"
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebServer.h>
#include "BMP.h"
 
const int SIOD = 21; //SDA
const int SIOC = 22; //SCL
const int VSYNC = 34;
const int HREF = 35;
const int XCLK = 32;
const int PCLK = 33;
const int D0 = 27;
const int D1 = 17;
const int D2 = 16;
const int D3 = 15;
const int D4 = 14;
const int D5 = 13;
const int D6 = 12;
const int D7 = 4;

#define ssid1        "IT-AP13"
#define password1    "greentea80342958"

OV7670 *camera;
WiFiMulti wifiMulti;
WebServer server(80);  // 創建一個HTTP服務器，監聽80端口

unsigned char bmpHeader[BMP::headerSize];
uint8_t *imageBuffer = NULL;
size_t imageSize = 0;

// 流客戶端狀態
#define MAX_CLIENTS 5
WiFiClient streamClients[MAX_CLIENTS];
bool clientConnected[MAX_CLIENTS] = {false};

// 幀率控制
unsigned long frameInterval = 100; // 默認100毫秒 (10 FPS)

// WiFi連接狀態監控
bool lastWiFiStatus = false;
unsigned long lastWiFiCheckTime = 0;
const unsigned long wifiCheckInterval = 5000; // 每5秒檢查一次WiFi狀態

// 相機初始化重試
const int maxCameraInitRetries = 5;

// 相機設置
int currentExposureLevel = 0; // 默認曝光級別 (範圍: -2 到 +2)
int currentBrightness = 0;    // 默認亮度級別 (範圍: -2 到 +2)
bool isNightModeEnabled = false; // 默認夜間模式關閉

void setup() 
{
  Serial.begin(115200);
  Serial.println("Starting ESP32 Camera Server...");
  
  // 先連接WiFi
  connectToWiFi();
  
  // 初始化相機（帶重試機制）
  bool cameraInitialized = false;
  int retryCount = 0;
  
  while (!cameraInitialized && retryCount < maxCameraInitRetries) {
    Serial.printf("Initializing camera, attempt %d/%d\n", retryCount + 1, maxCameraInitRetries);
    
    try {
      // 手動設置XCLK引腳
      pinMode(XCLK, OUTPUT);
      digitalWrite(XCLK, LOW);
      
      // 使用標準方式初始化相機，不指定XCLK頻率參數
      camera = new OV7670(OV7670::Mode::QQVGA_RGB565, SIOD, SIOC, VSYNC, HREF, XCLK, PCLK, D0, D1, D2, D3, D4, D5, D6, D7);
      
      cameraInitialized = true;
      Serial.println("Camera initialized successfully");
    } catch (const std::exception& e) {
      Serial.print("Camera initialization failed with exception: ");
      Serial.println(e.what());
      // 如果相機已經被創建，刪除它
      if (camera != nullptr) {
        delete camera;
        camera = nullptr;
      }
      retryCount++;
      delay(1000); // 等待1秒後重試
    } catch (...) {
      Serial.println("Camera initialization failed with unknown exception, retrying...");
      // 如果相機已經被創建，刪除它
      if (camera != nullptr) {
        delete camera;
        camera = nullptr;
      }
      retryCount++;
      delay(1000); // 等待1秒後重試
    }
  }
  
  if (!cameraInitialized) {
    Serial.println("Failed to initialize camera after multiple attempts. Restarting...");
    ESP.restart();
    return;
  }
  
  // 繼續初始化BMP和緩衝區
  BMP::construct16BitHeader(bmpHeader, camera->xres, camera->yres);
  
  // 分配圖像緩衝區
  imageSize = BMP::headerSize + camera->xres * camera->yres * 2;
  imageBuffer = (uint8_t*)malloc(imageSize);
  if (!imageBuffer) {
    Serial.println("Failed to allocate memory for image buffer");
    return;
  }
  
  // 複製BMP頭部到緩衝區
  memcpy(imageBuffer, bmpHeader, BMP::headerSize);
  
  // 設置HTTP服務器路由
  server.on("/", HTTP_GET, handleRoot);
  server.on("/capture", HTTP_GET, handleCapture);
  server.on("/stream", HTTP_GET, handleStream);
  server.on("/setInterval", HTTP_GET, handleSetInterval);
  server.on("/check", HTTP_GET, handleConnectionCheck);
  
  // 添加相機曝光設置路由
  server.on("/setExposure", HTTP_GET, handleSetExposure);
  server.on("/setBrightness", HTTP_GET, handleSetBrightness);
  server.on("/setNightMode", HTTP_GET, handleSetNightMode);
  
  server.onNotFound(handleNotFound);
  
  // 啟動HTTP服務器
  server.begin();
  Serial.println("HTTP server started");
  
  lastWiFiStatus = WiFi.status() == WL_CONNECTED;
  
  // 設置默認的曝光級別
  camera->setExposureLevel(currentExposureLevel);
}

void loop() {
  // 檢查WiFi連接狀態
  checkWiFiConnection();
  
  // 如果WiFi已連接，處理HTTP請求和MJPEG流
  if (WiFi.status() == WL_CONNECTED) {
    server.handleClient();
    streamMJPEG();
  }
  
  delay(10);
}

// 連接到WiFi
void connectToWiFi() {
  wifiMulti.addAP(ssid1, password1);
  Serial.println("Connecting Wifi...");
  
  // 嘗試連接WiFi，最多等待20秒
  unsigned long startAttemptTime = millis();
  while (wifiMulti.run() != WL_CONNECTED && millis() - startAttemptTime < 20000) {
    Serial.print(".");
    delay(500);
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("");
    Serial.println("WiFi connection failed");
  }
}

// 檢查WiFi連接狀態，如果斷開則嘗試重新連接
void checkWiFiConnection() {
  unsigned long currentMillis = millis();
  
  // 每隔wifiCheckInterval檢查一次WiFi狀態
  if (currentMillis - lastWiFiCheckTime >= wifiCheckInterval) {
    lastWiFiCheckTime = currentMillis;
    
    bool currentWiFiStatus = WiFi.status() == WL_CONNECTED;
    
    // 如果WiFi狀態發生變化
    if (currentWiFiStatus != lastWiFiStatus) {
      if (currentWiFiStatus) {
        // WiFi剛剛連接上
        Serial.println("WiFi reconnected");
        Serial.println("IP address: ");
        Serial.println(WiFi.localIP());
        
        // 重新啟動HTTP服務器
        server.begin();
        Serial.println("HTTP server restarted");
      } else {
        // WiFi剛剛斷開
        Serial.println("WiFi disconnected, attempting to reconnect...");
        wifiMulti.run(); // 嘗試重新連接
      }
      
      lastWiFiStatus = currentWiFiStatus;
    } else if (!currentWiFiStatus) {
      // 如果WiFi仍然斷開，繼續嘗試重新連接
      Serial.println("Still disconnected, attempting to reconnect WiFi...");
      wifiMulti.run();
    }
  }
}

// 處理連接檢查請求 - 用於客戶端檢測連接狀態
void handleConnectionCheck() {
  server.send(200, "text/plain; charset=utf-8", "connected");
}

// 處理設置曝光級別的請求
void handleSetExposure() {
  if (server.hasArg("level")) {
    String level = server.arg("level");
    currentExposureLevel = level.toInt();
    
    // 限制曝光級別範圍在 -2 到 +2 之間
    if (currentExposureLevel < -2) currentExposureLevel = -2;
    if (currentExposureLevel > 2) currentExposureLevel = 2;
    
    // 應用曝光設置
    camera->setExposureLevel(currentExposureLevel);
    
    server.send(200, "text/plain; charset=utf-8", "Exposure level set to " + String(currentExposureLevel));
    Serial.println("Exposure level set to " + String(currentExposureLevel));
  } else {
    server.send(400, "text/plain; charset=utf-8", "Missing level parameter");
  }
}

// 處理設置亮度的請求
void handleSetBrightness() {
  if (server.hasArg("level")) {
    String level = server.arg("level");
    currentBrightness = level.toInt();
    
    // 限制亮度範圍在 -2 到 +2 之間
    if (currentBrightness < -2) currentBrightness = -2;
    if (currentBrightness > 2) currentBrightness = 2;
    
    // 應用亮度設置
    camera->setBrightness(currentBrightness);
    
    server.send(200, "text/plain; charset=utf-8", "Brightness set to " + String(currentBrightness));
    Serial.println("Brightness set to " + String(currentBrightness));
  } else {
    server.send(400, "text/plain; charset=utf-8", "Missing level parameter");
  }
}

// 處理設置夜間模式的請求
void handleSetNightMode() {
  if (server.hasArg("enable")) {
    String enable = server.arg("enable");
    isNightModeEnabled = (enable == "1" || enable == "true" || enable == "on");
    
    // 應用夜間模式設置
    camera->setNightMode(isNightModeEnabled);
    
    server.send(200, "text/plain; charset=utf-8", "Night mode " + String(isNightModeEnabled ? "enabled" : "disabled"));
    Serial.println("Night mode " + String(isNightModeEnabled ? "enabled" : "disabled"));
  } else {
    server.send(400, "text/plain; charset=utf-8", "Missing enable parameter");
  }
}

// 處理設置幀率間隔的請求
void handleSetInterval() {
  if (server.hasArg("ms")) {
    String ms = server.arg("ms");
    frameInterval = ms.toInt();
    if (frameInterval < 50) frameInterval = 50; // 最小50ms (20 FPS)
    if (frameInterval > 2000) frameInterval = 2000; // 最大2000ms (0.5 FPS)
    server.send(200, "text/plain; charset=utf-8", "Interval set to " + String(frameInterval) + "ms");
  } else {
    server.send(400, "text/plain; charset=utf-8", "Missing ms parameter");
  }
}

// 處理根路徑請求，返回一個HTML頁面
void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset=\"UTF-8\">";
  html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">";
  html += "<title>ESP32 Camera Server</title>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 20px; text-align: center; background-color: #f5f5f5; }";
  html += "h1 { color: #333366; margin-bottom: 20px; }";
  html += ".camera-container { margin: 0 auto; max-width: 95%; position: relative; }";
  html += "#streamImage { border: 2px solid #cccccc; border-radius: 5px; }";
  html += "select, button { margin: 10px; padding: 8px 12px; border-radius: 4px; border: 1px solid #ccc; }";
  html += "input[type='range'] { width: 200px; vertical-align: middle; }";
  html += ".container { max-width: 800px; margin: 0 auto; padding: 20px; background-color: white; border-radius: 10px; box-shadow: 0 0 10px rgba(0,0,0,0.1); }";
  html += "#statusMessage { color: red; display: none; margin: 10px 0; font-weight: bold; }";
  html += ".control-group { margin: 15px 0; }";
  html += "button { background-color: #4CAF50; color: white; cursor: pointer; border: none; }";
  html += "button:hover { background-color: #45a049; }";
  html += "button.fullscreen { position: absolute; top: 10px; right: 10px; background: rgba(0,0,0,0.5); }";
  html += "a { display: inline-block; background-color: #f0f0f0; padding: 8px 16px; border-radius: 4px; text-decoration: none; color: #333; }";
  html += ".slider-container { display: flex; align-items: center; justify-content: center; margin: 10px 0; }";
  html += ".slider-container label { width: 120px; text-align: right; margin-right: 10px; }";
  html += ".slider-container input[type='range'] { flex: 1; max-width: 200px; }";
  html += ".slider-container span { width: 30px; text-align: left; margin-left: 10px; }";
  html += ".switch { position: relative; display: inline-block; width: 60px; height: 34px; }";
  html += ".switch input { opacity: 0; width: 0; height: 0; }";
  html += ".slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background-color: #ccc; transition: .4s; border-radius: 34px; }";
  html += ".slider:before { position: absolute; content: \"\"; height: 26px; width: 26px; left: 4px; bottom: 4px; background-color: white; transition: .4s; border-radius: 50%; }";
  html += "input:checked + .slider { background-color: #2196F3; }";
  html += "input:checked + .slider:before { transform: translateX(26px); }";
  html += ".tabs { display: flex; margin-bottom: 15px; }";
  html += ".tab { padding: 10px 20px; cursor: pointer; background-color: #f0f0f0; border-radius: 5px 5px 0 0; }";
  html += ".tab.active { background-color: white; border-bottom: 2px solid #4CAF50; }";
  html += ".tab-content { display: none; }";
  html += ".tab-content.active { display: block; }";
  html += "</style>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h1>ESP32 Camera Server</h1>";
  html += "<div id='statusMessage'>連接已中斷，正在嘗試重新連接...</div>";
  
  html += "<div class='camera-container'>";
  html += "<img id='streamImage' src='/stream' alt='Camera Stream' style='max-width:100%;'>";
  html += "<button class='fullscreen' onclick='toggleFullscreen()'>全屏</button>";
  html += "</div>";
  
  html += "<div class='tabs'>";
  html += "<div class='tab active' onclick='openTab(event, \"basicSettings\")'>基本設置</div>";
  html += "<div class='tab' onclick='openTab(event, \"exposureSettings\")'>曝光設置</div>";
  html += "</div>";
  
  // 基本設置標籤頁
  html += "<div id='basicSettings' class='tab-content active'>";
  
  html += "<div class='control-group'>";
  html += "<label for='imageSize'>圖像大小: </label>";
  html += "<span id='sizeValue'>100%</span><br>";
  html += "<input type='range' id='imageSize' min='10' max='200' value='100' style='margin:10px 0;'>";
  html += "<button onclick='applySize()'>應用大小</button>";
  html += "</div>";
  
  html += "<div class='control-group'>";
  html += "<label for='frameRate'>幀率設置: </label>";
  html += "<select id='frameRate' onchange='updateFrameRate()'>";
  html += "<option value='1000'>1 FPS (每秒1幀)</option>";
  html += "<option value='500'>2 FPS (每秒2幀)</option>";
  html += "<option value='200'>5 FPS (每秒5幀)</option>";
  html += "<option value='100' selected>10 FPS (每秒10幀)</option>";
  html += "<option value='50'>20 FPS (每秒20幀)</option>";
  html += "</select>";
  html += "</div>";
  
  html += "<div class='control-group'>";
  html += "<a href='/capture' target='_blank'>下載單張圖片</a>";
  html += "</div>";
  
  html += "</div>"; // 基本設置標籤頁結束
  
  // 曝光設置標籤頁
  html += "<div id='exposureSettings' class='tab-content'>";
  
  html += "<div class='control-group'>";
  html += "<div class='slider-container'>";
  html += "<label for='exposure'>曝光級別: </label>";
  html += "<input type='range' id='exposure' min='-2' max='2' value='" + String(currentExposureLevel) + "' oninput='updateExposureValue()'>";
  html += "<span id='exposureValue'>" + String(currentExposureLevel) + "</span>";
  html += "</div>";
  html += "<button onclick='applyExposure()'>應用曝光設置</button>";
  html += "</div>";
  
  html += "<div class='control-group'>";
  html += "<div class='slider-container'>";
  html += "<label for='brightness'>亮度級別: </label>";
  html += "<input type='range' id='brightness' min='-2' max='2' value='" + String(currentBrightness) + "' oninput='updateBrightnessValue()'>";
  html += "<span id='brightnessValue'>" + String(currentBrightness) + "</span>";
  html += "</div>";
  html += "<button onclick='applyBrightness()'>應用亮度設置</button>";
  html += "</div>";
  
  html += "<div class='control-group'>";
  html += "<label for='nightMode'>夜間模式: </label>";
  html += "<label class='switch'>";
  html += "<input type='checkbox' id='nightMode' " + String(isNightModeEnabled ? "checked" : "") + ">";
  html += "<span class='slider'></span>";
  html += "</label>";
  html += "<button onclick='applyNightMode()' style='margin-left: 10px;'>應用夜間模式</button>";
  html += "</div>";
  
  html += "</div>"; // 曝光設置標籤頁結束
  
  html += "</div>"; // 容器結束
  
  html += "<script>";
  
  // 標籤頁切換
  html += "function openTab(evt, tabName) {";
  html += "  var i, tabContent, tabLinks;";
  html += "  tabContent = document.getElementsByClassName('tab-content');";
  html += "  for (i = 0; i < tabContent.length; i++) {";
  html += "    tabContent[i].className = tabContent[i].className.replace(' active', '');";
  html += "  }";
  html += "  tabLinks = document.getElementsByClassName('tab');";
  html += "  for (i = 0; i < tabLinks.length; i++) {";
  html += "    tabLinks[i].className = tabLinks[i].className.replace(' active', '');";
  html += "  }";
  html += "  document.getElementById(tabName).className += ' active';";
  html += "  evt.currentTarget.className += ' active';";
  html += "}";
  
  // 更新曝光值顯示
  html += "function updateExposureValue() {";
  html += "  var exposure = document.getElementById('exposure').value;";
  html += "  document.getElementById('exposureValue').textContent = exposure;";
  html += "}";
  
  // 更新亮度值顯示
  html += "function updateBrightnessValue() {";
  html += "  var brightness = document.getElementById('brightness').value;";
  html += "  document.getElementById('brightnessValue').textContent = brightness;";
  html += "}";
  
  // 應用曝光設置
  html += "function applyExposure() {";
  html += "  var exposure = document.getElementById('exposure').value;";
  html += "  fetch('/setExposure?level=' + exposure)";
  html += "    .then(response => response.text())";
  html += "    .then(data => alert('曝光級別已設置為 ' + exposure))";
  html += "    .catch(error => console.error('Error:', error));";
  html += "}";
  
  // 應用亮度設置
  html += "function applyBrightness() {";
  html += "  var brightness = document.getElementById('brightness').value;";
  html += "  fetch('/setBrightness?level=' + brightness)";
  html += "    .then(response => response.text())";
  html += "    .then(data => alert('亮度已設置為 ' + brightness))";
  html += "    .catch(error => console.error('Error:', error));";
  html += "}";
  
  // 應用夜間模式
  html += "function applyNightMode() {";
  html += "  var nightMode = document.getElementById('nightMode').checked ? 1 : 0;";
  html += "  fetch('/setNightMode?enable=' + nightMode)";
  html += "    .then(response => response.text())";
  html += "    .then(data => alert('夜間模式已' + (nightMode ? '啟用' : '禁用')))";
  html += "    .catch(error => console.error('Error:', error));";
  html += "}";
  
  // 更新大小顯示值
  html += "var sizeSlider = document.getElementById('imageSize');";
  html += "var sizeValue = document.getElementById('sizeValue');";
  html += "sizeSlider.oninput = function() {";
  html += "  sizeValue.textContent = this.value + '%';";
  html += "};";
  
  // 應用大小按鈕
  html += "function applySize() {";
  html += "  var size = document.getElementById('imageSize').value;";
  html += "  var img = document.getElementById('streamImage');";
  html += "  img.style.width = size + '%';";
  html += "  localStorage.setItem('cameraSize', size);";
  html += "  alert('圖像大小已設置為 ' + size + '%');";
  html += "}";
  
  // 幀率更新
  html += "function updateFrameRate() {";
  html += "  var interval = document.getElementById('frameRate').value;";
  html += "  fetch('/setInterval?ms=' + interval)";
  html += "    .then(response => response.text())";
  html += "    .then(data => console.log(data))";
  html += "    .catch(error => console.error('Error:', error));";
  html += "}";
  
  // 全屏切換
  html += "function toggleFullscreen() {";
  html += "  var container = document.querySelector('.camera-container');";
  html += "  if (!document.fullscreenElement) {";
  html += "    if (container.requestFullscreen) {";
  html += "      container.requestFullscreen();";
  html += "    } else if (container.webkitRequestFullscreen) {";
  html += "      container.webkitRequestFullscreen();";
  html += "    } else if (container.msRequestFullscreen) {";
  html += "      container.msRequestFullscreen();";
  html += "    }";
  html += "  } else {";
  html += "    if (document.exitFullscreen) {";
  html += "      document.exitFullscreen();";
  html += "    } else if (document.webkitExitFullscreen) {";
  html += "      document.webkitExitFullscreen();";
  html += "    } else if (document.msExitFullscreen) {";
  html += "      document.msExitFullscreen();";
  html += "    }";
  html += "  }";
  html += "}";
  
  // 連接監控
  html += "var connectionLost = false;";
  html += "var reconnectAttempts = 0;";
  html += "var maxReconnectAttempts = 10;";
  html += "var statusMessage = document.getElementById('statusMessage');";
  html += "var streamImage = document.getElementById('streamImage');";
  
  html += "function checkConnection() {";
  html += "  fetch('/check?' + new Date().getTime())";
  html += "    .then(response => {";
  html += "      if (response.ok && connectionLost) {";
  html += "        connectionLost = false;";
  html += "        statusMessage.style.display = 'none';";
  html += "        reconnectAttempts = 0;";
  html += "        streamImage.src = '/stream?' + new Date().getTime();";
  html += "      }";
  html += "    })";
  html += "    .catch(error => {";
  html += "      if (!connectionLost) {";
  html += "        connectionLost = true;";
  html += "        statusMessage.style.display = 'block';";
  html += "        attemptReconnect();";
  html += "      }";
  html += "    });";
  html += "}";
  
  html += "function attemptReconnect() {";
  html += "  if (reconnectAttempts >= maxReconnectAttempts) {";
  html += "    statusMessage.textContent = '重新連接失敗，請刷新頁面重試';";
  html += "    return;";
  html += "  }";
  html += "  reconnectAttempts++;";
  html += "  statusMessage.textContent = '連接已中斷，正在嘗試重新連接... (' + reconnectAttempts + '/' + maxReconnectAttempts + ')';";
  html += "  setTimeout(function() {";
  html += "    streamImage.src = '/stream?' + new Date().getTime();";
  html += "    checkConnection();";
  html += "  }, 2000);";
  html += "}";
  
  html += "streamImage.onerror = function() {";
  html += "  if (!connectionLost) {";
  html += "    connectionLost = true;";
  html += "    statusMessage.style.display = 'block';";
  html += "    attemptReconnect();";
  html += "  }";
  html += "};";
  
  // 頁面加載時恢復設置
  html += "window.onload = function() {";
  html += "  var savedSize = localStorage.getItem('cameraSize');";
  html += "  if (savedSize) {";
  html += "    document.getElementById('imageSize').value = savedSize;";
  html += "    document.getElementById('sizeValue').textContent = savedSize + '%';";
  html += "    document.getElementById('streamImage').style.width = savedSize + '%';";
  html += "  }";
  html += "  setInterval(checkConnection, 5000);";
  html += "};";
  
  html += "</script>";
  html += "</body></html>";
  
  server.send(200, "text/html; charset=utf-8", html);
}

// 處理圖像捕獲請求
void handleCapture() {
  // 捕獲一幀圖像
  camera->oneFrame();
  
  // 將圖像數據複製到緩衝區
  memcpy(imageBuffer + BMP::headerSize, camera->frame, camera->xres * camera->yres * 2);
  
  // 發送圖像
  server.sendHeader("Content-Type", "image/bmp");
  server.sendHeader("Content-Length", String(imageSize));
  server.sendHeader("Content-Disposition", "attachment; filename=capture.bmp");
  server.send_P(200, "image/bmp", (const char*)imageBuffer, imageSize);
}

// 處理MJPEG流請求
void handleStream() {
  WiFiClient client = server.client();
  
  // 查找可用的客戶端插槽
  int clientSlot = -1;
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (!clientConnected[i]) {
      clientSlot = i;
      break;
    }
  }

  // 如果沒有可用插槽，返回服務器忙
  if (clientSlot == -1) {
    server.send(503, "text/plain; charset=utf-8", "Server Busy");
    return;
  }
  
  // 保存客戶端
  streamClients[clientSlot] = client;
  clientConnected[clientSlot] = true;
  
  // 發送HTTP頭部
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: multipart/x-mixed-replace; boundary=frame");
  client.println();
  
  // 客戶端處理將在loop()中進行
  
  // 防止WebServer關閉連接
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendContent("");
}

// 在loop()中處理MJPEG流
void streamMJPEG() {
  static unsigned long lastFrameTime = 0;
  unsigned long currentTime = millis();
  
  // 根據設定的幀率間隔發送幀
  if (currentTime - lastFrameTime >= frameInterval) {
    lastFrameTime = currentTime;
    
    // 捕獲一幀
    camera->oneFrame();
    
    // 將圖像數據複製到緩衝區
    memcpy(imageBuffer + BMP::headerSize, camera->frame, camera->xres * camera->yres * 2);
    
    // 向所有連接的客戶端發送圖像
    for (int i = 0; i < MAX_CLIENTS; i++) {
      if (clientConnected[i]) {
        WiFiClient client = streamClients[i];
        
        if (client.connected()) {
          // 發送多部分邊界
          client.println("--frame");
          client.println("Content-Type: image/bmp");
          client.println("Content-Length: " + String(imageSize));
          client.println();
          
          // 發送圖像數據
          client.write(imageBuffer, imageSize);
          client.println();
        } else {
          // 客戶端已斷開連接
          clientConnected[i] = false;
        }
      }
    }
  }
}

// 處理404錯誤
void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain; charset=utf-8", message);
}